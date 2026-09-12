#define _UCO_THREAD_ENV_IMPL
#include <thread>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <coroutine>
#include <chrono>
#include <cstdlib>
#include <emmintrin.h>
#include <liburing.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <unordered_set>

#include "core/uco.h"
#include "core/ulog.h"
#include "core/usync.h"

namespace uco
{
namespace __inner__
{
extern int push_sync_node_to_thread(task<void>::promise_type *co);
}

struct get_promise_addr_t
{
    task<void>::promise_type *prom_ptr = nullptr;
    auto await_ready() { return false; }
    auto await_suspend(task<void>::coro_handle h)
    {
        this->prom_ptr = std::addressof(h.promise());
        return false;
    }
    auto await_resume() { return prom_ptr; }
};

struct sync_awaitable
{
    bool await_ready() { return false; }
    bool await_suspend(std::coroutine_handle<> h) { return true; }
    void await_resume() {}
};

class spin_lock
{
  public:
    spin_lock() { pthread_spin_init(&spinlock, 0); }
    ~spin_lock() { pthread_spin_destroy(&spinlock); }
    spin_lock(const spin_lock &) = delete;
    spin_lock &operator=(const spin_lock &) = delete;
    spin_lock(spin_lock &&) = delete;
    spin_lock &operator=(spin_lock &&) = delete;
    void lock() { pthread_spin_lock(&spinlock); }
    void unlock() { pthread_spin_unlock(&spinlock); }
    bool try_lock() { return pthread_spin_trylock(&spinlock) == 0; }

  private:
    pthread_spinlock_t spinlock;
};

#define UCOENV uco::__inner__::thread_co_env::GetInstance()
#define LOCK ((spin_lock *)pLock)
#define PUSH2(Queue)                                                           \
    do                                                                         \
    {                                                                          \
        auto pPromise = co_await get_promise_addr_t();                         \
        pPromise->tid = UCOENV.thread_id;                                      \
        pPromise->next = nullptr;                                              \
        Queue->push(pPromise);                                                 \
    } while (0)

#define POP(Queue)                                                             \
    do                                                                         \
    {                                                                          \
        auto co = Queue->pop();                                                \
        LOCK->unlock();                                                        \
        if (co == nullptr)                                                     \
        {                                                                      \
            SYSERR("should not be nullptr");                                   \
            return;                                                            \
        }                                                                      \
        int fd = __inner__::push_sync_node_to_thread(co);                      \
        eventfd_t writemsg = 1;                                                \
        if (fd != 0)                                                           \
        {                                                                      \
            auto ret = write(fd, &writemsg, sizeof(writemsg));                 \
            if (ret < 0)                                                       \
            {                                                                  \
                SYSERR("eventfd", fd, "full");                                 \
            }                                                                  \
        }                                                                      \
    } while (0)

#define YIELD_RETRY                                                            \
    ++UCOENV.sync_event_count;                                                 \
    co_await sync_awaitable();                                                 \
    --UCOENV.sync_event_count;                                                 \
    goto retry;                                                                \
    co_return;

#define CPU_RELAX                                                              \
    if (i != MAX_SPIN - 1)                                                     \
    {                                                                          \
        LOCK->unlock();                                                        \
        int pause_count = i < 10 ? 100 : 500;                                  \
        for (int j = 0; j < pause_count; j++)                                  \
        {                                                                      \
            _mm_pause();                                                       \
        }                                                                      \
    }

const static int MAX_SPIN = 32;

umutex::umutex() : iState(0) { pSema = new usema(0); }

umutex::~umutex()
{
    iState.store(0);
    if (pSema)
        delete (usema *)(pSema);
    pSema = nullptr;
}

const static int mutexLocked = 0b0001;
const static int mutexWoken = 0b0010;
const static int mutexStarving = 0b0100;
const static int mutexWaiterShift = 3;

#define runtime_canSpin(iter) (iter < MAX_SPIN)
#define rumtime_doSpin()                                                       \
    for (int i = 0; i < 100; i++)                                              \
    _mm_pause()

task<void> umutex::lock()
{
    int expect = 0;
    if (iState.compare_exchange_strong(expect, 1))
    {
        co_return;
    }

    std::chrono::steady_clock::time_point waitStartTime;
    bool starving = false;
    bool awoke = false;
    int iter = 0;
    int old = iState.load();
    while (true)
    {
        // old is mutexLocked and not mutexStarving, and can spin.
        if ((old & (mutexLocked | mutexStarving)) == mutexLocked &&
            runtime_canSpin(iter))
        {
            if (!awoke && (old & mutexWoken) == 0 &&
                (old >> mutexWaiterShift) != 0 &&
                iState.compare_exchange_strong(old, old | mutexWoken))
            {
                awoke = true;
            }
            rumtime_doSpin();
            ++iter;
            old = iState.load();
            continue;
        }
        int newstate = old;
        if ((old & mutexStarving) == 0)
        {
            // only not starving, set mutexLocked.
            newstate |= mutexLocked;
        }
        if ((old & (mutexLocked | mutexStarving)) != 0)
        {
            newstate += (1 << mutexWaiterShift);
        }
        if (starving && ((old & mutexLocked) != 0))
        {
            newstate |= mutexStarving;
        }
        if (awoke)
        {
            if ((newstate & mutexWoken) == 0)
            {
                SYSFTL("sync: inconsistent mutex state");
            }
            newstate &= ~mutexWoken;
        }
        if (iState.compare_exchange_strong(old, newstate))
        {
            if ((old & (mutexLocked | mutexStarving)) == 0)
            {
                break;
            }
            bool queueLifo = (waitStartTime.time_since_epoch().count() != 0);
            if (waitStartTime.time_since_epoch().count() == 0)
            {
                waitStartTime = std::chrono::steady_clock::now();
            }
            co_await ((usema *)(pSema))->wait(queueLifo);
            auto now = std::chrono::steady_clock::now();
            auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           now - waitStartTime)
                           .count();
            starving = starving || (dur > 1'000'000); // 1ms.
            old = iState.load();
            if ((old & mutexStarving) != 0)
            {
                if ((old & (mutexLocked | mutexWoken)) != 0 ||
                    (old >> mutexWaiterShift) == 0)
                {
                    SYSFTL("sync: inconsistent mutex state");
                }
                int delta = mutexLocked - (1 << mutexWaiterShift);
                if (!starving || (old >> mutexWaiterShift) == 1)
                {
                    delta -= mutexStarving;
                }
                iState += delta;
                break;
            }
            awoke = true;
            iter = 0;
        }
        else
        {
            old = iState.load();
        }
    }
    co_return;
}

