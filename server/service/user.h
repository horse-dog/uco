#pragma once

/**
 * @file user.h
 * @brief 用户业务逻辑接口 (service 层): IUserService + UserService.
 *
 * @note service 职责: 密码加密/校验 (bcrypt, 生成与比对在此),
 *       业务规则与错误翻译; 不做存取 (dao), 不做登录态 (session);
 * @note controller 依赖 IUserService 接口 (引用注入, 生命周期归使用方),
 *       单测可换 mock 实现.
 */

#include "core/uco.h"
#include "server/service/user.pb.h"

#include "server/dao/user.h" // dao 层接口 (webserver::dao::IUserDao)

namespace webserver
{
namespace service
{

/**
 * @brief 用户业务接口.
 * @note 返回码统一: 0 成功 / >0 业务失败 / -1 不可重试 / -2 可重试;
 *       系统错误详情由实现记日志, 不进返回值.
 */
class IUserService
{
  public:
    virtual ~IUserService() = default;

    /**
     * @brief 注册: 校验参数, bcrypt 加密, 生成 vid, 插入用户.
     *        规则契约见 service/user.proto RegisterReq 注释 (前后端单一文档源).
     * @return 0 成功 (rsp 已填充);
     *         1 用户名已被占用; 2 参数非法 (长度);
     *         3 密码过于简单 (黑名单/含用户名/危险字符);
     *         -1/-2 系统错误 (不可重试/可重试).
     */
    virtual uco::task<int> Register(const service::RegisterReq &req,
                                    service::RegisterRsp &rsp) = 0;

    /**
     * @brief 登录: 按用户名取凭据并做 bcrypt 校验.
     * @return 0 成功 (rsp 已填充);
     *         1 用户不存在; 2 密码错误;
     *         -1/-2 系统错误 (不可重试/可重试).
     */
    virtual uco::task<int> Login(const service::LoginReq &req,
                                 service::LoginRsp &rsp) = 0;
};

/**
 * @brief 默认实现 (dao 注入 + libxcrypt bcrypt).
 */
class UserService final : public IUserService
{
  public:
    explicit UserService(dao::IUserDao &dao);
    ~UserService() override = default;
    UserService(const UserService &) = delete;
    UserService &operator=(const UserService &) = delete;

    /**
     * @brief 注册: 校验参数, bcrypt 加密, 生成 vid, 插入用户.
     *        规则契约见 service/user.proto RegisterReq 注释 (前后端单一文档源).
     * @return 0 成功 (rsp 已填充);
     *         1 用户名已被占用; 2 参数非法 (长度);
     *         3 密码过于简单 (黑名单/含用户名/危险字符);
     *         -1/-2 系统错误 (不可重试/可重试).
     */
    uco::task<int> Register(const service::RegisterReq &req,
                            service::RegisterRsp &rsp) override;

    /**
     * @brief 登录: 按用户名取凭据并做 bcrypt 校验.
     * @return 0 成功 (rsp 已填充);
     *         1 用户不存在; 2 密码错误;
     *         -1/-2 系统错误 (不可重试/可重试).
     */
    uco::task<int> Login(const service::LoginReq &req,
                         service::LoginRsp &rsp) override;

  private:
    dao::IUserDao &m_dao; ///< dao 接口 (引用: 非空契约, 生命周期归使用方).
};

} // namespace service
} // namespace webserver
