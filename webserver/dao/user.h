#pragma once

/**
 * @file user.h
 * @brief user 表数据访问接口 (dao 层).
 *
 * 实现: user_mysql.h (生产, MySQL), user_mock.h (单测).
 *
 * 表结构 (见 webserver 建表约定):
 *   vid      BIGINT UNSIGNED  主键, 业务算法生成, 非自增;
 *   username VARCHAR(64)      唯一键;
 *   password VARCHAR(255)     密文 (bcrypt 等, 生成与校验在 service 层).
 *
 * 模型约定:
 *   - dao 出入参一律 proto 化 (user.proto, package webserver.user):
 *     每个接口一对 XxxReq / XxxRsp, 错误码仍在返回值
 *     (0 成功 / 1 业务未命中 / -1 不可重试 / -2 可重试);
 *   - Rsp 按用途重新定义、平铺字段, 不复用表镜像 User:
 *     业务查询不含密文 (密文不出 dao); 认证凭据
 *     (QueryByUsernameRsp) 仅限登录/改密链路做 bcrypt 校验;
 *   - 对外响应一律构造 UserInfo (不含 password), 密文回显的防线
 *     在响应建模.
 *
 * @note dao 只做存取: 不做密码加密/校验 (service 职责),
 *       不做登录态判断 (session 职责);
 * @note service 依赖本接口 (引用注入, 生命周期归使用方).
 */

#include "uco.h"
#include "user.pb.h" // user 领域模型与出入参 (package webserver.user)

namespace webserver
{
namespace dao
{

/**
 * @brief user 表访问接口.
 */
class IUserDao
{
  public:
    virtual ~IUserDao() = default;

    /**
     * @brief 注册: 插入用户.
     * @return 0 成功; 1 用户名已被占用; -1/-2 系统错误 (不可重试/可重试).
     */
    virtual uco::task<int> InsertUser(const user::InsertUserReq &req,
                                      user::InsertUserRsp &rsp) = 0;

    /**
     * @brief 登录: 按用户名取认证凭据.
     * @note rsp 含 password_hash, 仅限登录/改密链路, 勿复用于业务响应.
     * @return 0 成功 (rsp 已填充); 1 无此用户;
     *         -1/-2 系统错误 (不可重试/可重试).
     */
    virtual uco::task<int> QueryByUsername(const user::QueryByUsernameReq &req,
                                           user::QueryByUsernameRsp &rsp) = 0;

    /**
     * @brief 鉴权后取用户信息 / 按 vid 查询 (业务用途, rsp 不含密文).
     * @return 0 成功 (rsp 已填充); 1 无此用户;
     *         -1/-2 系统错误 (不可重试/可重试).
     */
    virtual uco::task<int> QueryByVid(const user::QueryByVidReq &req,
                                      user::QueryByVidRsp &rsp) = 0;

    /**
     * @brief 修改密码.
     * @return 0 成功; 1 无此用户; -1/-2 系统错误 (不可重试/可重试).
     */
    virtual uco::task<int> UpdatePassword(const user::UpdatePasswordReq &req,
                                          user::UpdatePasswordRsp &rsp) = 0;
};

} // namespace dao
} // namespace webserver