bool umutex::try_lock()
{
    int expect = 0;
    if (iState.compare_exchange_strong(expect, 1))
    {
        return true;
    }
    return false;
}

void umutex::unlock()
{
    int old = iState.fetch_sub(mutexLocked);
    if (old == mutexLocked)
        return;

    int newstate = old - mutexLocked;
    if ((old & mutexLocked) == 0)
    {
        SYSFTL("sync: unlock of unlocked mutex");
    }
    if ((newstate & mutexStarving) == 0)
    {
        old = newstate;
        while (true)
        {
            if ((old >> mutexWaiterShift) == 0 ||
                (old & (mutexLocked | mutexWoken | mutexStarving)) != 0)
            {
                return;
            }
            newstate = (old - (1 << mutexWaiterShift)) | mutexWoken;
            if (iState.compare_exchange_strong(old, newstate))
            {
                ((usema *)(pSema))->signal();
                return;
            }
            old = iState.load();
        }
    }
    else
    {
        ((usema *)(pSema))->signal();
    }
}

#define RQ ((__inner__::uco_linked_list *)pRwaiter)
#define WQ ((__inner__::uco_linked_list *)pWwaiter)

#ifndef USE_NEW_SHARED_MUTEX_IMPL
ushared_mutex::ushared_mutex() : state(0)
{
    pRwaiter = new __inner__::uco_linked_list();
    pWwaiter = new __inner__::uco_linked_list();
    pLock = new spin_lock();
}

ushared_mutex::~ushared_mutex()
{
    if (pRwaiter)
        delete RQ;
    pRwaiter = nullptr;
    if (pWwaiter)
        delete WQ;
    pWwaiter = nullptr;
    if (pLock)
        delete LOCK;
    pLock = nullptr;
    state = 0;
}

task<void> ushared_mutex::lock()
{
retry:
    for (int i = 0; i < MAX_SPIN; i++)
    {
        LOCK->lock();
        if (state == 0)
        {
            state = -1;
            LOCK->unlock();
            co_return;
        }
        CPU_RELAX;
    }
    PUSH2(WQ);
    LOCK->unlock();
    YIELD_RETRY;
}

bool ushared_mutex::try_lock()
{
    LOCK->lock();
    if (state == 0)
    {
        state = -1;
        LOCK->unlock();
        return true;
    }
    LOCK->unlock();
    return false;
}

