#pragma once

#include "core/umacro.h"
#include "core/ulog.h"
#include <atomic>
#include <coroutine>
#include <exception>
#include <liburing.h>

using i8 = char;
using u8 = unsigned char;
using i16 = short;
using u16 = unsigned short;
using i32 = int;
using u32 = unsigned int;
using i64 = int64_t;
using u64 = uint64_t;

#if USE_FRAMEWORK_DBG
#define FRAMEWORK_DBG SYSDBG
#else
#define FRAMEWORK_DBG(...)
#endif

namespace uco
{
namespace __inner__
{
struct __go__;
struct suspend_always;
struct suspend_conditional;
} // namespace __inner__

// Forward declaration so task<T>::operator co_await() can reference it
template <class _Tp> struct task_awaiter;

template <class _Tp> struct [[nodiscard("coroutine")]] task
{
    struct promise_type
    {
        using coro_handle = std::coroutine_handle<promise_type>;

        // 显式声明构造函数以禁用聚合初始化，避免协程参数被误初始化到 caller。
        promise_type() noexcept = default;

        void *caller = nullptr; // caller coroutine handle addr.
        void *next = nullptr;   // next coroutine promise addr.
        u64 tid = 0;
        std::exception_ptr error = nullptr;
        typename std::aligned_storage<sizeof(_Tp), alignof(_Tp)>::type
            value_buf;

        // set_value used in scheduler, return_value used in coroutine.
        void set_value(_Tp &&value) noexcept
        {
            FRAMEWORK_DBG("set_value");
            new (&value_buf) _Tp(std::forward<_Tp>(value));
        }

        const _Tp &value() const noexcept
        {
            return *reinterpret_cast<const _Tp *>(&value_buf);
        }

        _Tp &value() noexcept
        {
            return *reinterpret_cast<_Tp *>(&value_buf);
        }

        auto get_return_object() noexcept
        {
            return task(coro_handle::from_promise(*this));
        }

        auto initial_suspend() noexcept -> std::suspend_always;

        auto final_suspend() noexcept -> __inner__::suspend_conditional;

        // set_value used in scheduler, return_value used in coroutine.
        template <class _Up> void return_value(_Up &&value) noexcept
        {
            FRAMEWORK_DBG("return_value");
            new (&value_buf) _Tp(std::forward<_Up>(value));
        }

        void unhandled_exception() noexcept
        {
            FRAMEWORK_DBG("exception");
            error = std::current_exception();
        }

        template <class _Up> auto await_transform(_Up &&task) noexcept
        {
            FRAMEWORK_DBG("await_transform (_Up&&)");
            return std::forward<_Up>(task);
        }

        auto await_transform(std::suspend_always task) noexcept
            -> __inner__::suspend_always;
    };

    using coro_handle = typename promise_type::coro_handle;

    task() noexcept = default;
    task(task& t) noexcept
    : m_handle(t.m_handle)
    {
        t.m_handle = nullptr;
    }
    task& operator=(task& t) noexcept
    {
        if (this != &t)
        {
            auto tmp = m_handle;
            m_handle = t.m_handle;
            t.m_handle = tmp;
        }
        return *this;
    }
    task(task&& t) noexcept
    : m_handle(t.m_handle)
    {
        t.m_handle = nullptr;
    }
    task& operator=(task&& t) noexcept
    {
        if (this != &t)
        {
            auto tmp = m_handle;
            m_handle = t.m_handle;
            t.m_handle = tmp;
        }
        return *this;
    }

    explicit task(coro_handle handle) noexcept : m_handle(handle)
    {
        FRAMEWORK_DBG("task ctor, handle:", m_handle.address());
    }

    ~task()
    {
        // handle will be released by system or co_await.
        if (m_handle != nullptr) [[unlikely]]
        {
            SYSFTL("create a task, but not go it or co_await it!");
        }
        return;
    }

    auto operator co_await();

  private:
    friend struct __inner__::__go__;
    coro_handle m_handle = nullptr;
};

// ==================== task<T> awaiter ====================
template <class _Tp>
struct task_awaiter
{
    using coro_handle = typename uco::task<_Tp>::coro_handle;

    explicit task_awaiter(coro_handle h) noexcept : m_handle(h) {}

    bool await_ready() noexcept
    {
        FRAMEWORK_DBG("handle:", m_handle.address());
        if (m_handle != nullptr) [[likely]]
        {
            FRAMEWORK_DBG("resume handle", m_handle.address(), "begin");
            m_handle.promise().caller = (void *)1;
            m_handle.resume();
            FRAMEWORK_DBG("resume handle", m_handle.address(), "end");
            FRAMEWORK_DBG(NR(m_handle.done()));
        }
        else
        {
            SYSFTL("handle is nullptr");
        }
        return m_handle.done();
    }

