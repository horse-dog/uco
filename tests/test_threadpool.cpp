#include "core/thread_pool.h"
#include "core/uco.h"
#include "core/usync.h"
#include <chrono>
#include <thread>

void ugly_add(int x, int y, int& result)
{
    std::this_thread::sleep_for(std::chrono::seconds(2));
    result = x + y;
}

uco::task<void> run_task(uco::uthread_pool& pool)
{
    int result = 0;
    int ret = co_await pool.execute(ugly_add, 3, 5, result);
    LOGMSG(NR(ret), NR(result));
    co_await pool.close();
    co_return;
}

uco::task<void> demo()
{
    uco::uthread_pool pool(0);

    uco::cobatch batchrunner;
    batchrunner.add(pool.add_current());
    batchrunner.add(run_task(pool));
    co_await batchrunner.run();

    co_await pool.close();
    co_return;
}

int main()
{
    uco::OpenLog("test", LogLevel::INFO, LogMode::CONSOLE, true);
    go demo();
    return 0;
}
