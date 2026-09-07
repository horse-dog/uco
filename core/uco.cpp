#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#define _UCO_THREAD_ENV_IMPL
#include "uco.h"
#include "ulog.h"

#include <cerrno>
#include <cstring>
#include <liburing.h>
#include <semaphore.h>
#include <shared_mutex>
#include <sys/eventfd.h>
#include <unordered_map>

#define UCOENV           uco::__inner__::thread_co_env::GetInstance()
#define WAIT_SQE_LIST  ((uco::__inner__::uco_linked_list *)(UCOENV.wait_sqe_list))
#define YIELD_LIST     ((uco::__inner__::uco_linked_list *)(UCOENV.yield_list))
#define NODE2ADDR(node) (uco::task<void>::coro_handle::from_promise(*node).address())

#define REGISTER_READ_SYNCFD(retry)                                            \
    do                                                                         \
    {                                                                          \
        retry = 0;                                                             \
        auto sqe = get_sqe();                                                  \
        if (sqe == nullptr)                                                    \
        {                                                                      \
            retry = 1;                                                         \
            break;                                                             \
        }                                                                      \
        --io_event_count;                                                      \
        sqe->user_data = 1;                                                    \
        io_uring_prep_read(sqe, sync_fd, sync_buffer, sizeof(eventfd_t), 0);   \
    } while (0)

namespace uco
{
    extern std::mutex uthread_list_mtx;
    extern std::vector<std::thread*> uthreads_list;
}

extern void reigster_hook_before_tls_dtor(void (*hook)(void));

static void __schedule__()
{
    UCOENV.schedule();
}

namespace uco::__inner__
{
std::vector<uco_linked_list::uco_linked_node *>
uco_linked_list::take_all() noexcept
{
    std::vector<uco_linked_node *> result;
    while (!empty())
    {
        result.push_back(head);
        head = (uco_linked_node *)(head->next);
    }
    head = tail = nullptr;
    return result;
}

void uco_linked_list::push(uco_linked_node *node, bool lifo) noexcept
{
    if (!node)
        return;

    if (lifo)
    {
        node->next = head;
        head = node;
        if (!tail)
        {
            tail = node;
        }
    }
    else
    {
        node->next = nullptr;
        if (tail)
        {
            tail->next = node;
        }
        else
        {
            head = node;
        }
        tail = node;
    }
}

uco_linked_list::uco_linked_node *uco_linked_list::pop() noexcept
{
    if (!head)
    {
        return nullptr;
    }
    auto result = head;
    head = (uco_linked_node *)(head->next);
    if (!head)
    {
        tail = nullptr;
    }
    return result;
}

struct sync_linked_list
{
  public:
    void push(task<void>::promise_type *co)
    {
        while (list_head.compare_exchange_weak(
                   *(task<void>::promise_type **)(&(co->next)), co) == false)
            ;
    }

    task<void>::promise_type *pop()
    {
        task<void>::promise_type *p = list_head;
        while (p != nullptr &&
               list_head.compare_exchange_weak(
                   p, (task<void>::promise_type *)(p->next)) == false)
            ;
        return p;
    }

    bool empty() const noexcept
    {
        return list_head.load(std::memory_order_acquire) == nullptr;
    }

  private:
    std::atomic<task<void>::promise_type *> list_head = nullptr;
};

void suspend_always::await_suspend(std::coroutine_handle<> h) noexcept
{
    FRAMEWORK_DBG("await_suspend");
    auto ch = uco::task<void>::coro_handle::from_address(h.address());
    YIELD_LIST->push(&(ch.promise()));
}

#define FLAG_ACQUIRE_SQE_FAILED 1

struct io_uring_sqe *thread_co_env::get_sqe() noexcept
{
    auto sqe = io_uring_get_sqe((struct io_uring*)uring);
    if (sqe == nullptr)
    {
        SYSWRN("io_uring_get_sqe failed");
        flags |= FLAG_ACQUIRE_SQE_FAILED;
        return nullptr;
    }
    flags &= ~FLAG_ACQUIRE_SQE_FAILED;
    ++io_event_count;
    return sqe;
}

struct uco_context
{
    uco_context(int sync_fd, thread_co_env *env) : sync_fd(sync_fd), env(env) {}

