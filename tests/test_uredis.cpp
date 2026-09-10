#include "core/ulog.h"
#include "core/uredis.h"
#include "core/uio.h"
#include "core/usync.h"

#include <string>
#include <vector>

using namespace uco;
using namespace std::chrono_literals;

// ==================== 基础命令: set/get/nil/二进制/注入 ====================
task<void> demo1(uredis::upool &pool)
{
    auto *c = co_await pool.Acquire();
    uredis::UredisGuard guard(pool, c);
    if (c == nullptr)
    {
        LOGERR("acquire failed");
        co_return;
    }

    auto pong = co_await uredis::uping(c);
    LOGMSG("ping:", pong.str, "ret_code:", pong.ret_code);

    auto sr = co_await uredis::uset(c, "uco:test:str", "hello");
    LOGMSG("set:", sr.str, "ret_code:", sr.ret_code);

    auto gr = co_await uredis::uget(c, "uco:test:str");
    LOGMSG("get:", gr.str, "type:", (int)gr.type);

    gr = co_await uredis::uget(c, "uco:test:missing");
    LOGMSG("get missing type(NIL=4):", (int)gr.type);

    // 二进制安全: value 含 \r\n 不破坏 RESP 帧.
    std::string bin = "a\r\nb\x01";
    co_await uredis::uset(c, "uco:test:bin", bin);
    gr = co_await uredis::uget(c, "uco:test:bin");
    LOGMSG("binary-safe:", NR(gr.str == bin));

    // 注入免疫: 参数数组下发, 特殊字符不拆分不转义.
    co_await uredis::uset(c, "uco:test:inj", "a b 'c' \"d\" $X");
    gr = co_await uredis::uget(c, "uco:test:inj");
    LOGMSG("injection-safe:", NR(gr.str == "a b 'c' \"d\" $X"));
}

// ==================== 数组回复/整数/服务端错误 ====================
task<void> demo2(uredis::upool &pool)
{
    auto *c = co_await pool.Acquire();
    uredis::UredisGuard guard(pool, c);
    if (c == nullptr)
    {
        LOGERR("acquire failed");
        co_return;
    }

    co_await uredis::udel(c, {"uco:test:list"});
    auto ar = co_await uredis::ucommand(c, {"LPUSH", "uco:test:list", "a", "b", "c"});
    LOGMSG("lpush:", ar.integer);

    ar = co_await uredis::ucommand(c, {"LRANGE", "uco:test:list", "0", "-1"});
    if (ar.type == uredis::Reply::ARRAY)
    {
        std::string items;
        for (auto &e : ar.elements)
        {
            items += e.str + " ";
        }
        LOGMSG("lrange:", items);
    }

    // 整数回复.
    co_await uredis::udel(c, {"uco:test:cnt"});
    auto ir = co_await uredis::uincr(c, "uco:test:cnt");
    LOGMSG("incr:", ir.integer);
    ir = co_await uredis::uincr(c, "uco:test:cnt");
    LOGMSG("incr:", ir.integer);

    // 服务端错误回复: 对 string key 执行 LPUSH -> WRONGTYPE.
    auto er = co_await uredis::ucommand(c, {"LPUSH", "uco:test:str", "x"});
    LOGMSG("error reply:", er.err_msg, "ret_code:", er.ret_code);

    auto dr = co_await uredis::udel(c, {"uco:test:cnt", "uco:test:list",
                                        "uco:test:str", "uco:test:bin",
                                        "uco:test:inj"});
    LOGMSG("del:", dr.integer);
}

// ==================== 分布式锁 ====================

/// 阻塞抢锁方: 等 releaser 放锁后 acquire 成功.
task<void> lock_waiter(uredis::ulock &l)
{
    auto ok = co_await l.Acquire({2, 0});
    LOGMSG("blocking acquire:", NR(ok), "owns:", NR(l.Owns()));
    co_await l.Release();
}

/// 延迟放锁方: 300ms 后释放.
task<void> lock_releaser(uredis::ulock &l)
{
    co_await uco_sleep(300ms);
    auto ok = co_await l.Release();
    LOGMSG("delayed release:", NR(ok));
}

task<void> demo_lock()
{
    uredis::ulock::config cfg;
    cfg.key = "uco:test:lock";
    cfg.ttl_ms = 5000;
    cfg.retry_interval_ms = 50;

    uredis::ulock l1(cfg);
    uredis::ulock l2(cfg);

    // 互斥.
    LOGMSG("l1 acquire:", NR(co_await l1.TryAcquire()));
    LOGMSG("l2 blocked:", NR(co_await l2.TryAcquire()));
    LOGMSG("l1 release:", NR(co_await l1.Release()));
    LOGMSG("l1 double release:", NR(co_await l1.Release()));
    LOGMSG("l2 acquire after release:", NR(co_await l2.TryAcquire()));
    co_await l2.Release();

    // 阻塞获取: l2 持有, releaser 300ms 后放, waiter 阻塞等到.
    // 两个子协程进同一 cobatch, run() 返回 = 均已结束, 锁静止, 可安全关闭.
    LOGMSG("l2 re-acquire:", NR(co_await l2.TryAcquire()));
    cobatch batch(2);
    batch.add(lock_releaser(l2));
    batch.add(lock_waiter(l1));
    co_await batch.run();

    l1.Close();
    l2.Close();
}

// ==================== 看门狗: 持有时长超过 ttl 仍存活 ====================
task<void> demo_watchdog()
{
    uredis::ulock::config cfg;
    cfg.key = "uco:test:wd";
    cfg.ttl_ms = 2000;           // 2s 过期.
    cfg.renew_interval_ms = 500; // 看门狗 500ms 续期.

    uredis::ulock l(cfg);
    LOGMSG("wd acquire:", NR(co_await l.TryAcquire()));
    co_await uco_sleep(3200ms); // 睡 3.2s > ttl 2s.
    LOGMSG("wd still owns after ttl:", NR(l.Owns()));
    LOGMSG("wd manual renew:", NR(co_await l.ReNew()));
    LOGMSG("wd release:", NR(co_await l.Release()));
    LOGMSG("wd not owns after release:", NR(!l.Owns()));
    l.Close();
}

// ==================== 入口 ====================
task<void> demo()
{
    uredis::upool::config cfg;
    cfg.max_size = 4;
    cfg.min_idle = 1;
    cfg.reap_interval_ms = 2'000;
    uredis::upool pool(cfg);

    cobatch batchrunner(5);
    batchrunner.add(demo1(pool));
    batchrunner.add(demo2(pool));
    co_await batchrunner.run();
    pool.Close();

    co_await demo_lock();
    co_await demo_watchdog();
}

int main()
{
    uco::OpenLog("test_uredis", LogLevel::INFO, LogMode::CONSOLE, true);
    go demo();
    return 0;
}