void ushared_mutex::unlock()
{
    LOCK->lock();
    assert(state == -1);
    state = 0;
    if (WQ->empty() && RQ->empty())
    {
        LOCK->unlock();
        return;
    }

    if (!WQ->empty())
    {
        POP(WQ);
        return;
    }
    if (!RQ->empty())
    {
        auto readers = RQ->take_all();
        LOCK->unlock();
        eventfd_t writemsg = 1;
        std::unordered_set<int> fds;
        for (auto reader : readers)
        {
            int fd = __inner__::push_sync_node_to_thread(reader);
            if (fd != 0)
                fds.insert(fd);
        }

        for (int fd : fds)
        {
            auto ret = write(fd, &writemsg, sizeof(writemsg));
            if (ret < 0)
            {
                SYSERR("eventfd", fd, "full");
            }
        }
    }
}

task<void> ushared_mutex::lock_shared()
{
retry:
    for (int i = 0; i < MAX_SPIN; i++)
    {
        LOCK->lock();
        if (state >= 0 && WQ->empty())
        {
            ++state;
            LOCK->unlock();
            co_return;
        }
        CPU_RELAX;
    }
    PUSH2(RQ);
    LOCK->unlock();
    YIELD_RETRY;
}

bool ushared_mutex::try_lock_shared()
{
    LOCK->lock();
    if (state >= 0 && WQ->empty())
    {
        ++state;
        LOCK->unlock();
        return true;
    }
    LOCK->unlock();
    return false;
}

void ushared_mutex::unlock_shared()
{
    LOCK->lock();
    if (state > 0)
        --state;
    else
        SYSERR("err");
    if (state == 0)
    {
        if (WQ->empty())
        {
            LOCK->unlock();
            return;
        }
        else
        {
            POP(WQ);
            return;
        }
    }
    LOCK->unlock();
}
#else
ushared_mutex::ushared_mutex() : readers_(0), reader_wait_(0)
{
    reader_sema_ = new usema(0);
    writer_sema_ = new usema(0);
}

ushared_mutex::~ushared_mutex()
{
    delete (usema *)reader_sema_;
    reader_sema_ = nullptr;
    delete (usema *)writer_sema_;
    writer_sema_ = nullptr;
}

task<void> ushared_mutex::lock()
{
    // 1. 写者间排队 (含饥饿模式, 同 umutex).
    co_await w_.lock();

    // 2. 挂"写者已登记"牌 (readers_ 转负): 之后到达的读者入 reader_sema
    //    排队. r = 挂牌瞬间的存量读者数.
    const int r = readers_.fetch_add(-kMaxReaders);
    if (r != 0 && reader_wait_.fetch_add(r) != -r)
    { // 3. 等存量读者清场: 最后一个离开的读者 signal.
        //    fetch_add(r) != -r: 并发 unlock_shared 先减一拍时计数仍闭合
        //    (提前清零即无需再等, 见 Go readerWait 同款仲裁).
        co_await ((usema *)writer_sema_)->wait();
    }
}

bool ushared_mutex::try_lock()
{
    if (!w_.try_lock())
    {
        return false;
    }
    // 仅在恰好零读者时原子挂牌: 不产生"短暂负值"窗口
    // (窗口内到达的读者会错入 reader_sema 队列且无人及时唤醒).
    int expect = 0;
    if (!readers_.compare_exchange_strong(expect, -kMaxReaders))
    {
        w_.unlock();
        return false;
    }
    return true;
}

void ushared_mutex::unlock()
{
    // 1. 收牌; old < 0, |old + kMaxReaders| = 登记期间排队的读者数.
    const int old = readers_.fetch_add(kMaxReaders);
    if (old >= 0)
    {
        SYSFTL("sync: unlock of unlocked shared mutex");
    }

    // 2. 批量放行排队读者 (醒来即持读锁, 新读者此后走快路径).
    const int queued = old + kMaxReaders;
    for (int i = 0; i < queued; i++)
    {
        ((usema *)reader_sema_)->signal();
    }

    // 3. 放行下一个写者.
    w_.unlock();
}

task<void> ushared_mutex::lock_shared()
{
    // 写者已登记 (结果为负): 入 reader_sema 排队, 由 unlock() 放行;
    // 否则单次原子加即持锁, 不挂起.
    if (readers_.fetch_add(1) < 0)
    {
        co_await ((usema *)reader_sema_)->wait();
    }
}