    int sync_fd;
    thread_co_env *env;
    sync_linked_list sync_list;
};

std::shared_mutex context_mutex;
std::unordered_map<u64, uco_context> context_map;

static void resume(void *ptr)
{
    auto pcur = ptr;
    while (pcur != nullptr)
    {
        auto cur_co = uco::task<void>::coro_handle::from_address(pcur);
        auto caller = cur_co.promise().caller;
        FRAMEWORK_DBG("caller:", caller, ',', "current:", pcur);
        cur_co.resume();
        if (caller)
        {
            if (caller != cur_co.promise().caller)
            {
                FRAMEWORK_DBG("coroutine", pcur, "finished, caller:", caller);
                if (caller == (void *)1) [[unlikely]]
                {
                    SYSFTL(NR(pcur), "never yield and finish, why return here ? "
                           "(should return to await_ready)");
                }
                pcur = caller;
            }
            else
            {
                FRAMEWORK_DBG("cur_co not finish, leave:", pcur);
                pcur = nullptr;
            }
        }
        else
        {
            FRAMEWORK_DBG("no caller, leave:", pcur);
            pcur = nullptr;
        }
    }
}

thread_co_env::thread_co_env()
{
    thread_id = gettid();
    FRAMEWORK_DBG("construct thread_co_env:", thread_id);
    sync_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    uring = new struct io_uring;
    if (io_uring_queue_init(8192, (struct io_uring*)uring, 0) < 0)
    {
        SYSERR("io_uring_queue_init:", strerror(errno));
        _exit(1);
    }

    yield_list = new uco_linked_list();
    wait_sqe_list = new uco_linked_list();
    timer_heap = new std::vector<__inner__::timer_node *>();
    timer_ts = new struct __kernel_timespec;

    context_mutex.lock();
    auto [it, ok] = context_map.try_emplace(thread_id, sync_fd, this);
    if (!ok)
    {
        SYSERR("context_map try_emplace failed");
        _exit(1);
    }
    sync_list = &(it->second.sync_list);
    context_mutex.unlock();

    int retry = 0;
    sync_buffer = new eventfd_t(0);
    REGISTER_READ_SYNCFD(retry);
    if (retry)
    {
        errno = EMFILE;
        SYSERR("REGISTER_READ_SYNCFD:", strerror(errno));
        _exit(1);
    }

    io_uring_submit((struct io_uring*)uring);
    reigster_hook_before_tls_dtor(__schedule__);
}

thread_co_env::~thread_co_env()
{
    uco_linked_list* pYieldList = (uco_linked_list *)yield_list;
    if (pYieldList) delete pYieldList;
    yield_list = nullptr;

    uco_linked_list* pWaitSqeList = (uco_linked_list *)wait_sqe_list;
    if (pWaitSqeList) delete pWaitSqeList;
    wait_sqe_list = nullptr;

    auto *pTimerHeap = (std::vector<__inner__::timer_node *> *)timer_heap;
    if (pTimerHeap) delete pTimerHeap;
    timer_heap = nullptr;

    auto *pTimerTs = (struct __kernel_timespec *)timer_ts;
    if (pTimerTs) delete pTimerTs;
    timer_ts = nullptr;

    struct io_uring* pUring = (struct io_uring*)uring;
    if (pUring) delete pUring;
    uring = nullptr;

    eventfd_t* pSyncBuffer = (eventfd_t*)sync_buffer;
    if (pSyncBuffer) delete pSyncBuffer;
    sync_buffer = nullptr;
}

void thread_co_env::schedule()
{
    FRAMEWORK_DBG("shceduler start");
    int retry_register_read_syncfd = 0;
    auto yield_co_list = (uco_linked_list *)yield_list;
    auto sqe_co_list = (uco_linked_list *)wait_sqe_list;
    auto sync_co_list = (sync_linked_list *)sync_list;
    while (true)
    {
        if (io_event_count == 0 && sync_event_count == 0 &&
            yield_co_list->empty() && sqe_co_list->empty() &&
            sync_co_list->empty())
        {
            if (timer_active())
            { // 堆非空或超时请求未决: 此刻提交队列必有余位,
                // 补交/取消超时请求, 再继续循环等完成事件.
                timer_refresh();
            }
            else
            {
                FRAMEWORK_DBG("shceduler exit");
                break;
            }
        }

        // process sync list coroutines.
        while (true)
        {
            auto co = sync_co_list->pop();
            if (co == nullptr)
            {
                break;
            }
            if (co->tid != thread_id) [[unlikely]]
            {
                SYSFTL("co->tid and thread_id mismatch", NR(co->tid), NR(thread_id));
            }
            resume(NODE2ADDR(co));
        }

        // process yield coroutines.
        while (!yield_co_list->empty())
        {
            auto co = yield_co_list->pop();
            resume(NODE2ADDR(co));
        }

        // 提交队列曾满导致超时请求没交上, 重试.
        if (timer_arm_pending)
        {
            timer_refresh();
        }

        // register read sync fd if failed.
        if (retry_register_read_syncfd)
        {
            SYSWRN("retry_register_read_syncfd");
            REGISTER_READ_SYNCFD(retry_register_read_syncfd);
        }

        // process io_uring events.
        if (io_event_count > 0)
        {
            io_uring_submit_and_wait((struct io_uring*)uring, 1);
            struct io_uring_cqe *cqe;
            unsigned head;
            unsigned count = 0;
            io_uring_for_each_cqe((struct io_uring*)uring, head, cqe)
            {
                ++count;
                --io_event_count;
                if (cqe->user_data == 0)
                {
                    FRAMEWORK_DBG("operate canceled");
                    continue;
                }
                if (cqe->user_data == 1)
                {
                    FRAMEWORK_DBG("sync fd notify:", NR(thread_id), NR(sync_fd));
                    REGISTER_READ_SYNCFD(retry_register_read_syncfd);
                    ++io_event_count;
                    continue;
                }
                if ((cqe->user_data & 0xff) == 2)
                { // 定时器超时请求完成 (到期或被取消): 幂等处理.
                    --timer_inflight;
                    timer_on_fire();
                    continue;
                }
                auto ptr = (void *)cqe->user_data;
                auto &&handle = task<void>::coro_handle::from_address(ptr);
                handle.promise().set_value(cqe->res);
                resume(ptr);
            }
            io_uring_cq_advance((struct io_uring*)uring, count);
        }

        // process sqe list coroutines.
        while (!sqe_co_list->empty())
        {
            auto co = sqe_co_list->pop();
            resume(NODE2ADDR(co));
            if (flags & FLAG_ACQUIRE_SQE_FAILED)
            {
                break;
            }
        }
    }
}

// ==================== per-thread timer infra (usleeper) ====================
// user_data 布局: (序号 << 8) | 2. 协程句柄 8 字节对齐 (低 3 位为 0),
// 永不与标记值 2 冲突; 0/1 为调度器保留值. 序号逐次递增, 使每次提交的
// 超时请求可被精确定位和取消.

int push_sync_node_to_thread(uco::task<void>::promise_type *node);

static i64 mono_now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (i64)ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
}

