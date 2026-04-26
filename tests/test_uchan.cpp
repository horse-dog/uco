#include "usync.h"

using namespace uco;

// Test 1: Buffered channel (capacity=3)
task<void> producer(uchan<int, 3> &ch, usema &sema)
{
    for (int i = 1; i <= 5; i++)
    {
        LOGDBGF("Producer: sending %d", i);
        co_await (i >> ch);
        LOGDBGF("Producer: sent %d", i);
    }
    // close chan in single producer mode is safe.
    ch.close();
    LOGDBG("Producer: finished.");
    sema.signal();
}

task<void> consumer(uchan<int, 3> &ch, usema &sema)
{
    while (true)
    {
        int val = -1;
        auto ok = co_await (ch >> val);
        if (!ok)
        {
            LOGDBG("Consumer: received EOF, exiting.");
            break;
        }
        LOGDBGF("Consumer: received %d", val);
    }
    LOGDBG("Consumer: finished.");
    sema.signal();
}

task<void> test_chan_buffered()
{
    LOGMSG("=== test_chan_buffered ===");
    uchan<int, 3> ch;
    usema sema;

    go producer(ch, sema);
    go consumer(ch, sema);

    for (int i = 0; i < 2; i++)
    {
        co_await sema.wait();
    }
    LOGDBG("test_chan_buffered: All coroutines finished.");
}

// Test 2: Unbuffered channel (rendezvous, capacity=0)
task<void> producer_unbuf(uchan<int> &ch, usema &sema)
{
    for (int i = 1; i <= 3; i++)
    {
        LOGDBGF("Producer-unbuf: sending %d", i);
        co_await (i >> ch);
        LOGDBGF("Producer-unbuf: sent %d (consumer must have taken it)", i);
    }
    // close chan in single producer mode is safe.
    ch.close();
    LOGDBG("Producer-unbuf: finished.");
    sema.signal();
}

task<void> consumer_unbuf(uchan<int> &ch, usema &sema)
{
    while (true)
    {
        int val = -1;
        auto ok = co_await (ch >> val);
        if (!ok)
        {
            LOGDBG("Consumer-unbuf: received EOF, exiting.");
            break;
        }
        LOGDBGF("Consumer-unbuf: received %d", val);
    }
    LOGDBG("Consumer-unbuf: finished.");
    sema.signal();
}

task<void> test_chan_unbuffered()
{
    LOGMSG("=== test_chan_unbuffered ===");
    uchan<int> ch;
    usema sema;

    go producer_unbuf(ch, sema);
    go consumer_unbuf(ch, sema);

    for (int i = 0; i < 2; i++)
    {
        co_await sema.wait();
    }
    LOGDBG("test_chan_unbuffered: All coroutines finished.");
}

// Test 3: Multiple producers, single consumer (buffered)
task<void> multi_producer(uchan<std::string, 2> &ch, int id, usema &sema)
{
    for (int i = 1; i <= 3; i++)
    {
        std::string msg = "P" + std::to_string(id) + "_" + std::to_string(i);
        LOGDBGF("Producer-%d: sending %s", id, msg.c_str());
        co_await (msg >> ch);
        LOGDBGF("Producer-%d: sending %s OK", id, msg.c_str());
    }
    sema.signal();
}

task<void> multi_consumer(uchan<std::string, 2> &ch, int& count, int id, int total, usema &sema)
{
    while (true)
    {
        std::string val;
        auto ok = co_await (ch >> val);
        if (!ok)
        {
            LOGDBGF("Consumer-%d: finished.", id);
            break;
        }
        LOGDBGF("Consumer-%d: received [%d/%d] %s", id, ++count, total, val.c_str());
    }
    sema.signal();
}

task<void> test_chan_multi_buffered()
{
    LOGMSG("=== test_chan_multi_buffered ===");
    uchan<std::string, 2> ch;
    usema psema;
    usema csema;
    const int N_PRODUCERS = 3;
    const int N_CONSUMERS = 2;
    const int MSGS_PER_PRODUCER = 3;
    const int TOTAL_MSGS = N_PRODUCERS * MSGS_PER_PRODUCER;
    int count = 0;

    for (int i = 1; i <= N_PRODUCERS; i++)
    {
        go multi_producer(ch, i, psema);
    }
    for (int i = 1; i <= N_CONSUMERS; i++)
    {
        go multi_consumer(ch, count, i, TOTAL_MSGS, csema);
    }

    for (int i = 0; i < N_PRODUCERS; i++)
    {
        co_await psema.wait();
    }
    ch.close();

    for (int i = 0; i < N_CONSUMERS; i++)
    {
        co_await csema.wait();
    }
    LOGDBG("test_chan_multi_buffered: All coroutines finished.");
}

task<void> multi_producer_unbuffered(uchan<std::string> &ch, int id, usema &sema)
{
    for (int i = 1; i <= 3; i++)
    {
        std::string msg = "P" + std::to_string(id) + "_" + std::to_string(i);
        LOGDBGF("Producer-%d: sending %s", id, msg.c_str());
        co_await (msg >> ch);
        LOGDBGF("Producer-%d: sending %s OK", id, msg.c_str());
    }
    sema.signal();
}

task<void> multi_consumer_unbuffered(uchan<std::string> &ch, int& count, int id, int total, usema &sema)
{
    while (true)
    {
        std::string val;
        auto ok = co_await (ch >> val);
        if (!ok)
        {
            LOGDBGF("Consumer-%d: finished.", id);
            break;
        }
        LOGDBGF("Consumer-%d: received [%d/%d] %s", id, ++count, total, val.c_str());
    }
    sema.signal();
}

task<void> test_chan_multi_unbuffered()
{
    LOGMSG("=== test_chan_multi_unbuffered ===");
    uchan<std::string> ch;
    usema psema;
    usema csema;
    const int N_PRODUCERS = 3;
    const int N_CONSUMERS = 2;
    const int MSGS_PER_PRODUCER = 3;
    const int TOTAL_MSGS = N_PRODUCERS * MSGS_PER_PRODUCER;
    int count = 0;

    for (int i = 1; i <= N_PRODUCERS; i++)
    {
        go multi_producer_unbuffered(ch, i, psema);
    }
    for (int i = 1; i <= N_CONSUMERS; i++)
    {
        go multi_consumer_unbuffered(ch, count, i, TOTAL_MSGS, csema);
    }

    for (int i = 0; i < N_PRODUCERS; i++)
    {
        co_await psema.wait();
    }
    ch.close();

    for (int i = 0; i < N_CONSUMERS; i++)
    {
        co_await csema.wait();
    }
    LOGDBG("test_chan_multi_unbuffered: All coroutines finished.");
}

// Run all tests
task<void> test_all()
{
    co_await test_chan_buffered();
    co_await test_chan_unbuffered();
    co_await test_chan_multi_buffered();
    co_await test_chan_multi_unbuffered();
    LOGMSG("=== All channel tests passed! ===");
}

int main(int argc, char *argv[])
{
    uco::OpenLog("test", LogLevel::DEBUG, LogMode::CONSOLE, false);
    go test_all();
    return 0;
}
