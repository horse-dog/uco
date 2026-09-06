// usleeper 竞态与跨线程场景验证:
// 基础睡眠路径 / 醒着时唤醒 / 跨线程唤醒 / 唤醒已到期的睡眠 /
// 唤醒与到期同时发生 / 多定时器堆重排 / 竞态压力 / 冲突睡眠 / 析构安全.
#include "usync.h"
#include "uio.h"
#include "ulog.h"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace std::chrono;
using timer_result = uco::usleeper::result;

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

// ==================== 唤醒者 ====================

/// 协程: 延时 ms 后 wake() (与同线程的其他睡眠并发).
static uco::task<void> waker_after(uco::usleeper &t, milliseconds ms)
{
    co_await uco_sleep(ms);
    t.wake();
}

/// 外部线程入口: 延时 ms 后从外部线程 wake().
static void wake_after_delay(uco::usleeper *t, milliseconds ms)
{
    std::this_thread::sleep_for(ms);
    t->wake();
}

/// 外部线程入口: 立即 wake(). 此刻无睡眠, 应记为待处理请求.
static void wake_immediately(uco::usleeper *t)
{
    t->wake();
}

/// 记录一次睡眠的结果, 供多定时器用例汇总断言.
struct sleep_outcome
{
    timer_result r = timer_result::EXPIRED; ///< 睡眠结果.
    int64_t elapsed_ms = 0;                 ///< 实际经过时长.
};

/// 协程: 计时睡眠并把结果写入 out.
static uco::task<void> timed_sleep(uco::usleeper &t, milliseconds ms,
                                   sleep_outcome *out)
{
    auto begin = steady_clock::now();
    out->r = co_await t.sleep_for(ms);
    out->elapsed_ms = ms_since(begin);
}

// ==================== 用例 ====================

/// 1. 基础路径: 睡满, 以及被同线程协程 wake 打断.
static uco::task<void> case_basic(uco::usleeper &t)
{
    auto begin = steady_clock::now();
    auto r = co_await t.sleep_for(120ms);
    auto elapsed = ms_since(begin);
    CHECK(r == timer_result::EXPIRED, "basic: slept through");
    CHECK(elapsed >= 110 && elapsed < 400, "basic: elapsed ~120ms, got",
          elapsed, "ms");

    go waker_after(t, 60ms);
    begin = steady_clock::now();
    r = co_await t.sleep_for(1h);
    elapsed = ms_since(begin);
    CHECK(r == timer_result::WOKEN, "basic: interrupted by wake");
    CHECK(elapsed < 1000, "basic: fast interrupt, got", elapsed, "ms");
}

/// 2. 醒着时 wake (还没开始 sleep 的 timer): 记为待处理请求.
static uco::task<void> case_wake_before_sleep(uco::usleeper &t)
{
    t.wake();
    t.wake();
    t.wake(); // 醒着连 wake 三次: 待处理计数累积.
    auto begin = steady_clock::now();
    auto r = co_await t.sleep_for(1h);
    auto elapsed = ms_since(begin);
    CHECK(r == timer_result::WOKEN,
          "wake-before-sleep: next sleep returns immediately");
    CHECK(elapsed < 100, "wake-before-sleep: fast, got", elapsed, "ms");

    // 计数已被上一次睡眠消费, 之后应恢复正常睡满.
    begin = steady_clock::now();
    r = co_await t.sleep_for(150ms);
    elapsed = ms_since(begin);
    CHECK(r == timer_result::EXPIRED,
          "wake-before-sleep: requests consumed, next sleeps full");
    CHECK(elapsed >= 140, "wake-before-sleep: second elapsed ~150ms, got",
          elapsed, "ms");
}

/// 3. 跨线程唤醒: 外部线程打断挂起中的睡眠; 醒着时的跨线程请求同样有效.
static uco::task<void> case_cross_thread_wake(uco::usleeper &t)
{
    // 外部线程 50ms 后 wake; sleeper 被打断的时刻即 wake 生效的证据,
    // 无需等待线程结束.
    auto begin = steady_clock::now();
    uco::uthread th(wake_after_delay, &t, 50ms);
    th.daemonize(); // 托管后台, 程序收尾时统一释放.
    auto r = co_await t.sleep_for(1h);
    auto elapsed = ms_since(begin);
    CHECK(r == timer_result::WOKEN,
          "cross-thread: sleep interrupted from another thread");
    CHECK(elapsed >= 40 && elapsed < 1000,
          "cross-thread: fast interrupt, got", elapsed, "ms");

    // 醒着时从外部线程 wake: 无论落在睡眠前还是睡眠中, 结果一致.
    uco::uthread th2(wake_immediately, &t);
    th2.daemonize();
    begin = steady_clock::now();
    r = co_await t.sleep_for(1h);
    elapsed = ms_since(begin);
    CHECK(r == timer_result::WOKEN, "cross-thread: pending request honored");
    CHECK(elapsed < 100, "cross-thread: next sleep fast, got", elapsed,
          "ms");
}