    void await_suspend(std::coroutine_handle<> caller) noexcept
    {
        FRAMEWORK_DBG("handle:", m_handle.address(), ',', "caller:", caller.address());
        m_handle.promise().caller = caller.address();
    }

    _Tp await_resume()
    {
        // value_buf 在协程帧内，必须在 destroy 前移出；这里要求移动构造不抛，
        // 否则 destroy 会被跳过导致协程帧泄漏。
        static_assert(std::is_nothrow_move_constructible_v<_Tp>,
                      "uco::task<T> requires T to be nothrow move constructible");
        FRAMEWORK_DBG("handle:", m_handle.address());
        auto err = m_handle.promise().error;
        if (err) [[unlikely]]
        {
            FRAMEWORK_DBG("destory handle", m_handle.address());
            m_handle.destroy();
            FRAMEWORK_DBG("rethrow exception");
            std::rethrow_exception(err);
        }
        auto val = std::move(m_handle.promise().value());
        FRAMEWORK_DBG("destory handle", m_handle.address());
        m_handle.destroy();
        return val;
    }

  private:
    coro_handle m_handle = nullptr;
};

template <class _Tp>
inline auto task<_Tp>::operator co_await()
{
    auto tmp = m_handle;
    m_handle = nullptr;
    return task_awaiter<_Tp>(tmp);
}

template <> struct [[nodiscard("coroutine")]] task<void>
{
    struct promise_type
    {
        using coro_handle = std::coroutine_handle<promise_type>;

        // 显式声明构造函数以禁用聚合初始化，避免协程参数被误初始化到 caller。
        promise_type() noexcept = default;

        void *caller = nullptr; // caller coroutine handle addr.
        void *next = nullptr;   // next coroutine promise addr.
        u64 tid = 0;
        std::exception_ptr error = nullptr;
        u64 value_buf = 0;

        void set_value(u64 value) noexcept
        {
            FRAMEWORK_DBG("set_value");
            value_buf = value;
        }

        u64 value() const noexcept { return value_buf; }

        auto get_return_object() noexcept
        {
            return task(coro_handle::from_promise(*this));
        }

        inline auto initial_suspend() noexcept -> std::suspend_always;

        inline auto final_suspend() noexcept -> __inner__::suspend_conditional;

        void return_void() noexcept { FRAMEWORK_DBG("return_void"); }

        void unhandled_exception() noexcept
        {
            FRAMEWORK_DBG("exception");
            error = std::current_exception();
        }

        template <class _Up> auto await_transform(_Up &&task) noexcept
        {
            FRAMEWORK_DBG("await_transform (_Up&&)");
            return std::forward<_Up>(task);
        }

        inline auto await_transform(std::suspend_always task) noexcept
            -> __inner__::suspend_always;
    };

    using coro_handle = typename promise_type::coro_handle;

    task() noexcept = default;
    task(task& t) noexcept
    : m_handle(t.m_handle)
    {
        t.m_handle = nullptr;
    }
    task& operator=(task& t) noexcept
    {
        if (this != &t)
        {
            auto tmp = m_handle;
            m_handle = t.m_handle;
            t.m_handle = tmp;
        }
        return *this;
    }
    task(task&& t) noexcept
    : m_handle(t.m_handle)
    {
        t.m_handle = nullptr;
    }
    task& operator=(task&& t) noexcept
    {
        if (this != &t)
        {
            auto tmp = m_handle;
            m_handle = t.m_handle;
            t.m_handle = tmp;
        }
        return *this;
    }

    explicit task(coro_handle handle) noexcept : m_handle(handle)
    {
        FRAMEWORK_DBG("task ctor, handle:", m_handle.address());
    }

    ~task()
    {
        // handle will be released by system or co_await.
        if (m_handle != nullptr) [[unlikely]]
        {
            SYSFTL("create a task, but not go it or co_await it!");
        }
        return;
    }

    auto operator co_await();

  private:
    friend struct __inner__::__go__;
    coro_handle m_handle = nullptr;
};

template <>
struct task_awaiter<void>
{
    using coro_handle = uco::task<void>::coro_handle;

    explicit task_awaiter(coro_handle h) noexcept : m_handle(h) {}

    bool await_ready() noexcept
    {
        FRAMEWORK_DBG("handle:", m_handle.address());
        if (m_handle != nullptr) [[likely]]
        {
            FRAMEWORK_DBG("resume handle", m_handle.address(), "begin");
            m_handle.promise().caller = (void *)1;
            m_handle.resume();
            FRAMEWORK_DBG("resume handle %p end", m_handle.address());
            FRAMEWORK_DBG(NR(m_handle.done()));
        }
        else
        {
            SYSFTL("handle is nullptr");
        }
        return m_handle.done();
    }

