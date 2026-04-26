#define _UCO_THREAD_ENV_IMPL
#include <thread>
#include "usync.h"
#include "uco.h"
#include "ulog.h"
#include <atomic>
#include <cassert>
#include <coroutine>
#include <chrono>
#include <cstdlib>
#include <emmintrin.h>
#include <sys/eventfd.h>
#include <unordered_set>

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
#define WQ ((__inner__::uco_linked_list *)pWqueue)
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

#undef WQ
#define RQ ((__inner__::uco_linked_list *)pRwaiter)
#define WQ ((__inner__::uco_linked_list *)pWwaiter)

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
: concurrent_(concurrent), done_(0), slot_(0)
{
    slot_ = new uco::usema(std::min(concurrent_, 50));
    done_ = new uco::usema(0);
}

cobatch::~cobatch()
{
    uco::usema* pSlot = (uco::usema*)slot_;
    uco::usema* pDone = (uco::usema*)done_;
    if (pSlot != nullptr) delete pSlot;
    if (pDone != nullptr) delete pDone;
    slot_ = nullptr;
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

uco::task<void> cobatch::run()
{
    if (is_do_batching)
    {
        for (auto&& co : tasks_)
        {
            try { co_await co; }
            catch (std::exception& e) {
                SYSERR("co exception:", e.what());
            }
        }
        co_return;
    }
    is_do_batching = true;
    int task_count = tasks_.size();
    for (auto&& co : tasks_) 
    {
        co_await ((uco::usema*)slot_)->wait();
        go run_one(co, (uco::usema*)slot_, (uco::usema*)done_);
    }
    uco::usema* pDone = (uco::usema*)done_;
    for (int i = 0; i < task_count; i++)
    {
        co_await pDone->wait();
    }
    is_do_batching = false;
    co_return;
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

} // namespace uco