bool ushared_mutex::try_lock_shared()
{
    int c = readers_.load();
    while (c >= 0)
    {
        if (readers_.compare_exchange_weak(c, c + 1))
        {
            return true;
        }
    }
    return false; // 写者已登记.
}

void ushared_mutex::unlock_shared()
{
    const int old = readers_.fetch_sub(1);
    if (old > 0)
    {
        return; // 无写者: 纯减活跃读者.
    }
    if (old == 0)
    {
        SYSFTL("sync: unlock_shared of unlocked shared mutex");
    }
    // 写者已登记 (old < 0): 存量读者倒计数, 归零者放行写者.
    // 排队读者的 unlock_shared 必在写者收牌之后 (readers_ 已非负),
    // 不会误减 reader_wait_.
    if (reader_wait_.fetch_sub(1) == 1)
    {
        ((usema *)writer_sema_)->signal();
    }
}
#endif

#undef RQ
#undef WQ
#define WQ ((__inner__::uco_linked_list *)pWaiter)

ucond::ucond()
{
    pWaiter = new __inner__::uco_linked_list();
    pLock = new spin_lock();
}

ucond::~ucond()
{
    if (pWaiter)
        delete WQ;
    pWaiter = nullptr;
    if (pLock)
        delete LOCK;
    pLock = nullptr;
}

task<void> ucond::wait(unique_ulock_t<umutex> &lock)
{
    LOCK->lock();
    PUSH2(WQ);
    LOCK->unlock();
    lock.unlock();
    ++UCOENV.sync_event_count;
    co_await sync_awaitable();
    --UCOENV.sync_event_count;
    co_await lock.lock();
}

void ucond::notify_one()
{
    LOCK->lock();
    if (WQ->empty())
    {
        LOCK->unlock();
        return;
    }
    POP(WQ);
}

void ucond::notify_all()
{
    LOCK->lock();
    if (WQ->empty())
    {
        LOCK->unlock();
        return;
    }
    auto waiters = WQ->take_all();
    LOCK->unlock();
    eventfd_t writemsg = 1;
    std::unordered_set<int> fds;
    for (auto waiter : waiters)
    {
        int fd = __inner__::push_sync_node_to_thread(waiter);
        if (fd != 0)
            fds.insert(fd);
    }

    for (int fd : fds)
    {
        auto ret = write(fd, &writemsg, sizeof(writemsg));
        if (ret < 0)
        {
            SYSERR("eventfd", fd, "full");
        }
    }
}

usema::usema(size_t cnt) : count(cnt)
{
    pLock = new spin_lock();
    pWaiter = new __inner__::uco_linked_list();
}

usema::~usema()
{
    if (pLock)
        delete LOCK;
    pLock = nullptr;
    if (pWaiter)
        delete WQ;
    pWaiter = nullptr;
    nwait.store(0);
    count.store(0);
}

task<void> usema::wait(bool lifo)
{
    auto pPromise = co_await get_promise_addr_t();
    pPromise->tid = UCOENV.thread_id;
    pPromise->next = nullptr;

retry:
    for (int i = 0; i < MAX_SPIN; i++)
    {
        if (try_wait())
        {
            co_return;
        }
        int pause_count = i < 10 ? 100 : 500;
        for (int j = 0; j < pause_count; j++)
        {
            _mm_pause();
        }
    }

    LOCK->lock();
    if (pWaiter == nullptr) [[unlikely]]
    {
        LOCK->unlock();
        co_return;
    }
    ++nwait;
    if (try_wait())
    {
        --nwait;
        LOCK->unlock();
        co_return;
    }
    WQ->push(pPromise, lifo);
    LOCK->unlock();
    ++UCOENV.sync_event_count;
    co_await sync_awaitable();
    --UCOENV.sync_event_count;
    goto retry;
}

bool usema::try_wait()
{
    while (true)
    {
        auto cur = count.load();
        if (cur == 0)
        {
            return false;
        }
        if (count.compare_exchange_strong(cur, cur - 1))
        {
            return true;
        }
    }
}

void usema::signal()
{
    ++count;
    if (nwait.load() == 0)
    {
        return;
    }

    LOCK->lock();
    if (nwait.load() == 0)
    {
        LOCK->unlock();
        return;
    }
    if (WQ->empty())
    {
        LOCK->unlock();
        return;
    }
    auto co = WQ->pop();
    if (co != nullptr)
    {
        --nwait;
    }
    LOCK->unlock();
    if (co == nullptr)
    {
        SYSERR("should not be nullptr");
        return;
    }
    int fd = __inner__::push_sync_node_to_thread(co);
    eventfd_t writemsg = 1;
    if (fd != 0)
    {
        auto ret = write(fd, &writemsg, sizeof(writemsg));
        if (ret < 0)
        {
            SYSERR("eventfd", fd, "full");
        }
    }
}

