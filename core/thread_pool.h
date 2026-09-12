#pragma once

#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
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
class thread_pool
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
    explicit thread_pool(size_t thread_num, size_t max_pending = 1024)
        : m_pool(std::make_shared<Pool>())
    {
        m_num_workers.store(thread_num);
        m_pool->max_pending = max_pending;
        // if (thread_num == 0)
        // {
        //     LOGFTL("thread_pool: thread_num must be > 0");
        // }
        for (size_t i = 0; i < thread_num; ++i)
        {
            m_threads.emplace_back(Loop, m_pool);
        }
    }

    /**
     * @brief 构造函数
     */
    thread_pool() = default;

    /**
     * @brief 移动构造函数
     */
    thread_pool(thread_pool &&) = delete;

    /**
     * @brief 析构函数: 拒收新任务, 队列中未执行任务丢弃并标 kExit 通知等待方,
     *        join 全部线程后返回.
     * @note  析构返回 = 在执行任务已结束, 未执行任务已被拒; 不得在任务内析构池
     *       (join 自身死锁), 亦不得在任务中持续投递新任务.
     */
    ~thread_pool() { close(); }

    /**
     * @brief 拒收新任务，丢弃队列中未执行任务，等待正在执行的任务结束并
     *        join 全部工作线程。
     * @note 可重复或并发调用；不得在线程池任务内调用（会 join 自身）。
     */
    void close()
    {
        std::lock_guard<std::mutex> close_locker(m_closeMtx);
        if (!m_pool)
        {
            return;
        }

        {
            std::lock_guard<std::mutex> locker(m_pool->mtx);
            if (m_pool->isClosed)
            {
                return;
            }
            m_pool->isClosed = true;
        }
        m_pool->cond.notify_all();
        for (auto &t : m_threads)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
    }

    /**
     * @brief 异步提交 fn(args...)，不等待任务完成.
     * @return kOk: 任务已入队; kExit: 线程池已关闭;
     *         kBusy: 无工作线程或等待队列已满.
     * @note 参数语义与 std::thread 一致：函数和参数默认 decay-copy/move；
     *       如需引用传递，调用方必须显式使用 std::ref/std::cref，并保证引用
     *       对象存活至任务执行结束。任务异步执行时抛出的异常只记录日志，
     *       无法通过本接口返回 kException.
     */
    template <class F, class... Args>
    int addtask(F &&fn, Args &&...args)
    {
        auto task =
            [fn = std::decay_t<F>(std::forward<F>(fn)),
             args = std::tuple<std::decay_t<Args>...>(
                 std::forward<Args>(args)...)]() mutable {
                std::apply(std::move(fn), std::move(args));
            };
        return addTask(std::move(task), nullptr);
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

        // wait() 成功仅表示 signal() 发布的许可已被消费，不保证工作线程的
        // signal() 已返回。若完成状态位于本协程帧并以裸指针交给 Task，当前
        // 协程可能先返回并销毁 usema，而工作线程仍在 signal() 中访问它。
        // execute 与 Task 各持有一份共享所有权，使 Completion 至少存活到
        // signal() 完整返回；这也是普通线程信号量要求调用方保证的生命周期。
        auto completion = std::make_shared<Completion>();
        int add_ret = addTask(std::move(task), completion);
        if (add_ret != RetCode::kOk)
        {
            co_return add_ret;
        }
        co_await completion->notify_sema.wait();
        co_return completion->ret_code;
    }

  private:
    struct Completion
    {
        int ret_code = RetCode::kOk;
        uco::usema notify_sema;
    };

    // 任务
    struct Task
    {
        std::shared_ptr<Completion> completion;
        std::function<void()> job;
    };

    // 任务池
    struct Pool
    {
        std::mutex mtx; // 任务池锁,保证任务队列存取的线程安全
        std::condition_variable cond; // 任务池条件变量, 任务队列空时阻塞线程
        bool isClosed = false;     // 任务池是否关闭
        size_t max_pending = 1024; // 队列积压上限 (背压拒收)
        std::queue<Task> tasks;    // 任务队列
    };
    std::shared_ptr<Pool> m_pool;       // 任务池
    std::vector<std::thread> m_threads; // 工作线程 (析构 join)
    std::atomic<int> m_num_workers = 0;
    std::mutex m_closeMtx; // 串行化 close, 保证重复/并发调用不会重复 join

  private:
    /**
     * @brief 添加一个待执行任务
     * @return kOk: 已入队; kExit: 线程池已关闭;
     *         kBusy: 无工作线程或等待队列已满.
     */
    template <class F>
    int addTask(F &&task, std::shared_ptr<Completion> completion)
    {
        if (m_num_workers.load() == 0)
        {
            LOGERR("No worker");
            return RetCode::kBusy;
        }
        if (!m_pool)
        {
            LOGERR("thread_pool not initialized, reject");
            return RetCode::kExit;
        }

        {
            std::lock_guard<std::mutex> locker(m_pool->mtx);
            if (m_pool->isClosed)
            {
                LOGERR("Pool exit, reject");
                return RetCode::kExit;
            }
            if (m_pool->tasks.size() >= m_pool->max_pending)
            {
                SYSERR("Pool overloaded, reject");
                return RetCode::kBusy;
            }
            m_pool->tasks.emplace(std::move(completion), std::forward<F>(task));
        }
        m_pool->cond.notify_one();
        return RetCode::kOk;
    }

    static void Loop(std::shared_ptr<Pool> pool)
    {
        std::unique_lock<std::mutex> locker(pool->mtx);
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
                    if (task.completion)
                    {
                        task.completion->ret_code = RetCode::kExit;
                        task.completion->notify_sema.signal();
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
                    if (task.completion)
                        task.completion->ret_code = RetCode::kOk;
                }
                catch (const std::exception &e)
                {
                    SYSERR("thread pool task exception:", e.what());
                    if (task.completion)
                        task.completion->ret_code = RetCode::kException;
                }
                catch (...)
                {
                    SYSERR("thread pool task exception: unknown");
                    if (task.completion)
                        task.completion->ret_code = RetCode::kException;
                }
                if (task.completion) [[likely]]
                {
                    task.completion->notify_sema.signal();
                }
                locker.lock();
            }
            else
                pool->cond.wait(locker);
        }
    }
};
}
