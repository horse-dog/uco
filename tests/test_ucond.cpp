#include "core/uco.h"
#include "core/uio.h"
#include "core/ulog.h"
#include "core/usync.h"

using namespace uco;

// 本文件的局部 sema 只用于同一调度线程内的完成计数。usema 的共享内部
// 状态也允许 wait() 消费许可并析构外层 usema 时，已进入的 signal() 安全
// 返回。它不允许另一线程在 usema 已析构后才开始调用 signal()；跨线程时
// 仍须通过 join 或其他协议保证所有成员函数都在对象生命周期内开始调用。
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