void usema::__kill_broadcast()
{
    LOCK->lock();
    auto wq = WQ;
    if (wq == nullptr)
    {
        SYSERR("double usema kill.");
        std::__terminate();
    }
    pWaiter = nullptr;
    nwait.store(0);
    auto waiters = wq->take_all();
    LOCK->unlock();
    delete wq;

    eventfd_t writemsg = 1;
    std::unordered_set<int> fds;
    for (auto waiter : waiters)
    {
        int fd = __inner__::push_sync_node_to_thread(waiter);
        if (fd != 0)
            fds.insert(fd);
    }

    for (int fd : fds)
    {
        auto ret = write(fd, &writemsg, sizeof(writemsg));
        if (ret < 0)
        {
            SYSERR("eventfd %d full", fd);
        }
    }
}

thread_local bool is_do_batching = false;

cobatch::cobatch(int concurrent)
: concurrent_(concurrent)
{
    done_ = new uco::usema(0);
}

cobatch::~cobatch()
{
    uco::usema* pDone = (uco::usema*)done_;
    if (pDone != nullptr) delete pDone;
    done_ = nullptr;
    concurrent_ = 0;
    tasks_.clear();
}

void cobatch::add(task<void> co)
{
    tasks_.emplace_back(co);
}

static uco::task<void> run_one(uco::task<void> co, uco::usema* pSlot, uco::usema* pDone)
{
    try { co_await co; }
    catch (std::exception& e) {
        SYSERR("co exception:", e.what());
    }
    pSlot->signal();
    pDone->signal();
    co_return;
}

// start go 出的调度协程: 嵌套批次串行执行; 首层占槽提交并等待全部完成.
// slot/done 为局部信号量: dispatch 收齐 done 前不会析构, run_one 无 UAF
// (usema::signal 只入队+eventfd 通知, 不做协程内切换).
static uco::task<void> dispatch(std::vector<task<void>> tasks, int concurrent,
                                uco::usema* pDone)
{
    if (is_do_batching)
    { // 嵌套批次: 串行执行, 防打乱外层计数.
        for (auto&& co : tasks)
        {
            try { co_await co; }
            catch (std::exception& e) {
                SYSERR("co exception:", e.what());
            }
        }
        pDone->signal();
        co_return;
    }
    uco::usema slot(std::min(concurrent, 50)); // 并发槽, 生命周期=批次.
    uco::usema task_done(0);                   // 任务完成计数.
    is_do_batching = true;
    for (auto&& co : tasks)
    {
        co_await slot.wait();
        go run_one(co, &slot, &task_done);
    }
    for (size_t i = 0; i < tasks.size(); i++)
    {
        co_await task_done.wait();
    }
    is_do_batching = false;
    pDone->signal();
    co_return;
}

void cobatch::start()
{
    if (tasks_.empty()) return; // 无任务无批次.
    if (in_flight_) LOGFTL("cobatch::start: previous batch not waited");
    in_flight_ = true;
    go dispatch(std::move(tasks_), concurrent_, (uco::usema*)done_);
    tasks_.clear();
}

uco::task<void> cobatch::wait()
{
    if (!in_flight_) co_return; // 未 start 或已 wait.
    in_flight_ = false;
    co_await ((uco::usema*)done_)->wait();
    co_return;
}

uco::task<void> cobatch::run()
{
    start();
    co_await wait();
}

std::mutex uthread_list_mtx;
std::vector<std::thread*> uthreads_list;

void uthread::join()
{
    if (_Mp_thread != nullptr)
    {
        _Mp_thread->join();
        delete _Mp_thread;
        _Mp_thread = nullptr;
    }
}

void uthread::daemonize()
{
    uthread_list_mtx.lock();
    uthreads_list.push_back(_Mp_thread);
    uthread_list_mtx.unlock();
    _Mp_thread = nullptr;
}

// ==================== usleeper (框架级定时器, 跨线程可唤醒) ====================

using __inner__::timer_node;

/// CLOCK_MONOTONIC 纳秒 (与超时请求使用的时钟一致).
static int64_t usleeper_mono_now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
}

