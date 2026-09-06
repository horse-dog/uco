// utimer (单线程模型) 功能验证:
// 睡满 / wake 取消 / update 重睡 / until 睡满·已过·被打断 /
// 醒着时的唤醒请求 / 析构安全.
#include "usync.h"
#include "uio.h"
#include "ulog.h"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace std::chrono;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, ...)                                                       \
    do                                                                         \
    {                                                                          \
        if (cond)                                                              \
        {                                                                      \
            ++g_pass;                                                          \
            LOGMSG("[PASS]", __VA_ARGS__);                                     \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            ++g_fail;                                                          \
            LOGERR("[FAIL]", __VA_ARGS__);                                     \
        }                                                                      \
    } while (0)

static int64_t ms_since(const steady_clock::time_point &begin)
{
    return duration_cast<milliseconds>(steady_clock::now() - begin).count();
}

/// 1. 正常睡满 (chrono duration).
static uco::task<void> case_full_sleep(uco::utimer &t)
{
    auto begin = steady_clock::now();
    bool full = co_await t.sleep_for(300ms);
    auto elapsed = ms_since(begin);
    CHECK(full, "full sleep: slept through");
    CHECK(elapsed >= 280 && elapsed < 600, "full sleep: elapsed ~300ms, got ",
          elapsed, "ms");
}

/// 2. wake 打断挂起中的睡眠.
static uco::task<void> case_wake(uco::utimer &t)
{
    auto begin = steady_clock::now();
    bool full = co_await t.sleep_for(1h);
    auto elapsed = ms_since(begin);
    CHECK(!full, "wake: interrupted, not full");
    CHECK(elapsed < 1000, "wake: fast interrupt, got ", elapsed, "ms");
}

static uco::task<void> waker_after(uco::utimer &t, milliseconds ms)
{
    co_await uco_sleep(ms);
    t.wake();
}

/// 3. update: 打断并携带新时长, sleeper 重睡新时长后睡满.
static uco::task<void> case_update(uco::utimer &t)
{
    auto begin = steady_clock::now();
    bool full = co_await t.sleep_for(1h);
    CHECK(!full, "update: first sleep interrupted");
    nanoseconds d{};
    CHECK(t.take_pending(d), "update: pending taken");
    CHECK(d == 200ms, "update: new duration is 200ms, got ", d.count(), "ns");

    bool full2 = co_await t.sleep_for(d);
    auto elapsed = ms_since(begin);
    CHECK(full2, "update: second sleep full");
    CHECK(elapsed >= 280 && elapsed < 900,
          "update: total ~100+200ms, got ", elapsed, "ms");
}

static uco::task<void> updater_after(uco::utimer &t, milliseconds ms,
                                     milliseconds newd)
{
    co_await uco_sleep(ms);
    t.update(newd);
}

/// 4. sleep_until 睡满: 到达绝对时刻.
static uco::task<void> case_until_full(uco::utimer &t)
{
    auto begin = steady_clock::now();
    bool reached = co_await t.sleep_until(begin + 150ms);
    auto elapsed = ms_since(begin);
    CHECK(reached, "until-full: reached target time");
    CHECK(elapsed >= 130 && elapsed < 400, "until-full: elapsed ~150ms, got ",
          elapsed, "ms");
}

/// 5. sleep_until 已过期: 目标时刻在过去, 立即返回 true.
static uco::task<void> case_until_past(uco::utimer &t)
{
    auto begin = steady_clock::now();
    bool reached = co_await t.sleep_until(begin - 1s);
    auto elapsed = ms_since(begin);
    CHECK(reached, "until-past: returns true immediately");
    CHECK(elapsed < 50, "until-past: fast, got ", elapsed, "ms");

    // 已过期不消费唤醒请求: 下一次睡眠应正常进行.
    auto begin2 = steady_clock::now();
    bool full = co_await t.sleep_for(100ms);
    auto elapsed2 = ms_since(begin2);
    CHECK(full, "until-past: no request consumed, next sleeps full");
    CHECK(elapsed2 >= 80, "until-past: second elapsed ~100ms, got ",
          elapsed2, "ms");
}

/// 6. sleep_until 被 wake 打断.
static uco::task<void> case_until_wake(uco::utimer &t)
{
    auto begin = steady_clock::now();
    bool reached =
        co_await t.sleep_until(begin + 1h); // 一小时后的绝对时刻.
    auto elapsed = ms_since(begin);
    CHECK(!reached, "until-wake: interrupted, not reached");
    CHECK(elapsed < 1000, "until-wake: fast, got ", elapsed, "ms");
}

/// 7. 醒着时 wake(): 记为待处理请求, 下一次睡眠立即返回 false.
static uco::task<void> case_wake_while_awake(uco::utimer &t)
{
    t.wake(); // 此刻无睡眠: 仅记请求.
    auto begin = steady_clock::now();
    bool full = co_await t.sleep_for(1h);
    auto elapsed = ms_since(begin);
    CHECK(!full, "wake-while-awake: next sleep returns immediately");
    CHECK(elapsed < 100, "wake-while-awake: fast, got ", elapsed, "ms");

    auto begin2 = steady_clock::now();
    bool full2 = co_await t.sleep_for(150ms);
    auto elapsed2 = ms_since(begin2);
    CHECK(full2, "wake-while-awake: request consumed, next sleeps full");
    CHECK(elapsed2 >= 130, "wake-while-awake: second elapsed ~150ms, got ",
          elapsed2, "ms");
}

/// 8. 析构安全: 无睡眠时析构, 程序正常收尾 (生命周期契约).
static uco::task<void> case_destroy()
{
    {
        uco::utimer t;
        t.wake(); // 无睡眠时 wake: 应被安全吸收.
        co_await uco_sleep(10ms);
    } // 析构.
    co_await uco_sleep(10ms);
    LOGMSG("destroy: timer destroyed cleanly");
}

/// 9. 跨线程调用被拒绝并安全降级: wake 落空, sleeper 睡满自然醒
///    (等价于旧轮询版行为, 而非数据竞争 UB).
static uco::task<void> case_cross_thread_rejected(uco::utimer &t)
{
    auto begin = steady_clock::now();
    std::thread th([&t] {
        std::this_thread::sleep_for(50ms); // 等 sleeper 挂起.
        t.wake(); // 违反单线程契约: 应被拒绝并记 SYSERR.
    });
    bool full = co_await t.sleep_for(200ms);
    th.join();
    auto elapsed = ms_since(begin);
    CHECK(full, "cross-thread: wake rejected, sleeper slept through");
    CHECK(elapsed >= 180 && elapsed < 500,
          "cross-thread: degraded to full sleep, got ", elapsed, "ms");
}

uco::task<void> demo()
{
    uco::utimer t;
    co_await case_full_sleep(t);
    go waker_after(t, 100ms);
    co_await case_wake(t);
    go updater_after(t, 100ms, 200ms);
    co_await case_update(t);
    co_await case_until_full(t);
    co_await case_until_past(t);
    go waker_after(t, 100ms);
    co_await case_until_wake(t);
    co_await case_wake_while_awake(t);
    co_await case_destroy();
    co_await case_cross_thread_rejected(t);
}

int main()
{
    uco::OpenLog("test_utimer", LogLevel::INFO, LogMode::CONSOLE, true);
    go demo();
    return 0;
}