    void await_suspend(std::coroutine_handle<> caller) noexcept
    {
        FRAMEWORK_DBG("handle:", m_handle.address(), ',', "caller:", caller.address());
        m_handle.promise().caller = caller.address();
    }

    void await_resume()
    {
        FRAMEWORK_DBG("handle:", m_handle.address());
        auto err = m_handle.promise().error;
        FRAMEWORK_DBG("destory handle", m_handle.address());
        m_handle.destroy();
        if (err) [[unlikely]]
        {
            FRAMEWORK_DBG("rethrow exception");
            std::rethrow_exception(err);
        }
    }

  private:
    coro_handle m_handle = nullptr;
};

inline auto task<void>::operator co_await()
{
    auto tmp = m_handle;
    m_handle = nullptr;
    return task_awaiter<void>(tmp);
}
} // namespace uco

namespace uco
{
namespace __inner__
{

/**
 * @brief usleeper 一次睡眠的登记信息.
 *
 * 节点作为成员常驻于 usleeper 对象 (而非协程帧), 地址因此保持稳定,
 * 任意线程调用 wake() 访问它都不会悬垂.
 *
 * st 同时兼任"睡眠槽位": sleep 以 CAS IDLE -> REGISTERING 原子占位
 * (成功后才写其余字段), 因此同一 usleeper 同时至多一个生效睡眠.
 * 槽位被占时后来者被拒 (记 SYSERR, 立即返回), 不影响先睡者:
 *
 * - 占位:     IDLE       -> REGISTERING (睡眠协程, 写字段前)
 * - 就绪:     REGISTERING-> PARKED      (睡眠协程, 字段填毕后)
 * - 自然到期: PARKED     -> EXPIRED     (睡眠所在线程的调度器)
 * - 唤醒:     PARKED     -> WOKEN       (任意线程的 wake())
 * - 清理:     *          -> IDLE        (睡眠协程, 恢复执行时)
 */
struct timer_node
{
    enum state { IDLE = 0, REGISTERING = 1, PARKED = 2, WOKEN = 3, EXPIRED = 4 };
    std::atomic<int> st{IDLE};
    uco::task<void>::promise_type *prom = nullptr; ///< 挂起期间有效 (唤醒投递目标).
    u64 tid = 0;         ///< 睡眠所在线程 (跨线程唤醒时定位目标线程).
    i64 deadline_ns = 0; ///< 到期时刻 (CLOCK_MONOTONIC 纳秒).
    u64 idx = 0;         ///< 在定时器堆中的下标 (仅睡眠线程访问).
    bool in_heap = false;///< 是否仍在定时器堆中 (仅睡眠线程访问).
};

struct thread_co_env
{
#ifndef _UCO_THREAD_ENV_IMPL
private:
#endif
    static thread_co_env &GetInstance()
    {
        thread_local thread_co_env instance;
        return instance;
    }

    struct io_uring_sqe *get_sqe() noexcept;
    void schedule();

    thread_co_env(const thread_co_env &) = delete;
    thread_co_env &operator=(const thread_co_env &) = delete;
    thread_co_env(thread_co_env &&) = delete;
    thread_co_env &operator=(thread_co_env &&) = delete;

    i64 io_event_count = 0;
    i64 sync_event_count = 0;
    u64 thread_id = 0;
    i32 sync_fd = -1;
    i32 flags = 0;
    void *sync_buffer = 0;
    void *uring = 0;
    void *sync_list = 0;
    void *yield_list = 0;
    void *wait_sqe_list = 0;

    // ---- per-thread timer infra (usleeper) ----
    // 原理: 所有睡眠按到期时刻入小根堆, 线程只向 io_uring 提交一个
    // 针对堆顶时刻的超时请求; 堆顶变化时取消旧请求、提交新请求.
    void timer_push(uco::__inner__::timer_node *node);   // 入堆并更新超时请求
    void timer_remove(uco::__inner__::timer_node *node); // 移出堆并更新超时请求
    void timer_on_fire();                                // 超时请求完成: 唤醒到期睡眠
    void timer_refresh();                                // 提交/取消超时请求 (幂等)
    bool timer_active();                                 // 堆非空或存在未决超时请求

    void *timer_heap = 0;                 ///< std::vector<timer_node*>*
    struct __kernel_timespec *timer_ts = 0; ///< 超时请求的时长 (须存活到内核提交)
    u64 timer_gen = 0;                    ///< 超时请求序号 (编码进 user_data)
    i64 timer_inflight = 0;               ///< 已提交且未完成的超时请求数
    i64 timer_armed_deadline = 0;         ///< 当前超时请求对应的到期时刻
    u64 timer_armed_user_data = 0;        ///< 当前超时请求的 user_data (取消时定位用)
    bool timer_arm_pending = false;       ///< 提交队列满导致的待重试标记
    int scheduler_state = 0;              ///< 0=未开跑 1=运行中 2=已退出 (此后 go abort)

