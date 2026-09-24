#include "core/thread_pool.h"
#include "core/uco.h"
#include "core/uio.h"
#include <chrono>
#include <thread>

void ugly_add(int x, int y, int& result)
{
    std::this_thread::sleep_for(std::chrono::seconds(2));
    result = x + y;
}

uco::task<void> demo()
{
    uco::thread_pool pool(1);
    int result = 0;
    LOGMSG("BEGIN");
    go []() -> uco::task<void> {
        for (int i = 1; i <= 20; i++)
        {
            co_await uco_sleep(std::chrono::milliseconds(100));
            LOGDBG(NR(i));
        }
    }();
    int ret = co_await pool.execute(ugly_add, 3, 5, result);
    LOGMSG(NR(ret), NR(result));
    co_return;
}

int main()
{
    uco::OpenLog("test", LogLevel::DEBUG, LogMode::CONSOLE, false);
    go demo();
    return 0;
}
