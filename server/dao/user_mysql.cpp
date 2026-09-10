/**
 * @file user_mysql.cpp
 * @brief user 表 MySQL 数据访问实现 (MySqlUserDao), 见 dao/user_mysql.h.
 */

#include "server/dao/user_mysql.h"

#include "core/ulog.h"
#include "core/usql.h" // upool / UsqlGuard / uselect / UsqlError.

#include <utility>

namespace webserver
{
namespace dao
{

MySqlUserDao::MySqlUserDao(usql::upool &pool) : m_pool(pool)
{
}

// ---- 内部样板封装 ----

/**
 * @brief dao 样板: 借 MySQL 连接 + 失败返回 + RAII 归还.
 *        展开: 连接变量 m / 判空 co_return -2 (建连失败, 可重试, 已记日志)
 *        / guard_m 随当前作用域归还连接.
 * @note 仅限返回 int (0/1/-1/-2 约定) 的 dao 函数使用;
 *       只能封装为宏: guard 必须存活于调用方协程帧 (子函数内声明会在
 *       其首个 co_return 时提前归还), co_return 亦无法从子函数冒泡.
 */
#define USER_DAO_CONN(m)                                                   \
    MYSQL *m = co_await m_pool.Acquire();                                  \
    if (m == nullptr)                                                      \
    {                                                                      \
        LOGERR("user dao: mysql acquire failed");                          \
        co_return -2; /* 建连失败, 可重试. */                               \
    }                                                                      \
    usql::UsqlGuard guard_##m(m_pool, m)

uco::task<int> MySqlUserDao::InsertUser(const dao::InsertUserReq &req,
                                        dao::InsertUserRsp &rsp)
{
    (void)rsp; // 暂无数据返回 (预留: insert_id 等).

    USER_DAO_CONN(m); // RAII 归还连接.

    // 字符串一律 uescape (防注入); vid 为整数, 直接拼接无风险.
    std::string sql;
    sql.reserve(req.username().size() + req.password_hash().size() + 96);
    sql += "INSERT INTO `user` (vid, username, password) VALUES (";
    sql += std::to_string(req.vid());
    sql += ", '";
    sql += usql::uescape(m, req.username());
    sql += "', '";
    sql += usql::uescape(m, req.password_hash());
    sql += "')";

    usql::InsertResult r = co_await usql::uinsert(m, std::move(sql));
    if (r.ret_code == 0)
    {
        co_return 0;
    }
    if (r.ret_code == usql::kDuplicateEntry)
    {
        // 用户名唯一键冲突 (并发注册同名用户的最终防线).
        co_return 1;
    }
    LOGERR("user dao: insert failed:", r.ret_code, r.err_msg); // 详情进日志.
    co_return r.Retryable() ? -2 : -1;
}

uco::task<int> MySqlUserDao::UpdatePassword(const dao::UpdatePasswordReq &req,
                                            dao::UpdatePasswordRsp &rsp)
{
    (void)rsp; // 暂无数据返回.

    USER_DAO_CONN(m);

    std::string sql;
    sql.reserve(req.password_hash().size() + 80);
    sql += "UPDATE `user` SET password = '";
    sql += usql::uescape(m, req.password_hash());
    sql += "' WHERE vid = ";
    sql += std::to_string(req.vid());

    usql::AffectedResult r = co_await usql::uupdate(m, std::move(sql));
    if (r.ret_code != 0)
    {
        LOGERR("user dao: update failed:", r.ret_code, r.err_msg); // 详情进日志.
        co_return r.Retryable() ? -2 : -1;
    }
    if (r.affected_rows == 0)
    {
        // bcrypt 带随机盐, 新旧密文必不同, affected=0 即无此用户.
        co_return 1;
    }
    co_return 0;
}

uco::task<int> MySqlUserDao::QueryByUsername(
    const dao::QueryByUsernameReq &req, dao::QueryByUsernameRsp &rsp)
{
    USER_DAO_CONN(m);

    // 认证凭据查询: 列名须与 Rsp 字段同名同型 (别名 password_hash),
    // 由 uselect 内部 check_alignment 严格校验.
    std::string sql;
    sql.reserve(req.username().size() + 96);
    sql += "SELECT vid, username, password AS password_hash FROM `user`"
           " WHERE username = '";
    sql += usql::uescape(m, req.username());
    sql += "'";

    usql::SelectResult r = co_await usql::uselect(m, std::move(sql), &rsp);
    if (r.ret_code != 0)
    {
        LOGERR("user dao: select failed:", r.ret_code, r.err_msg); // 详情进日志.
        co_return r.Retryable() ? -2 : -1;
    }
    if (r.num_rows == 0)
    {
        co_return 1; // 用户不存在/未注册.
    }
    // 多余行被忽略; 唯一键保证至多一行, 无需额外检测.
    co_return 0;
}

uco::task<int> MySqlUserDao::QueryByVid(const dao::QueryByVidReq &req,
                                        dao::QueryByVidRsp &rsp)
{
    USER_DAO_CONN(m);

    // 业务查询: 不 SELECT 密文, 密文不出 dao.
    std::string sql;
    sql.reserve(80);
    sql += "SELECT vid, username FROM `user` WHERE vid = ";
    sql += std::to_string(req.vid());

    usql::SelectResult r = co_await usql::uselect(m, std::move(sql), &rsp);
    if (r.ret_code != 0)
    {
        LOGERR("user dao: select failed:", r.ret_code, r.err_msg); // 详情进日志.
        co_return r.Retryable() ? -2 : -1;
    }
    if (r.num_rows == 0)
    {
        co_return 1; // 会话里的 vid 已无对应用户 (被删等).
    }
    co_return 0;
}

} // namespace dao
} // namespace webserver