bool thread_co_env::timer_active()
{
    auto &heap = *(std::vector<__inner__::timer_node *> *)timer_heap;
    return !heap.empty() || timer_inflight > 0 || timer_arm_pending;
}

/// 入堆 (仅睡眠线程调用), 维护 min-heap 与 idx.
void thread_co_env::timer_push(__inner__::timer_node *node)
{
    auto &heap = *(std::vector<__inner__::timer_node *> *)timer_heap;
    if (node->in_heap) [[unlikely]]
    { // 不变量检查: 占位 CAS 成功意味着槽位曾为 IDLE, 即上一个睡眠
      // 已清理 (in_heap 必为 false). 正常流程不可达, 仅防实现回归.
        SYSFTL("usleeper: invariant broken, push a node already in heap");
    }
    node->in_heap = true;
    heap.push_back(node);
    u64 i = heap.size() - 1;
    while (i > 0)
    { // sift up.
        u64 parent = (i - 1) / 2;
        if (heap[parent]->deadline_ns <= heap[i]->deadline_ns)
        {
            break;
        }
        std::swap(heap[parent], heap[i]);
        heap[parent]->idx = parent;
        heap[i]->idx = i;
        i = parent;
    }
    node->idx = i;
    timer_refresh();
}

/// 移出堆 (仅睡眠线程调用: 睡眠协程被唤醒后自行清理).
void thread_co_env::timer_remove(__inner__::timer_node *node)
{
    auto &heap = *(std::vector<__inner__::timer_node *> *)timer_heap;
    if (!node->in_heap)
    {
        return; // 已被 timer_on_fire 弹出.
    }
    node->in_heap = false;
    u64 i = node->idx;
    heap[i] = heap.back();
    heap[i]->idx = i;
    heap.pop_back();
    if (i < heap.size())
    { // sift down + up (被换上来的尾元素可能两个方向都要走).
        while (true)
        {
            u64 left = 2 * i + 1, right = left + 1, best = i;
            if (left < heap.size() &&
                heap[left]->deadline_ns < heap[best]->deadline_ns)
            {
                best = left;
            }
            if (right < heap.size() &&
                heap[right]->deadline_ns < heap[best]->deadline_ns)
            {
                best = right;
            }
            if (best == i)
            {
                break;
            }
            std::swap(heap[best], heap[i]);
            heap[best]->idx = best;
            heap[i]->idx = i;
            i = best;
        }
        while (i > 0)
        {
            u64 parent = (i - 1) / 2;
            if (heap[parent]->deadline_ns <= heap[i]->deadline_ns)
            {
                break;
            }
            std::swap(heap[parent], heap[i]);
            heap[parent]->idx = parent;
            heap[i]->idx = i;
            i = parent;
        }
    }
    timer_refresh();
}

