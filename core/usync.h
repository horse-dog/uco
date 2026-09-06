#pragma once
#include "uco.h"
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <deque>

namespace uco
{

class umutex
{
  public:
    umutex();

    ~umutex();

    umutex(umutex &&) = delete;

    umutex &operator=(umutex &&) = delete;

    umutex(const umutex &) = delete;

    umutex &operator=(const umutex &) = delete;

    task<void> lock();

    bool try_lock();

    void unlock();

  private:
    std::atomic<int> iState;
    void *pSema;
};

class ushared_mutex
{
  public:
    ushared_mutex();

    ~ushared_mutex();

    ushared_mutex(const ushared_mutex &) = delete;

    ushared_mutex &operator=(const ushared_mutex &) = delete;

    ushared_mutex(ushared_mutex &&) = delete;

    ushared_mutex &operator=(ushared_mutex &&) = delete;

    task<void> lock();

    bool try_lock();

    void unlock();

    task<void> lock_shared();

    bool try_lock_shared();

    void unlock_shared();

  private:
    void *pRwaiter;
    void *pWwaiter;
    void *pLock;
    int state; // -1: write, 0~n: readers count.
};

template <class _Mutex>
class ulock_guard_t
{
  template <class _Mu> friend uco::task<ulock_guard_t<_Mu>> ulock_guard(_Mu& mutex);

  public:
    ~ulock_guard_t()
    {
        if (_M_mtx_ptr) _M_mtx_ptr->unlock();
        _M_mtx_ptr = 0;
    }
    ulock_guard_t(const ulock_guard_t&) = delete;
    ulock_guard_t& operator=(const ulock_guard_t&) = delete;
    ulock_guard_t(ulock_guard_t&& __t) noexcept : _M_mtx_ptr(__t._M_mtx_ptr)
    {
        __t._M_mtx_ptr = 0;
    }
    ulock_guard_t& operator=(ulock_guard_t&& __t) noexcept
    {
        if (this != &__t)
        {
            std::swap(_M_mtx_ptr, __t._M_mtx_ptr);
        }
        return *this;
    }

    void swap(ulock_guard_t& __t)
    {
        std::swap(_M_mtx_ptr, __t._M_mtx_ptr);
    }

    private:
    ulock_guard_t() = default;
    _Mutex* _M_mtx_ptr = 0;
};

template <class _Mutex>
class unique_ulock_t
{
  template <class _Mu> friend uco::task<unique_ulock_t<_Mu>> unique_ulock(_Mu& mutex);
  template <class _Mu> friend unique_ulock_t<_Mu> unique_ulock(_Mu& mutex, std::defer_lock_t);
  template <class _Mu> friend unique_ulock_t<_Mu> unique_ulock(_Mu& mutex, std::try_to_lock_t);
  template <class _Mu> friend unique_ulock_t<_Mu> unique_ulock(_Mu& mutex, std::adopt_lock_t);

  public:
    unique_ulock_t() noexcept
    : _M_mtx_ptr(0), _M_owns(false)
    { }

    ~unique_ulock_t()
    {
        if (_M_owns) unlock();
    }

    unique_ulock_t(const unique_ulock_t&) = delete;
    unique_ulock_t& operator=(const unique_ulock_t&) = delete;

    unique_ulock_t(unique_ulock_t&& __u) noexcept
    : _M_mtx_ptr(__u._M_mtx_ptr), _M_owns(__u._M_owns)
    {
        __u._M_mtx_ptr = 0;
        __u._M_owns = false;
    }

    unique_ulock_t& operator=(unique_ulock_t&& __u) noexcept
    {
        if (&__u != this)
        {
            if(_M_owns)
                unlock();
            unique_ulock_t(std::move(__u)).swap(*this);
            __u._M_device = 0;
            __u._M_owns = false;
        }
        return *this;
    }

    uco::task<void> lock()
    {
        if (_M_mtx_ptr == nullptr)
            throw std::runtime_error("lock of nullptr");
        else if (_M_owns)
            throw std::runtime_error("double lock");
        else
        {
            co_await _M_mtx_ptr->lock();
            _M_owns = true;
        }
    }

    bool try_lock()
    {
        if (_M_mtx_ptr == nullptr)
            throw std::runtime_error("lock of nullptr");
        else if (_M_owns)
            throw std::runtime_error("double lock");
        else
        {
            _M_owns = _M_mtx_ptr->try_lock();
        }
    }

    void unlock()
    {
        if (!_M_owns)
            throw std::runtime_error("unlock of not owned lock");
        else if (_M_mtx_ptr)
        {
            _M_mtx_ptr->unlock();
            _M_owns = false;
        }
    }

