#include "ulog.h"
#include "usql.h"
#include "user.pb.h"
#include <vector>

using namespace uco;

task<void> demo1(usql::upool& pool)
{
    MYSQL *m = co_await pool.acquire();
    if (m == nullptr)
    {
        LOGERR("acquire failed");
        co_return;
    }

    TBUserList users;

    auto ret = co_await usql::uselect(m, "SELECT * FROM user", users.mutable_userlist());
    if (!ret)
    {
        LOGERR("uselect error");
        co_return; // 傻逼AI，co_return 不释放.
    }
    LOGMSG("result:", users);
    pool.release(m);
}

task<void> demo2(usql::upool& pool)
{
    MYSQL *m = co_await pool.acquire();
    if (m == nullptr)
    {
        LOGERR("acquire failed");
        co_return;
    }

    TBUser user;

    auto ret = co_await usql::uselect(m, "SELECT * FROM `user` WHERE username = 'root'", &user);
    if (!ret)
    {
        LOGERR("uselect error");
        co_return;
    }
    LOGMSG("result:", user);
    pool.release(m);
}

task<void> demo3(usql::upool& pool)
{
    MYSQL *m = co_await pool.acquire();
    if (m == nullptr)
    {
        LOGERR("acquire failed");
        co_return;
    }

    std::vector<std::string> options = {
        "INSERT INTO `user` (vid, username, password) VALUES (2, 'example', 'root')",
        "UPDATE `user` SET password = '123456' WHERE password = 'root'",
        "DELETE FROM `user` WHERE vid = 2"
    };

    auto txn = co_await usql::utransaction(m, options);
    LOGMSG("txn committed =", NR(txn.committed));
    pool.release(m);
}

// 连接池正常用法.
task<void> demo()
{
    usql::upool::config cfg;
    cfg.host = "127.0.0.1";
    cfg.user = "root";
    cfg.pass = "123456";
    cfg.db = "webserver";
    cfg.max_size = 4;
    cfg.reap_interval_ms = 2'000;
    cfg.min_idle = 1;
    auto &pool = usql::upool::GetInstance(cfg);

    cobatch batchrunner(5);
    batchrunner.add(demo1(pool));
    batchrunner.add(demo2(pool));
    batchrunner.add(demo3(pool));
    co_await batchrunner.run();
    pool.close();
}

int main()
{
    uco::OpenLog("test_usql", LogLevel::DEBUG, LogMode::CONSOLE, true);
    go demo();
    return 0;
}