/// 超时请求的完成事件到达 (到期, 或被取消): 唤醒所有已到期的睡眠.
/// 幂等: 过期的完成事件 (取消与到期同时发生) 唤醒不了任何人,
/// 只是多触发一次超时请求的更新, 无害.
void thread_co_env::timer_on_fire()
{
    auto &heap = *(std::vector<__inner__::timer_node *> *)timer_heap;
    i64 now = mono_now_ns();
    while (!heap.empty() && heap.front()->deadline_ns <= now)
    {
        auto n = heap.front();
        heap[0] = heap.back();
        heap[0]->idx = 0;
        heap.pop_back();
        n->in_heap = false;
        size_t i = 0; // 恢复堆序 (sift down).
        while (true)
        {
            size_t left = 2 * i + 1, right = left + 1, best = i;
            if (left < heap.size() &&
                heap[left]->deadline_ns < heap[best]->deadline_ns)
            {
                best = left;
            }
            if (right < heap.size() &&
                heap[right]->deadline_ns < heap[best]->deadline_ns)
            {
                best = right;
            }
            if (best == i)
            {
                break;
            }
            std::swap(heap[best], heap[i]);
            heap[best]->idx = best;
            heap[i]->idx = i;
            i = best;
        }
        int expect = __inner__::timer_node::PARKED;
        if (n->st.compare_exchange_strong(
                expect, __inner__::timer_node::EXPIRED))
        { // 赢得仲裁, 负责唯一的唤醒. 此刻正在本线程处理完成事件
          // (调度器调用栈上), 直接恢复协程执行即可 (与框架处理其他
          // 完成事件的方式一致); 不能投 sync_list 稍后处理, 否则
          // 线程收尾检查先于队列消费执行, 睡眠协程会被遗弃.
            resume(NODE2ADDR(n->prom));
        } // 输者 (已被 wake 抢先唤醒): 由 wake 负责恢复, 无需处理.
    }
    timer_refresh();
}

