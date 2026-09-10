#include "core/ulog.h"
#include "core/usql.h"
#include "server/dao/user.pb.h"
#include <chrono>
#include <vector>

using namespace uco;

task<void> demo1(usql::upool& pool)
{
    MYSQL *m = co_await pool.Acquire();
    usql::UsqlGuard guard(pool, m);
    if (m == nullptr)
    {
        LOGERR("acquire failed");
        co_return;
    }

    webserver::dao::UserList users;

    auto ret = co_await usql::uselect(m, "SELECT * FROM user", users.mutable_userlist());
    if (ret.ret_code != 0)
    {
        LOGERR("uselect error:", ret.err_msg);
        co_return;
    }
    LOGMSG("rows:", ret.num_rows, ", result:", users);
}

task<void> demo2(usql::upool& pool)
{
    MYSQL *m = co_await pool.Acquire();
    usql::UsqlGuard guard(pool, m);
    if (m == nullptr)
    {
        LOGERR("acquire failed");
        co_return;
    }

    webserver::dao::User user;

    auto ret = co_await usql::uselect(m, "SELECT * FROM `user` WHERE username = 'root'", &user);
    if (ret.ret_code != 0)
    {
        LOGERR("uselect error:", ret.err_msg);
        co_return;
    }
    if (ret.num_rows == 0)
    {
        LOGERR("no data");
        co_return;
    }
    LOGMSG("result:", user);
}

task<void> demo3(usql::upool& pool)
{
    MYSQL *m = co_await pool.Acquire();
    usql::UsqlGuard guard(pool, m);
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
    auto&& pool = usql::upool(cfg);

    cobatch batchrunner(5);
    batchrunner.add(demo1(pool));
    batchrunner.add(demo2(pool));
    batchrunner.add(demo3(pool));
    co_await batchrunner.run();
    co_await uco_sleep(std::chrono::seconds(5)); // 等待 reaper 缩容, 观察日志.
    pool.Close();
}

int main()
{
    uco::OpenLog("test_usql", LogLevel::DEBUG, LogMode::CONSOLE, true);
    go demo();
    return 0;
}
