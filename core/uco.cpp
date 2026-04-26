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

  private:
    std::atomic<task<void>::promise_type *> list_head = nullptr;
};

void suspend_always::await_suspend(std::coroutine_handle<> h) noexcept
{
    SYSDBG("await_suspend");
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
        SYSDBG("caller:", caller, ',', "current:", pcur);
        cur_co.resume();
        if (caller)
        {
            if (caller != cur_co.promise().caller)
            {
                SYSDBG("coroutine", pcur, "finished, caller:", caller);
                if (caller == (void *)1) [[unlikely]]
                {
                    SYSFTL(NR(pcur), "never yield and finish, why return here ? "
                           "(should return to await_ready)");
                }
                pcur = caller;
            }
            else
            {
                SYSDBG("cur_co not finish, leave:", pcur);
                pcur = nullptr;
            }
        }
        else
        {
            SYSDBG("no caller, leave:", pcur);
            pcur = nullptr;
        }
    }
}

thread_co_env::thread_co_env()
{
    thread_id = gettid();
    SYSMSG("construct thread_co_env:", thread_id);
    sync_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    uring = new struct io_uring;
    if (io_uring_queue_init(8192, (struct io_uring*)uring, 0) < 0)
    {
        SYSERR("io_uring_queue_init:", strerror(errno));
        _exit(1);
    }

    yield_list = new uco_linked_list();
    wait_sqe_list = new uco_linked_list();

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

    struct io_uring* pUring = (struct io_uring*)uring;
    if (pUring) delete pUring;
    uring = nullptr;

    eventfd_t* pSyncBuffer = (eventfd_t*)sync_buffer;
    if (pSyncBuffer) delete pSyncBuffer;
    sync_buffer = nullptr;
}

void thread_co_env::schedule()
{
    SYSMSG("shceduler start");
    int retry_register_read_syncfd = 0;
    auto yield_co_list = (uco_linked_list *)yield_list;
    auto sqe_co_list = (uco_linked_list *)wait_sqe_list;
    auto sync_co_list = (sync_linked_list *)sync_list;
    while (true)
    {
        if (io_event_count == 0 && sync_event_count == 0 &&
            yield_co_list->empty() && sqe_co_list->empty())
        {
            SYSMSG("shceduler exit");
            break;
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
                    SYSDBG("operate canceled");
                    continue;
                }
                if (cqe->user_data == 1)
                {
                    SYSDBG("sync fd notify:", NR(thread_id), NR(sync_fd));
                    REGISTER_READ_SYNCFD(retry_register_read_syncfd);
                    ++io_event_count;
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
                SYSDBG("join thead:", id, "begin");
                if (pthread->joinable())
                {
                    pthread->join();
                }
                delete pthread;
                SYSDBG("join thead:", id, "finish");
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
