#pragma once

/**
 * @file user_mysql.h
 * @brief user 表 dao 的 MySQL 实现 (MySqlUserDao), 接口见 user.h.
 *
 * @note 防注入: 字符串参数一律 usql::uescape 后拼接, 整数直接拼接;
 * @note 连接经 usql::upool 获取 (UsqlGuard RAII 归还), 绑定走
 *       usql::uselect (三态语义: 未命中与系统错误分离).
 */

#include "user.h"

#include "usql.h"

namespace webserver
{
namespace dao
{

class MySqlUserDao final : public IUserDao
{
  public:
    explicit MySqlUserDao(usql::upool &pool);
    ~MySqlUserDao() override = default;
    MySqlUserDao(const MySqlUserDao &) = delete;
    MySqlUserDao &operator=(const MySqlUserDao &) = delete;

    /**
     * @brief 注册: 插入用户.
     * @return 0 成功; 1 用户名已被占用; -1/-2 系统错误 (不可重试/可重试).
     */
    uco::task<int> InsertUser(const user::InsertUserReq &req,
                              user::InsertUserRsp &rsp) override;
    /**
     * @brief 登录: 按用户名取认证凭据.
     * @note rsp 含 password_hash, 仅限登录/改密链路, 勿复用于业务响应.
     * @return 0 成功 (rsp 已填充); 1 无此用户;
     *         -1/-2 系统错误 (不可重试/可重试).
     */
    uco::task<int> QueryByUsername(const user::QueryByUsernameReq &req,
                                   user::QueryByUsernameRsp &rsp) override;
    /**
     * @brief 鉴权后取用户信息 / 按 vid 查询 (业务用途, rsp 不含密文).
     * @return 0 成功 (rsp 已填充); 1 无此用户;
     *         -1/-2 系统错误 (不可重试/可重试).
     */
    uco::task<int> QueryByVid(const user::QueryByVidReq &req,
                              user::QueryByVidRsp &rsp) override;
    /**
     * @brief 修改密码.
     * @return 0 成功; 1 无此用户; -1/-2 系统错误 (不可重试/可重试).
     */
    uco::task<int> UpdatePassword(const user::UpdatePasswordReq &req,
                                  user::UpdatePasswordRsp &rsp) override;

  private:
    usql::upool &m_pool; ///< MySQL 连接池 (引用: 非空契约, 生命周期归使用方).
};

} // namespace dao
} // namespace webserver