    _Mutex* release() noexcept
    {
        _Mutex* __ret = _M_mtx_ptr;
        _M_mtx_ptr = 0;
        _M_owns = false;
        return __ret;
    }

    _Mutex* mutex() const noexcept
    { return _M_mtx_ptr; }

    bool owns_lock() const noexcept
    { return _M_owns; }

    void swap(unique_ulock_t& __u) noexcept
    {
        std::swap(_M_mtx_ptr, __u._M_device);
        std::swap(_M_owns, __u._M_owns);
    }

  private:
    _Mutex* _M_mtx_ptr = 0;
    bool _M_owns = false;
};

template <class _Mutex>
uco::task<ulock_guard_t<_Mutex>> ulock_guard(_Mutex& mutex)
{
    ulock_guard_t<_Mutex> guard;
    guard._M_mtx_ptr = std::addressof(mutex);
    co_await mutex.lock();
    co_return guard;
}

template <class _Mutex>
uco::task<unique_ulock_t<_Mutex>> unique_ulock(_Mutex& mutex)
{
    unique_ulock_t<_Mutex> ulock;
    ulock._M_mtx_ptr = std::addressof(mutex);
    ulock._M_owns = false;
    co_await mutex.lock();
    ulock._M_owns = true;
    co_return ulock;
}

template <class _Mutex>
unique_ulock_t<_Mutex> unique_ulock(_Mutex& mutex, std::defer_lock_t)
{
    unique_ulock_t<_Mutex> ulock;
    ulock._M_mtx_ptr = std::addressof(mutex);
    ulock._M_owns = false;
    co_return ulock;
}

template <class _Mutex>
unique_ulock_t<_Mutex> unique_ulock(_Mutex& mutex, std::try_to_lock_t)
{
    unique_ulock_t<_Mutex> ulock;
    ulock._M_mtx_ptr = std::addressof(mutex);
    ulock._M_owns = mutex.try_lock();
    co_return ulock;
}

template <class _Mutex>
unique_ulock_t<_Mutex> unique_ulock(_Mutex& mutex, std::adopt_lock_t)
{
    unique_ulock_t<_Mutex> ulock;
    ulock._M_mtx_ptr = std::addressof(mutex);
    ulock._M_owns = true;
    co_return ulock;
}

class ucond
{
  public:
    ucond();

    ~ucond();

    ucond(const ucond &) = delete;

    ucond &operator=(const ucond &) = delete;

    ucond(ucond &&) = delete;

    ucond &operator=(ucond &&) = delete;

    task<void> wait(unique_ulock_t<umutex> &lock);

    template <typename _Predicate>
    task<void> wait(unique_ulock_t<umutex> &lock, _Predicate pred)
    {
        while (!pred())
            co_await wait(lock);
    }

    void notify_one();

    void notify_all();

  private:
    void *pWaiter;
    void *pLock;
};

class usema
{
  public:
    usema(size_t cnt = 0);

    ~usema();

    usema(const usema &) = delete;

    usema &operator=(const usema &) = delete;

    usema(usema &&) = delete;

    usema &operator=(usema &&) = delete;

    task<void> wait(bool lifo=false);
    
    bool try_wait();

    void signal();

  private:
    template <typename _Tp, int Size> friend class uchan;
    void __kill_broadcast();
    void *pWaiter;
    void *pLock;
    std::atomic<size_t> nwait;
    std::atomic<size_t> count;
};

/**
 * @brief 可取消 / 可更新的 io_uring 定时睡眠 (单线程模型).
 *
 * 协程睡在 io_uring timeout 上时, 外界无法及时唤醒它, 只能靠短间隔
 * 轮询标志位. 本类通过 IORING_OP_ASYNC_CANCEL (与 timeout 同一
 * user_data) 实现 "睡眠 / 取消 / 更新", 消除轮询.
 *
 * @note 线程约束: 所有接口仅可在 utimer 所在线程调用, 入口有运行时
 *       检查; 跨线程调用记 SYSERR 并拒绝, sleeper 睡满自然醒
 *       (安全降级, 避免数据竞争).
 * @note 唤醒为提示性 (类似条件变量): wake() 时若无睡眠进行中,
 *       记为待处理请求, 下一次睡眠立即返回 false, 不丢失唤醒.
 * @note 析构时须无挂起的睡眠 (由调用方结构保证, 如协程帧持有
 *       外层 state 的 shared_ptr).
 * @note sleep_until 内部换算为剩余时长走相对 timeout (不用
 *       IORING_TIMEOUT_ABS: 需 5.15+ 且固定 CLOCK_REALTIME);
 *       被 wake 打断后以原时刻重睡会重新换算, 无漂移.
 */
class utimer
{
  public:
    utimer() : owner_tid_(std::this_thread::get_id()) {}
    ~utimer() = default;