/// 4. 唤醒一个已经超时的 timer: wake 迟于到期到达, 本次睡满,
///    输掉的 wake 转为待处理请求, 不丢失.
static uco::task<void> case_wake_after_expiry(uco::usleeper &t)
{
    uco::uthread th(wake_after_delay, &t, 80ms);
    th.daemonize();
    auto begin = steady_clock::now();
    auto r = co_await t.sleep_for(30ms); // 30ms 睡满, wake 在 80ms 才到.
    auto elapsed = ms_since(begin);
    CHECK(r == timer_result::EXPIRED, "wake-after-expiry: slept through");
    CHECK(elapsed >= 25 && elapsed < 200,
          "wake-after-expiry: elapsed ~30ms, got", elapsed, "ms");

    // 睡到 wake 必然已到达 (30 + 60 > 80ms), 此时 timer 醒着,
    // wake 应已记为待处理.
    co_await uco_sleep(60ms);
    begin = steady_clock::now();
    r = co_await t.sleep_for(1h);
    elapsed = ms_since(begin);
    CHECK(r == timer_result::WOKEN,
          "wake-after-expiry: lost wake becomes pending request");
    CHECK(elapsed < 100, "wake-after-expiry: next sleep fast, got",
          elapsed, "ms");

    // 消费后恢复正常.
    begin = steady_clock::now();
    r = co_await t.sleep_for(100ms);
    elapsed = ms_since(begin);
    CHECK(r == timer_result::EXPIRED,
          "wake-after-expiry: state clean, sleeps full again");
    CHECK(elapsed >= 90, "wake-after-expiry: last elapsed ~100ms, got",
          elapsed, "ms");
}

/// 5. 唤醒与到期同时发生: 多轮让外部线程在到期时刻附近 wake,
///    仲裁结果二选一皆合法, 但必须与实际经过时间自洽, 且不崩溃.
static uco::task<void> case_race_wake_vs_expiry(uco::usleeper &t)
{
    const int rounds = 30;
    const milliseconds span = 20ms;
    int expired = 0;
    int woken = 0;
    for (int i = 0; i < rounds; i++)
    {
        auto begin = steady_clock::now();
        // wake 时刻在 16~25ms 抖动, 与 20ms 到期窗口重叠.
        uco::uthread th(wake_after_delay, &t,
                        milliseconds(16 + i % 10));
        th.daemonize();
        auto r = co_await t.sleep_for(span);
        auto elapsed = ms_since(begin);
        if (r == timer_result::EXPIRED)
        { // 到期赢: 至少接近睡满时长.
            ++expired;
            CHECK(elapsed >= span.count() - 2,
                  "race: expired result but elapsed only", elapsed, "ms");
        }
        else
        { // 唤醒赢: 不应明显超过到期时刻 (允许调度误差).
            ++woken;
            CHECK(elapsed < span.count() + 30,
                  "race: woken result but elapsed", elapsed, "ms");
        }
        // 睡过最迟 wake 时刻 (20 + 15 > 25ms): 迟到而输掉的 wake
        // 在本轮内转为待处理, 不泄漏到下一轮.
        co_await t.sleep_for(15ms);
    }
    CHECK(expired + woken == rounds, "race: every round adjudicated once");
    LOGMSG("race: distribution expired", expired, "woken", woken);

    // 大量仲裁后状态应干净: 正常睡满.
    auto begin = steady_clock::now();
    auto r = co_await t.sleep_for(80ms);
    auto elapsed = ms_since(begin);
    CHECK(r == timer_result::EXPIRED && elapsed >= 70,
          "race: timer state clean after", rounds, "races");
}

