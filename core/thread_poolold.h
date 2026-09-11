#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "core/uco.h"
#include "core/ulog.h"
#include "core/usync.h"

// 线程池
class ThreadPool
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
    explicit ThreadPool(size_t thread_num, size_t max_pending = 1024)
        : m_pool(std::make_shared<Pool>())
    {
        m_num_workers.store(thread_num);
        m_pool->max_pending = max_pending;
        // if (thread_num == 0)
        // {
        //     LOGFTL("ThreadPool: thread_num must be > 0");
        // }
        for (size_t i = 0; i < thread_num; ++i)
        {
            m_threads.emplace_back(Loop, m_pool);
        }
    }

    /**
     * @brief 构造函数
     */
    ThreadPool() = default;

    /**
     * @brief 移动构造函数
     */
    ThreadPool(ThreadPool &&) = delete;

    /**
     * @brief 析构函数: 拒收新任务, 队列中未执行任务丢弃并标 kExit 通知等待方,
     *        join 全部线程后返回.
     * @note  析构返回 = 在执行任务已结束, 未执行任务已被拒; 不得在任务内析构池
     *       (join 自身死锁), 亦不得在任务中持续投递新任务.
     */
    ~ThreadPool()
    {
        if (static_cast<bool>(m_pool))
        {
            {
                std::lock_guard<std::mutex> locker(m_pool->mtx);
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
     * @brief 添加一个待执行任务
     * @param task 待执行任务
     * @return 投递结果: true = 已入队; false = 被拒 (ret_code 已置 kExit)
     */
    template <class F>
    bool addTask(F &&task, int *ret_code = 0, uco::usema *sem = 0)
    {
        if (m_num_workers.load() == 0)
        {
            LOGERR("No worker");
            if (ret_code)
                *ret_code = RetCode::kBusy;
            return false;
        }
        if (!m_pool)
        {
            LOGERR("ThreadPool not initialized, reject");
            if (ret_code)
                *ret_code = RetCode::kExit;
            return false;
        }
        {
            std::lock_guard<std::mutex> locker(m_pool->mtx);
            if (m_pool->isClosed)
            {
                LOGERR("Pool exit, reject");
                if (ret_code)
                    *ret_code = RetCode::kExit;
                return false;
            }
            if (m_pool->tasks.size() >= m_pool->max_pending)
            {
                SYSERR("Pool overloaded, reject");
                if (ret_code)
                    *ret_code = RetCode::kBusy;
                return false;
            }
            m_pool->tasks.emplace(ret_code, sem, std::forward<F>(task));
        }
        m_pool->cond.notify_one();
        return true;
    }

    template <class F> uco::task<int> execute(F &&task)
    {
        uco::usema sem;
        int ret_code = 0;
        if (!addTask(std::forward<F>(task), &ret_code,
                     &sem)) // 投递被拒, 不同步等待.
        {
            co_return ret_code;
        }
        co_await sem.wait();
        co_return ret_code;
    }

    void AddCurrentThread()
    {
        ++m_num_workers;
        Loop(m_pool);
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
        std::mutex mtx; // 任务池锁,保证任务队列存取的线程安全
        std::condition_variable cond; // 任务池条件变量, 任务队列空时阻塞线程
        bool isClosed = false;     // 任务池是否关闭
        size_t max_pending = 1024; // 队列积压上限 (背压拒收)
        std::queue<Task> tasks;    // 任务队列
    };
    std::shared_ptr<Pool> m_pool;       // 任务池
    std::vector<std::thread> m_threads; // 工作线程 (析构 join)
    std::atomic<int> m_num_workers = 0;

  private:
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
                locker.lock();
            }
            else
                pool->cond.wait(locker);
        }
    }
};