    utimer(const utimer &) = delete;
    utimer &operator=(const utimer &) = delete;

    /**
     * @brief 睡眠指定时长.
     * @param d 时长, 任意 chrono duration; <= 0 视为已睡满.
     * @return true 睡满; false 被 wake()/update() 打断.
     */
    template <typename Rep, typename Period>
    task<bool> sleep_for(const std::chrono::duration<Rep, Period> &d)
    {
        if (owner_tid_ != std::this_thread::get_id())
        {
            thread_violation("sleep_for");
            co_return false;
        }
        if (wake_requested_)
        {
            wake_requested_ = false;
            co_return false;
        }
        auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
        if (ns <= 0)
        {
            co_return true;
        }
        int res = co_await utimer_raw_sleep(this, ns);
        co_return (res == -ETIME); // 到期 -ETIME, 被取消 -ECANCELED.
    }

    /**
     * @brief 睡眠到指定时刻.
     * @param tp 目标时刻, 任意 Clock 的 time_point; 已过则立即返回.
     * @return true 到达目标时刻; false 被 wake()/update() 打断.
     */
    template <typename Clock, typename Duration>
    task<bool> sleep_until(
        const std::chrono::time_point<Clock, Duration> &tp)
    {
        auto now = Clock::now();
        if (now >= tp)
        {
            co_return true;
        }
        co_return co_await sleep_for(tp - now);
    }

    /**
     * @brief 打断当前睡眠 (提示性, 见类注释).
     */
    void wake();

    /**
     * @brief 打断当前睡眠并捎带新时长.
     *        sleeper 醒后经 take_pending() 读取并以新时长重睡.
     * @param d 新时长, 任意 chrono duration.
     */
    template <typename Rep, typename Period>
    void update(const std::chrono::duration<Rep, Period> &d)
    {
        if (owner_tid_ != std::this_thread::get_id())
        {
            thread_violation("update");
            return;
        }
        has_pending_ = true;
        pend_ns_ =
            std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
        wake();
    }

    /**
     * @brief 读取并清除 update() 捎带的时长.
     * @param d [out] 输出新时长.
     * @return true 有 pending; false 无.
     */
    bool take_pending(std::chrono::nanoseconds &d);

  private:
    friend task<int> utimer_raw_sleep(utimer *, int64_t ns);

    /// 跨线程调用检测: 记 SYSERR (接口名/期望线程/实际线程).
    void thread_violation(const char *op) const;

