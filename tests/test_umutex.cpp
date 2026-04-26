#include "uco.h"
#include "ulog.h"
#include "usync.h"
#include <cstdint>
#include <thread>
#include <vector>
using namespace uco;

const int ACC_COUNT = 1'000'000;
const int thread_count = 4;
const int co_count = 5;

task<void> accumulate(umutex& mtx, uint64_t& count)
{
    for (int i = 0; i < ACC_COUNT; i++)
    {
        co_await mtx.lock();
        ++count;
        mtx.unlock();
    }
}

void test_umutex(umutex& mtx, uint64_t& count)
{
    for (int i = 0; i < co_count; i++)
    {
        go accumulate(mtx, count);
    }
}

int main(int argc, const char* argv[])
{
    uco::OpenLog("test", LogLevel::INFO, LogMode::CONSOLE, false);
    umutex mtx;
    uint64_t count = 0;

    // 4 线程，每个线程 5 个 accumulate 协程. 共 20 个协程.
    std::vector<std::thread> threads;
    for (int i = 0; i < thread_count; i++)
    {
        threads.emplace_back(test_umutex, std::ref(mtx), std::ref(count));
    }
    for (auto&& t : threads) t.join();
    LOGMSG(NR(count));
    if (count != thread_count * co_count * ACC_COUNT)
    {
        LOGFTL("expect:", thread_count * co_count * ACC_COUNT, ',', "but get:", count);
    }
    return 0;
}