/// 维护"针对堆顶时刻的超时请求": 需要时取消旧请求并提交新请求.
/// 幂等, 可从任意路径调用 (睡眠协程 / 完成事件处理 / 线程收尾检查).
void thread_co_env::timer_refresh()
{
    auto &heap = *(std::vector<__inner__::timer_node *> *)timer_heap;
    if (heap.empty())
    { // 没有睡眠者: 取消未决的超时请求, 让线程能退出调度循环.
        if (timer_inflight > 0)
        {
            auto sqe = get_sqe();
            if (sqe == nullptr)
            { // 提交队列满: 记下待重试, 由调度循环兜底
                // (旧请求即使睡满自然完成, 处理也是幂等的).
                timer_arm_pending = true;
                return;
            }
            io_uring_prep_cancel64(sqe, timer_armed_user_data, 0);
            sqe->user_data = 0; // 取消请求自身的完成事件无需处理.
        }
        timer_arm_pending = false;
        return;
    }
    i64 top = heap.front()->deadline_ns;
    if (timer_inflight > 0 && top >= timer_armed_deadline &&
        !timer_arm_pending)
    { // 未决请求的目标时刻不晚于堆顶, 无需更新.
        return;
    }
    if (timer_inflight > 0)
    { // 堆顶时刻更早: 取消旧请求. 取消落空 (旧请求恰好已完成) 也无妨,
        // 完成事件的处理是幂等的.
        auto sqe = get_sqe();
        if (sqe != nullptr)
        {
            io_uring_prep_cancel64(sqe, timer_armed_user_data, 0);
            sqe->user_data = 0;
        }
    }
    auto sqe = get_sqe();
    if (sqe == nullptr)
    { // 提交队列满: 记下待重试. 睡眠者已挂起, 不依赖请求提交成功.
        SYSWRN("timer arm deferred, sqe exhausted");
        timer_arm_pending = true;
        return;
    }
    i64 remain = top - mono_now_ns();
    if (remain < 0)
    {
        remain = 0; // 堆顶已过期: 零时长使请求立即完成.
    }
    // 时长缓冲区须存活到内核提交; 本线程对超时请求的提交是串行的,
    // 单缓冲足够, 连续覆盖最多造成一次多余的幂等处理.
    auto ts = (struct __kernel_timespec *)timer_ts;
    ts->tv_sec = remain / 1'000'000'000LL;
    ts->tv_nsec = remain % 1'000'000'000LL;
    u64 ud = (++timer_gen << 8) | 2;
    io_uring_prep_timeout(sqe, ts, 0, 0);
    sqe->user_data = ud;
    timer_armed_user_data = ud;
    timer_armed_deadline = top;
    ++timer_inflight;
    timer_arm_pending = false;
}

int push_sync_node_to_thread(uco::task<void>::promise_type *node)
{
    __inner__::uco_context *context = nullptr;
    auto tid = node->tid;
    int syncfd = 0;

    context_mutex.lock_shared();
    auto it = context_map.find(tid);
    if (it == context_map.end()) [[unlikely]]
    {
        SYSFTL("sync_fd_map find failed..., tid:", tid);
    }
    context = &(it->second);
    syncfd = context->sync_fd;
    context_mutex.unlock_shared();

    context->sync_list.push(node);

    if (tid == UCOENV.thread_id)
    {
        return 0;
    }
    return syncfd;
}

struct __uthread_guard
{
    ~__uthread_guard()
    {
        while (true)
        {
            std::vector<std::thread*> tmp;
            uthread_list_mtx.lock();
            tmp.swap(uthreads_list);
            uthread_list_mtx.unlock();
            if (tmp.empty()) break;
            for (auto pthread : tmp)
            {
                auto id = pthread->get_id();
                FRAMEWORK_DBG("join thead:", id, "begin");
                if (pthread->joinable())
                {
                    pthread->join();
                }
                delete pthread;
                FRAMEWORK_DBG("join thead:", id, "finish");
            }
        }
    }
};

struct uthread_guard
{
    uthread_guard()
    {
        thread_local __uthread_guard guard;
        (void)guard;
    }
};

static uthread_guard __guard;

} // namespace uco::__inner__
