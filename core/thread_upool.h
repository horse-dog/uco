#pragma once

#include <functional>
#include <memory>
#include <queue>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/uco.h"
#include "core/ulog.h"
#include "core/usync.h"

namespace uco
{
// 线程池
class thread_upool
{
  public:
    enum RetCode
    {
        kOk = 0,
        kExit = 1,
        kException = 2,
        kBusy = 3
    };

  public:
    /**
     * @brief 构造函数
     * @param thread_num 线程数量
     * @param max_pending 队列积压上限, 超过则拒收新任务 (kBusy)
     */
    explicit thread_upool(size_t thread_num, size_t max_pending = 1024)
        : m_pool(std::make_shared<Pool>())
    {
        m_num_workers.store(thread_num);
        m_pool->max_pending = max_pending;
        for (size_t i = 0; i < thread_num; ++i)
        {
            m_threads.emplace_back([this, pool = m_pool] {
                go this->Loop(pool);
            });
        }
    }

    /**
     * @brief 构造函数
     */
    thread_upool() = default;

    /**
     * @brief 移动构造函数
     */
    thread_upool(thread_upool &&) = delete;

    /**
     * @brief 析构函数: 拒收新任务, 队列中未执行任务丢弃并标 kExit 通知等待方,
     *        join 全部线程后返回.
     * @note  析构返回 = 在执行任务已结束, 未执行任务已被拒; 不得在任务内析构池
     *       (join 自身死锁), 亦不得在任务中持续投递新任务.
     */
    ~thread_upool()
    {
        if (m_pool && !m_pool->isClosed)
        {
            SYSFTL("POOL not close, use Close() before dtor");
        }
    }

    uco::task<void> close()
    {
        if (static_cast<bool>(m_pool))
        {
            {
                auto locker = co_await uco::ulock_guard(m_pool->mtx);
                if (m_pool->isClosed)
                {
                    co_return;
                }
                m_pool->isClosed = true;
            }
            m_pool->cond.notify_all();
            for (auto &t : m_threads)
            {
                if (t.joinable())
                    t.join();
            }
        }
    }

    /**
     * @brief 在线程池中执行 fn(args...)，忽略 fn 的返回值并等待任务完成.
     * @return kOk(0): 执行成功.
     * @return kExit(1): 线程池已关闭.
     * @return kException(2): 任务抛出异常.
     * @return kBusy(3): 无工作线程或等待队列已满.
     * @note fn 由任务持有；左值参数按引用传递，右值参数按值持有.
     *       调用方须保证左值参数存活至 execute() 返回.
     */
    template <class F, class... Args>
    uco::task<int> execute(F &&fn, Args &&...args)
    {
        auto store_arg = []<class T>(T &&arg) {
            if constexpr (std::is_lvalue_reference_v<T &&>)
                return std::ref(arg);
            else
                return std::decay_t<T>(std::forward<T>(arg));
        };
        auto task = [fn = std::forward<F>(fn),
                     args = std::tuple{
                         store_arg(std::forward<Args>(args))...}]() mutable {
            std::apply(std::move(fn), std::move(args));
        };
        return executeTask(std::move(task));
    }

    ///> @brief 添加当前线程为工作线程的一员.
    ///> @note 需要自行确保 pool 生命周期可用, 例如使用 co_await, 或 cobatch 进行异步.
    uco::task<void> add_current()
    {
        ++m_num_workers;
        co_await Loop(m_pool);
    }

  private:
    // 任务
    struct Task
    {
        int *ret_code =
            0; // 0: ok, 1: 线程池已退出, 2: 执行异常, 3: 负载过高拒绝.
        uco::usema *notify_sema = 0;
        std::function<void()> job;
    };

    // 任务池
    struct Pool
    {
        uco::umutex mtx; // 任务池锁,保证任务队列存取的线程安全
        uco::ucond cond; // 任务池条件变量, 任务队列空时阻塞线程
        bool isClosed = false;     // 任务池是否关闭
        size_t max_pending = 1024; // 队列积压上限 (背压拒收)
        std::queue<Task> tasks;    // 任务队列
    };
    std::shared_ptr<Pool> m_pool;       // 任务池
    std::vector<std::thread> m_threads; // 工作线程 (析构 join)
    std::atomic<int> m_num_workers = 0;

  private:
    template <class F> uco::task<int> executeTask(F task)
    {
        uco::usema sem;
        int ret_code = RetCode::kOk;
        int add_ret = co_await addTask(std::move(task), &ret_code, &sem);
        if (add_ret != RetCode::kOk)
        {
            co_return add_ret;
        }
        co_await sem.wait();
        co_return ret_code;
    }

    /**
     * @brief 添加一个待执行任务
     * @param task 待执行任务
     * @return kOk: 已入队; kExit: 线程池已关闭;
     *         kBusy: 无工作线程或等待队列已满.
     */
    template <class F>
    uco::task<int> addTask(F &&task, int *ret_code, uco::usema *sem)
    {
        if (m_num_workers.load() == 0)
        {
            LOGERR("No worker");
            co_return RetCode::kBusy;
        }
        if (!m_pool)
        {
            LOGERR("thread_upool not initialized, reject");
            co_return RetCode::kExit;
        }
        {
            auto locker = co_await uco::ulock_guard(m_pool->mtx);
            if (m_pool->isClosed)
            {
                LOGERR("Pool exit, reject");
                co_return RetCode::kExit;
            }
            if (m_pool->tasks.size() >= m_pool->max_pending)
            {
                SYSERR("Pool overloaded, reject");
                co_return RetCode::kBusy;
            }
            m_pool->tasks.emplace(ret_code, sem, std::forward<F>(task));
        }
        m_pool->cond.notify_one();
        co_return RetCode::kOk;
    }

    uco::task<void> Loop(std::shared_ptr<Pool> pool)
    {
        auto locker = co_await uco::unique_ulock(pool->mtx);
        while (true)
        {
            if (pool->isClosed)
            {
                SYSDBG("thread pool exit, break loop");
                std::queue<Task> tasks;
                tasks.swap(pool->tasks);
                locker.unlock();

                while (!tasks.empty())
                {
                    auto task = std::move(tasks.front());
                    tasks.pop();
                    if (task.ret_code)
                        *task.ret_code = RetCode::kExit;
                    if (task.notify_sema)
                    {
                        task.notify_sema->signal();
                    }
                    else
                    {
                        SYSERR("Error: no notify sema");
                    }
                }

                break;
            }
            else if (!pool->tasks.empty())
            {
                auto task = std::move(pool->tasks.front());
                pool->tasks.pop();
                locker.unlock();
                try
                {
                    task.job();
                    if (task.ret_code)
                        *task.ret_code = RetCode::kOk;
                }
                catch (const std::exception &e)
                {
                    SYSERR("thread pool task exception:", e.what());
                    if (task.ret_code)
                        *task.ret_code = RetCode::kException;
                }
                catch (...)
                {
                    SYSERR("thread pool task exception: unknown");
                    if (task.ret_code)
                        *task.ret_code = RetCode::kException;
                }
                if (task.notify_sema) [[likely]]
                {
                    task.notify_sema->signal();
                }
                else
                {
                    SYSERR("Error: no notify sema");
                }
                co_await locker.lock();
            }
            else
            {
                co_await pool->cond.wait(locker);
            }
        }
    }
};
}
