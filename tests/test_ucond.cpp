#include "uco.h"
#include "uio.h"
#include "ulog.h"
#include "usync.h"

using namespace uco;

task<void> worker(bool sleep, umutex& mtx, ucond& cv, usema& sema, bool& is_ready, bool notify_all=false)
{
    LOGMSG("worker coroutine start.");
    if (sleep) co_await uco_nanosleep(2, 0);
    co_await mtx.lock();
    is_ready = true;
    notify_all ? cv.notify_all() : cv.notify_one();
    mtx.unlock();
    LOGMSG("worker coroutine finished.");
    sema.signal();
}

task<void> waiter(umutex& mtx, ucond& cv, usema& sema, bool& is_ready)
{
    LOGMSG("waiting for worker finished.");
    {
        auto lock = co_await unique_ulock(mtx);
        co_await cv.wait(lock, [&is_ready] { return is_ready; });
    }
    LOGMSG("waiting for worker finished OK.");
    sema.signal();
}

task<void> test_cond()
{
    LOGMSG("------------- TEST: notify_one -------------");
    umutex mtx;
    ucond cv;
    usema sema;
    bool is_ready = false;
    go waiter(mtx, cv, sema, is_ready);
    go worker(true, mtx, cv, sema, is_ready);
    for (int i = 0; i < 2; i++)
    {
        co_await sema.wait();
    }
    LOGMSG("all coroutines finished.");

    LOGMSG("------------- TEST: notify_all -------------");
    is_ready = false;
    go waiter(mtx, cv, sema, is_ready);
    go waiter(mtx, cv, sema, is_ready);
    go waiter(mtx, cv, sema, is_ready);
    go worker(false, mtx, cv, sema, is_ready, true);
    for (int i = 0; i < 4; i++)
    {
        co_await sema.wait();
    }
    LOGMSG("all coroutines finished.");
}

int main(int argc, const char* argv[])
{
    uco::OpenLog("test", LogLevel::INFO, LogMode::CONSOLE, false);
    go test_cond();
    return 0;
}
