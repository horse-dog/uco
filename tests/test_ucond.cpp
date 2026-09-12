#include "core/uco.h"
#include "core/uio.h"
#include "core/ulog.h"
#include "core/usync.h"

using namespace uco;

// 本文件的局部 sema 只用于同一调度线程内的完成计数：所有子协程均由
// 当前线程的 go 启动，signal() 完整返回后，调度器才可能恢复父协程，
// 因此父协程退出并析构 sema 时，不会有并行执行中的 signal()。
// 若将 worker/waiter 移到其他 OS 线程，这种写法可能产生 UB：wait()
// 消费许可后父协程可能立即析构 sema，而另一线程的 signal() 尚未返回、
// 仍可能访问 sema。跨线程时必须用共享所有权，或先 join 生产者线程。
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