  private:
    thread_co_env();
    ~thread_co_env();
};

extern void __go_dispatch(void *handle);

struct __go__
{
    template <class _Tp> inline void operator-(uco::task<_Tp> &&task)
    {
        __go_dispatch(task.m_handle.address());
        task.m_handle = nullptr;
    }
};

struct suspend_always
{
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<>) noexcept;
    void await_resume() const noexcept {}
};

struct suspend_conditional
{
    bool suspend = false;
    bool await_ready() noexcept { return !suspend; }
    void await_suspend(std::coroutine_handle<>) noexcept {}
    void await_resume() noexcept {}
};

struct uco_linked_list
{
    using uco_linked_node = uco::task<void>::promise_type;
    uco::task<void>::promise_type *head = nullptr;
    uco::task<void>::promise_type *tail = nullptr;

    inline bool empty() const noexcept { return head == nullptr; };

    std::vector<uco_linked_node *> take_all() noexcept;

    void push(uco_linked_node *node, bool lifo=false) noexcept;

    uco_linked_node *pop() noexcept;
};
} // namespace __inner__

template <class _Tp>
auto task<_Tp>::promise_type::initial_suspend() noexcept -> std::suspend_always
{
    FRAMEWORK_DBG("initial_suspend");
    return std::suspend_always();
}

auto task<void>::promise_type::initial_suspend() noexcept -> std::suspend_always
{
    FRAMEWORK_DBG("initial_suspend");
    return std::suspend_always();
}

template <class _Tp>
auto task<_Tp>::promise_type::final_suspend() noexcept
    -> __inner__::suspend_conditional
{
    FRAMEWORK_DBG("final_suspend");
    // 根协程（go 启动、无人 await）带着未处理异常结束：
    // 不会再有任何 await 方重抛这个异常，它将随协程帧一起无声消失。
    // 与 std::thread 顶层未捕获异常语义对齐：记录后终止进程（fail-fast）。
    // 定义 UCO_TOLERATE_ROOT_EXCEPTIONS 可退回"仅记日志，不终止"。
    if (caller == nullptr && error) [[unlikely]]
    {
        try
        {
            std::rethrow_exception(error);
        }
        catch (const std::exception &e)
        {
            SYSERR("root coroutine died with unhandled exception:", e.what());
        }
        catch (...)
        {
            SYSERR("root coroutine died with unknown exception");
        }
#if !UCO_TOLERATE_ROOT_EXCEPTIONS
        std::terminate(); // SIGABRT，配合 ulimit -c unlimited 生成 core
#endif
    }
    bool suspend = (caller != nullptr);
    if (caller != nullptr)
    {
        FRAMEWORK_DBG("caller:", caller);
        caller = nullptr;
    }
    return __inner__::suspend_conditional(suspend);
}

auto task<void>::promise_type::final_suspend() noexcept
    -> __inner__::suspend_conditional
{
    FRAMEWORK_DBG("final_suspend");
    // 同 task<_Tp>::promise_type::final_suspend：根协程未捕获异常，
    // 无人会再消费，最后的机会在这里报告并 fail-fast。
    if (caller == nullptr && error) [[unlikely]]
    {
        try
        {
            std::rethrow_exception(error);
        }
        catch (const std::exception &e)
        {
            SYSERR("root coroutine died with unhandled exception:", e.what());
        }
        catch (...)
        {
            SYSERR("root coroutine died with unknown exception");
        }
#if !UCO_TOLERATE_ROOT_EXCEPTIONS
        std::terminate(); // SIGABRT，配合 ulimit -c unlimited 生成 core
#endif
    }
    bool suspend = (caller != nullptr);
    if (caller != nullptr)
    {
        FRAMEWORK_DBG("caller:", caller);
        caller = nullptr;
    }
    return __inner__::suspend_conditional(suspend);
}

template <class _Tp>
auto task<_Tp>::promise_type::await_transform(std::suspend_always task) noexcept
    -> __inner__::suspend_always
{
    FRAMEWORK_DBG("await_transform (std::suspend_always)");
    return __inner__::suspend_always();
}

auto task<void>::promise_type::await_transform(
    std::suspend_always task) noexcept -> __inner__::suspend_always
{
    FRAMEWORK_DBG("await_transform (std::suspend_always)");
    return __inner__::suspend_always();
}
} // namespace uco

#define go uco::__inner__::__go__() -
#define co_yield co_await std::suspend_always{}