/// 6. 多定时器并存: 堆顶切换 (插入更早的到期时刻) 与
///    唤醒非堆顶节点 (摘除后堆顶不变).
static uco::task<void> case_multiple_timers()
{
    uco::usleeper t1, t2, t3;
    sleep_outcome r1, r2, r3;

    // 插入顺序与到期顺序刻意不同:
    //   t1 (200ms) 先入堆 -> t2 (100ms) 更早, 触发堆顶切换
    //   -> t3 (150ms) 非堆顶 -> t3 在 50ms 时被 wake (非堆顶摘除).
    go timed_sleep(t1, 200ms, &r1);
    go timed_sleep(t2, 100ms, &r2);
    go timed_sleep(t3, 150ms, &r3);
    go waker_after(t3, 50ms);
    co_await uco_sleep(300ms); // 等三段睡眠全部结束.

    CHECK(r2.r == timer_result::EXPIRED && r2.elapsed_ms >= 90 &&
              r2.elapsed_ms < 200,
          "timers: t2 (heap head switch) slept full ~100ms, got",
          r2.elapsed_ms, "ms");
    CHECK(r1.r == timer_result::EXPIRED && r1.elapsed_ms >= 190 &&
              r1.elapsed_ms < 320,
          "timers: t1 slept full ~200ms, got", r1.elapsed_ms, "ms");
    CHECK(r3.r == timer_result::WOKEN && r3.elapsed_ms >= 40 &&
              r3.elapsed_ms < 130,
          "timers: t3 (non-head node) woken at ~50ms, got",
          r3.elapsed_ms, "ms");
    CHECK(r2.elapsed_ms < r1.elapsed_ms,
          "timers: expiry order preserved (t2 before t1)");
}

/// 7. 竞态压力: 多轮短睡 + 交错 wake, 验证无崩溃与状态无泄漏.
static uco::task<void> case_stress(uco::usleeper &t)
{
    const int rounds = 50;
    for (int i = 0; i < rounds; i++)
    {
        // wake 时刻在 1~10ms 抖动, 与 5ms 到期反复交错.
        uco::uthread th(wake_after_delay, &t, milliseconds(1 + i % 10));
        th.daemonize();
        co_await t.sleep_for(5ms);
        // 睡过最迟 wake 时刻 (5 + 15 > 10ms): 吸收转待处理的 wake.
        co_await t.sleep_for(15ms);
    }
    auto begin = steady_clock::now();
    auto r = co_await t.sleep_for(80ms);
    auto elapsed = ms_since(begin);
    CHECK(r == timer_result::EXPIRED && elapsed >= 70,
          "stress: clean state after", rounds, "interleaved rounds");
}

/// 8. 冲突睡眠: A 挂起中, B 再睡同一 timer. B 得到 BUSY 而非 core,
///    A 不受影响; 槽位在 A 恢复后正常释放.
static uco::task<void> case_sleep_conflict(uco::usleeper &t)
{
    sleep_outcome ra;
    go timed_sleep(t, 150ms, &ra); // A: 睡 150ms.
    co_await uco_sleep(20ms);      // 等 A 挂起.

    auto begin = steady_clock::now();
    auto r = co_await t.sleep_for(1h); // B: 槽位被占, 得 BUSY.
    auto elapsed = ms_since(begin);
    CHECK(r == timer_result::BUSY,
          "conflict: second sleep rejected with BUSY");
    CHECK(elapsed < 100, "conflict: rejection is immediate, got",
          elapsed, "ms");

    // A 不受影响: 照常睡满.
    co_await uco_sleep(200ms);
    CHECK(ra.r == timer_result::EXPIRED && ra.elapsed_ms >= 140 &&
              ra.elapsed_ms < 260,
          "conflict: first sleeper unaffected, got", ra.elapsed_ms, "ms");

    // 槽位已随 A 的恢复释放: 后续睡眠正常.
    begin = steady_clock::now();
    r = co_await t.sleep_for(80ms);
    elapsed = ms_since(begin);
    CHECK(r == timer_result::EXPIRED && elapsed >= 70,
          "conflict: slot released, sleeps full again");
}

/// 9. 析构安全: 醒着 (含待处理请求) 析构, 程序正常收尾.
static uco::task<void> case_destroy()
{
    {
        uco::usleeper t;
        t.wake(); // 无睡眠时 wake: 应被安全吸收.
        co_await uco_sleep(10ms);
    } // 析构.
    co_await uco_sleep(10ms);
    LOGMSG("destroy: timer destroyed cleanly");
}

// ==================== 主流程 ====================

static uco::task<void> demo()
{
    uco::usleeper t;
    co_await case_basic(t);
    co_await case_wake_before_sleep(t);
    co_await case_cross_thread_wake(t);
    co_await case_wake_after_expiry(t);
    co_await case_race_wake_vs_expiry(t);
    co_await case_multiple_timers();
    co_await case_stress(t);
    co_await case_sleep_conflict(t);
    co_await case_destroy();
    LOGMSG("usleeper: all cases done, pass", g_pass, "fail", g_fail);
}

int main()
{
    uco::OpenLog("test_usleeper", LogLevel::INFO, LogMode::CONSOLE, true);
    go demo();
    return 0;
}
