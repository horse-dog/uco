
#include "uco.h"
#include "uio.h"
#include "ulog.h"
#include "usync.h"
#include <cstdio>
#include <thread>
using namespace uco;

class Global
{
  public:
    ~Global() { printf("dtor\n"); }
};

Global gb;

task<void> demo()
{
    cobatch batchrunner(20);
    for (int i = 0; i < 10; i++)
    {
        batchrunner.add([](int i) -> task<void> { 
          co_await uco_nanosleep(1, 0); 
          if (i == 2) throw std::runtime_error("error"); 
          LOGMSG("hello"); 
          co_return;
        }(i));
    }
    co_await batchrunner.run();
}

task<void> demo1()
{
    cobatch batchrunner(2);
    for (int i = 0; i < 2; i++)
    {
        batchrunner.add(demo());
    }
    co_await batchrunner.run();

    LOGMSG("finish");
    co_await demo();
}

struct Demo
{
  Demo(int xx) { x = xx; LOGMSG("ctor"); }
  ~Demo() { LOGERR("dtor"); }
  Demo(const Demo& o) { x = o.x; LOGMSG("const Demo&"); }
  Demo(Demo&& o) { std::swap(x, o.x); LOGMSG("Demo&&"); }

  int x = 0;
};

void demo2(const Demo& demo)
{
    LOGMSG("x is:", demo.x);
}

int main(int argc, const char *argv[])
{
    uco::OpenLog("test", LogLevel::INFO, LogMode::CONSOLE, true);
    go demo1();
    go []() -> task<void> {
        auto start = std::chrono::steady_clock::now();
        co_await uco_nanosleep(15, 0);
        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        LOGMSG("hello world!", duration.count());
        co_return;
    }();

    uthread t([] {
        go []() -> task<void> {
            auto start = std::chrono::steady_clock::now();
            co_await uco_nanosleep(20, 0);
            auto end = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            LOGMSG("hello world!", duration.count());
            co_return;
        }();
    });

    uthread t1([] {
        printf("hello world!\n");
    });

    Demo dd(123);
    uthread t2(demo2, dd);
    std::thread t3(demo2, dd);
    t.daemonize();
    t1.daemonize();
    t2.daemonize();
    t3.join();
    return 0;
}