    std::thread::id owner_tid_;       ///< 构造线程 = 唯一合法调用线程.
    void *sleeping_handle_ = nullptr; ///< 挂起中 sleeper 句柄 (cancel 目标).
    bool wake_requested_ = false;     ///< 醒着时收到的唤醒请求, 下次睡眠即醒.
    bool has_pending_ = false;        ///< update 捎带的新时长有效位.
    int64_t pend_ns_ = 0;             ///< update 捎带的新时长 (纳秒).
};

template <typename _Tp, int Size=0> class uchan
{
  public:
    uchan() : _M_full(0), _M_space(Size) {}
    ~uchan() {}

    uchan(const uchan &) = delete;
    uchan &operator=(const uchan &) = delete;
    uchan(uchan &&__other) = delete;
    uchan &operator=(uchan &&__other) = delete;

    friend task<void> operator>>(const _Tp &x, uchan &c)
    {
        co_await c._M_space.wait();
        co_await c._M_lock.lock();
        if (c._M_closed)
        {
            SYSERR("write to closed channel");
            std::__terminate();
        }
        c._M_items.push_back(x);
        c._M_lock.unlock();
        c._M_full.signal();
    }

    friend task<void> operator>>(_Tp &&x, uchan &c)
    {
        co_await c._M_space.wait();
        co_await c._M_lock.lock();
        if (c._M_closed)
        {
            SYSERR("write to closed channel");
            std::__terminate();
        }
        c._M_items.push_back(std::move(x));
        c._M_lock.unlock();
        c._M_full.signal();
    }

    task<bool> operator>>(_Tp &x)
    {
        co_await _M_full.wait();
        co_await _M_lock.lock();
        if (_M_items.empty())
        {
            FRAMEWORK_DBG("%d", _M_closed.load());
            _M_lock.unlock();
            co_return false;
        }
        x = std::move(_M_items.front());
        _M_items.pop_front();
        _M_lock.unlock();
        _M_space.signal();
        co_return true;
    }

    void close()
    {
        _M_closed.store(true);
        _M_full.__kill_broadcast();
        _M_space.__kill_broadcast();
    }

  private:
    umutex _M_lock;
    usema _M_full;
    usema _M_space;
    std::atomic<bool> _M_closed = false;
    std::deque<_Tp> _M_items;
};

template <typename _Tp> class uchan<_Tp, 0>
{
  public:
    uchan() = default;
    ~uchan() {}

    uchan(const uchan &) = delete;
    uchan &operator=(const uchan &) = delete;
    uchan(uchan &&__other) = delete;
    uchan &operator=(uchan &&__other) = delete;

    friend task<void> operator>>(const _Tp &x, uchan &c)
    {
        co_await c._M_can_write.wait();
        co_await c._M_lock.lock();
        if (c._M_closed)
        {
            SYSERR("write to closed channel");
            std::__terminate();
        }
        c._M_slot.emplace(x);
        c._M_lock.unlock();
        c._M_can_read.signal();
        co_await c._M_read_done.wait();
        if (c._M_closed)
        {
            SYSERR("write to closed channel");
            std::__terminate();
        }
    }

    friend task<void> operator>>(_Tp &&x, uchan &c)
    {
        co_await c._M_can_write.wait();
        co_await c._M_lock.lock();
        if (c._M_closed)
        {
            SYSERR("write to closed channel");
            std::__terminate();
        }
        c._M_slot.emplace(std::move(x));
        c._M_lock.unlock();
        c._M_can_read.signal();
        co_await c._M_read_done.wait();
        if (c._M_closed)
        {
            SYSERR("write to closed channel");
            std::__terminate();
        }
    }

    task<bool> operator>>(_Tp &x)
    {
        co_await _M_can_read.wait();
        co_await _M_lock.lock();
        if (!_M_slot.has_value())
        {
            FRAMEWORK_DBG("%d", _M_closed.load());
            _M_lock.unlock();
            co_return false;
        }
        x = std::move(*_M_slot);
        _M_slot.reset();
        _M_lock.unlock();
        _M_read_done.signal();
        _M_can_write.signal();
        co_return true;
    }

    void close()
    {
        _M_closed.store(true);
        _M_can_write.__kill_broadcast();
        _M_can_read.__kill_broadcast();
        _M_read_done.__kill_broadcast();
    }

  private:
    umutex _M_lock;
    usema _M_can_write = 1;
    usema _M_can_read = 0;
    usema _M_read_done = 0;
    std::atomic<bool> _M_closed = false;
    std::optional<_Tp> _M_slot;
};

class uthread 
{

public:
    uthread() noexcept {}
    uthread(const uthread&) = delete;
    uthread& operator=(const uthread&) = delete;

    uthread(uthread&& __t) noexcept
    { swap(__t); }

    uthread& operator=(uthread&& __t) noexcept
    {
        if (joinable())
        {
            std::__terminate();
        }
        swap(__t);
        return *this;
    }

    template<typename _Callable, typename... _Args>
    explicit uthread(_Callable&& __f, _Args&&... __args)
    {
        _Mp_thread = new std::thread(std::forward<_Callable>(__f), std::forward<_Args>(__args)...);
    }

    ~uthread()
    {
        if (joinable())
        {
            std::__terminate();
        }
    }

    bool joinable() const
    {
        return _Mp_thread != nullptr;
    }

    void join();

    /**
      * @attention 将线程托管给后台，线程在程序运行结束时才会被join和释放.
      * @note 该接口保证全局变量和TLS在线程中可用.
      * @note 常驻线程调用最佳，动态创建和销毁的线程慎用.
      */
    void daemonize();

    void swap(uthread& __t) noexcept
    { std::swap(_Mp_thread, __t._Mp_thread); }

private:
    std::thread* _Mp_thread = 0;
};

class cobatch
{

public:
    cobatch(int concurrent=20);
    ~cobatch();
    cobatch (const cobatch&) = delete;
    cobatch& operator=(const cobatch&) = delete;
    cobatch (cobatch&&) = delete;
    cobatch& operator=(cobatch&&) = delete;

    void add(uco::task<void> task);

    uco::task<void> run();

private:
    int concurrent_;
    void* done_;
    void* slot_;
    std::vector<task<void>> tasks_;

};

} // namespace uco

namespace std
{
    inline void
    swap(uco::uthread& __x, uco::uthread& __y) noexcept
    { __x.swap(__y); }

    template <class _Mutex>
    inline void
    swap(uco::ulock_guard_t<_Mutex>& __x, uco::ulock_guard_t<_Mutex>& __y) noexcept
    { __x.swap(__y); }

    template <class _Mutex>
    inline void
    swap(uco::unique_ulock_t<_Mutex>& __x, uco::unique_ulock_t<_Mutex>& __y) noexcept
    { __x.swap(__y); }
} // namespace std
