#pragma once
#include "uco.h"
#include <atomic>
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

#define USE_NEW_SHARED_MUTEX_IMPL

#ifndef USE_NEW_SHARED_MUTEX_IMPL
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
#else
/**
 * @brief 协程读写锁 (结构同 Go sync.RWMutex).
 *
 * readers_ >= 0 即活跃读者数; 写者经 w_ 排队后减 kMaxReaders 挂牌,
 * 新读者见负值即在 reader_sema 排队, 存量读者由 reader_wait_ 倒计数,
 * 归零者唤醒写者. 阻塞/唤醒全部委托 umutex 与 usema (复用其队列与
 * eventfd 管道, 无自旋锁, 无手写唤醒).
 *
 * @note 无竞争读锁为单次原子加 (无挂起); 写者登记后新读者不再插队.
 */
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
    static constexpr int kMaxReaders = 1 << 30; ///< 写者挂牌偏移 (Go 同款).

    umutex w_;                     ///< 写者间互斥 (含饥饿模式).
    void *reader_sema_;            ///< 读者排队: 写者登记期间到达.
    void *writer_sema_;            ///< 写者等存量读者清场.
    std::atomic<int> readers_;     ///< 活跃读者数; 写者登记后 -= kMaxReaders.
    std::atomic<int> reader_wait_; ///< 写者登记时的存量读者倒计数.
};
#endif

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
 * @brief 可唤醒的定时睡眠 (框架级定时器, 支持跨线程唤醒).
 *
 * 睡眠按到期时刻挂入所在线程的定时器堆 (小根堆); 每个线程只向
 * io_uring 提交一个针对堆顶时刻的超时请求, 到期后由调度器唤醒
 * 所有该醒的睡眠. wake() 任意线程可调: 与自然到期经原子状态
 * (PARKED -> WOKEN / EXPIRED) 仲裁, 赢家负责唯一一次唤醒, 无竞态.
 *
 * @note 资源: 与实例数无关. fd 为每线程一个 (框架唤醒通道);
 *       io_uring 流量仅取决于堆顶变化次数, 而非睡眠次数.
 * @note 线程约束: sleep_for/sleep_until 须在协程中 co_await; wake()
 *       任意线程 (含非 uco 线程) 均可调用.
 * @note 唤醒为提示性 (类似条件变量): wake() 时若无挂起睡眠, 计数
 *       保留, 下一次 sleep_for 立即返回 false, 不丢失唤醒.
 * @note 同一 usleeper 同时至多一个生效睡眠: 冲突时后来者不 core,
 *       记 SYSERR 并立即返回 false (语义同被唤醒), 先睡者不受
 *       影响; 先睡者恢复后槽位自动释放.
 * @note 析构时须无挂起睡眠 (由调用方结构保证, 如协程帧持有外层
 *       state 的 shared_ptr).
 * @note sleep_until 内部换算为剩余时长; 被 wake 打断后以原时刻重睡
 *       会重新换算, 无漂移.
 */
class usleeper
{
  public:
    usleeper() = default;
    ~usleeper();

    usleeper(const usleeper &) = delete;
    usleeper &operator=(const usleeper &) = delete;

    /// sleep 的结果.
    enum result
    {
        EXPIRED, ///< 睡满: 自然到期 (时长 <= 0 视为已到期).
        WOKEN,   ///< 被 wake() 打断 (含消费醒着时积压的唤醒请求).
        BUSY,    ///< 未睡: 槽位被另一个进行中的睡眠占用 (提示性,
                 ///< 生产环境可能频繁发生, 仅记调试日志).
    };

    /**
     * @brief 睡眠指定时长.
     * @param d 时长, 任意 chrono duration; <= 0 视为已睡满,
     *          不消耗待处理唤醒.
     * @return 结果, 见 result.
     */
    template <typename Rep, typename Period>
    task<result> sleep_for(const std::chrono::duration<Rep, Period> &d)
    {
        if (pending_wakes_.exchange(0, std::memory_order_acquire) > 0)
        { // 醒着时积压的唤醒请求: 视为被唤醒, 不丢失.
            co_return WOKEN;
        }
        auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
        if (ns <= 0)
        { // 时长已到: 视为睡满.
            co_return EXPIRED;
        }
        co_return co_await usleeper_raw_sleep(this, ns);
    }

    /**
     * @brief 睡眠到指定时刻.
     * @param tp 目标时刻, 任意 Clock 的 time_point; 已过则立即返回.
     * @return 结果, 见 result.
     */
    template <typename Clock, typename Duration>
    task<result> sleep_until(
        const std::chrono::time_point<Clock, Duration> &tp)
    {
        auto now = Clock::now();
        if (now >= tp)
        {
            co_return EXPIRED;
        }
        co_return co_await sleep_for(tp - now);
    }

    /**
     * @brief 打断一个挂起中的睡眠 (提示性, 见类注释); 任意线程可调.
     */
    void wake();

  private:
    friend task<result> usleeper_raw_sleep(usleeper *, int64_t ns);

    __inner__::timer_node node_;      ///< 常驻睡眠节点 (地址稳定).
    std::atomic<uint64_t> pending_wakes_ = 0; ///< 醒着时的唤醒计数.
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

    /**
     * @brief 添加批次任务，临时 task 可安全传入.
     * @warning 禁止传入立即调用的临时捕获型协程 lambda 返回的 task：其 closure
     *          会在惰性协程恢复前析构，协程访问捕获项将发生 UAF.
     * @note 应使用具名协程，或确保协程 lambda 对象存活至批次结束.
     */
    void add(uco::task<void> task);

    /**
     * @brief 启动调度.
     * @note 为确保并发量控制, cobatch 不支持嵌套, 内部嵌套 cobatch 会退化为串行执行.
     */
    void start();

    /**
     * @brief 等待批次完成 (调度协程及其 go 出的全部任务结束).
     * @note 该接口用于保证 cobatch 启动的所有协程的生命周期, 必须 co_await, 禁止 go.
     */
    uco::task<void> wait();

    /**
     * @brief start & wait.
     * @note 该接口用于保证 cobatch 启动的所有协程的生命周期, 必须 co_await, 禁止 go.
     */
    uco::task<void> run();

private:
    int concurrent_ = 0;
    bool in_flight_ = false;  ///< 批次在处理中 (已 start 未 wait).
    void* done_ = 0;          ///< 批次完成信号 (wait 与 dispatch 通信).
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