/// 状态名 (调试日志用).
[[maybe_unused]] static const char *timer_state_name(int s)
{
    switch (s)
    {
    case timer_node::IDLE:       return "IDLE";
    case timer_node::REGISTERING:return "REGISTERING";
    case timer_node::PARKED:     return "PARKED";
    case timer_node::WOKEN:      return "WOKEN";
    case timer_node::EXPIRED:    return "EXPIRED";
    default:                     return "?";
    }
}

/**
 * @brief 内层睡眠协程: 原子占位 -> 登记 -> 入本线程定时器堆 -> 挂起.
 * @note 占位 (CAS IDLE -> REGISTERING) 先于任何字段写入: 槽位被占
 *       (登记中/挂起中/已仲裁未清理) 时立即拒绝, 共享状态零污染,
 *       先睡者不受影响. 睡眠本身不占用 io_uring (超时请求由
 *       timer_refresh 统一提交). 唤醒由 wake()/自然到期经原子状态
 *       仲裁后投递, 恢复时 st 已是 WOKEN/EXPIRED.
 */
task<usleeper::result> usleeper_raw_sleep(usleeper *t, int64_t ns)
{
    struct awaitable
    {
        usleeper *t;
        int64_t ns;
        bool rejected = false; ///< 槽位被占: 未登记即被拒.

        bool await_ready() const noexcept { return false; }

        bool await_suspend(task<usleeper::result>::coro_handle h) noexcept
        {
            auto &node = t->node_;
            int expect = timer_node::IDLE;
            if (!node.st.compare_exchange_strong(
                    expect, timer_node::REGISTERING,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            { // 槽位被占: 未写任何共享字段, 先睡者完好. 拒绝并
              // 立即返回; 属正常可发生情形, 仅记调试日志.
              // 注: 此处不得读 node 的非原子字段 (tid/prom 可能正被
              // 登记线程写入), 仅使用 CAS 原子输出的 expect.
                rejected = true;
                FRAMEWORK_DBG("usleeper: concurrent sleep rejected, this:",
                              (void *)t, "state:", timer_state_name(expect),
                              "caller tid:", UCOENV.thread_id);
                return false; // 不挂起, 协程立即继续执行.
            }
            auto p = (task<void>::promise_type *)(&h.promise());
            p->tid = UCOENV.thread_id; // 供跨线程 push_sync_node 定位.
            node.prom = p;
            node.tid = UCOENV.thread_id;
            node.deadline_ns = usleeper_mono_now_ns() + ns;
            node.st.store(timer_node::PARKED, std::memory_order_release);
            UCOENV.timer_push(&node); // 入堆并更新超时请求 (仅本线程).
            return true;              // 挂起; 由仲裁赢家唤醒.
        }

        usleeper::result await_resume() noexcept
        {
            if (rejected)
            {
                return usleeper::BUSY; // 未登记: 无需清理, 不碰共享状态.
            }
            auto &node = t->node_;
            int s = node.st.load(std::memory_order_acquire);
            UCOENV.timer_remove(&node); // 若仍在堆中: 移出并更新超时请求.
            node.prom = nullptr;
            // release: 与下一个睡眠的占位 CAS 建立 happens-before,
            // 保证本次清理先于下一次登记可见.
            node.st.store(timer_node::IDLE, std::memory_order_release);
            return s == timer_node::EXPIRED ? usleeper::EXPIRED
                                            : usleeper::WOKEN;
        }
    };

    co_return co_await awaitable{t, ns};
}

usleeper::~usleeper()
{
    if (node_.st.load(std::memory_order_acquire) != timer_node::IDLE)
    { // 挂起中被析构: 节点仍在线程堆中, 悬垂. 契约违反, fail-fast.
        SYSFTL("usleeper: destroyed with a sleep in flight");
    }
}

void usleeper::wake()
{
    int expect = timer_node::PARKED;
    if (node_.st.compare_exchange_strong(expect, timer_node::WOKEN,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire))
    { // 赢得仲裁: 睡眠挂起中, prom 必有效 (st 离开 PARKED 前不会被清空).
        // 投递到睡眠线程 (同线程仅入 sync_list, 跨线程再写其 eventfd).
        int fd = __inner__::push_sync_node_to_thread(node_.prom);
        eventfd_t msg = 1;
        if (fd != 0 && write(fd, &msg, sizeof(msg)) < 0)
        {
            SYSWRN("usleeper: wake eventfd write failed, fd:", fd);
        }
        return;
    }
    // 无挂起睡眠 (或已被到期/唤醒赢走): 记待处理, 下次睡眠立即返回.
    pending_wakes_.fetch_add(1, std::memory_order_release);
}

} // namespace uco
