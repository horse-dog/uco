#include "uco.h"
#include "uio.h"
#include "ulog.h"
#include "usync.h"
#include <chrono>
#include <cstdint>
using namespace uco;

const int thread_count = 10;
const int co_count = 100;
std::atomic<uint64_t> read_count;

task<void> reader(int reader_id, ushared_mutex& rwmtx, uint64_t& data)
{
    for (int i = 0; i < 100; i++)
    {
        uint64_t data_local = 0;
        co_await rwmtx.lock_shared();
        data_local = data;
        rwmtx.unlock_shared();
        LOGDBG("reader:", reader_id, "reads data:", data_local);
        ++read_count;
        co_await uco_sleep(std::chrono::milliseconds(random() % 20 + 1));
    }
}

task<void> writer(int writer_id, ushared_mutex& rwmtx, uint64_t& data)
{
    for (int i = 0; i < 10; i++)
    {
        uint64_t data_local = 0;
        co_await rwmtx.lock();
        data_local = ++data;
        rwmtx.unlock();
        LOGDBG("writer:", writer_id, "writes data:", data_local);
        co_await uco_sleep(std::chrono::milliseconds(random() % 100 + 30));
    }
}

void test_ushared_mutex(ushared_mutex& rwmtx, uint64_t& data)
{
    for (int i = 0; i < co_count; i++)
    {
        go reader(i, rwmtx, data);
        go writer(i, rwmtx, data);
    }
}

int main(int argc, const char* argv[])
{
    uco::OpenLog("test", LogLevel::DEBUG, LogMode::CONSOLE, false);
    ushared_mutex rwmtx;
    uint64_t data = 0;
    std::vector<std::thread> threads;
    for (int i = 0; i < thread_count; i++)
    {
        threads.emplace_back(test_ushared_mutex, std::ref(rwmtx), std::ref(data));
    }
    for (auto&& t : threads) t.join();
    LOGMSG(NR(data));
    LOGMSG(NR(read_count));
    if (data != thread_count * co_count * 10)
    {
        LOGFTL("read_count expect:", thread_count * co_count * 10, ',', "but get:", data);
    }
    if (read_count != thread_count * co_count * 100)
    {
        LOGFTL("read_count expect:", thread_count * co_count * 100, ',', "but get:", read_count);
    }
    return 0;
}
