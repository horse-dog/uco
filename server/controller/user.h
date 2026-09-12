#pragma once

/**
 * @file user.h
 * @brief HTTP 控制器 (controller 层): UserController.
 *
 * 路由注册 (装配层用 MakeHandler 绑定成员协程, ≈ gin 的方法值):
 * @code
 *   httpserver.GET("/login",
 *                  MakeHandler(store, Sessions),
 *                  MakeHandler(userController, LoginPage));
 * @endcode
 *
 * @note controller 只做: 参数绑定/校验, 登录态与 CSRF 检查,
 *       调 service, 错误码到 HTTP 的翻译; 不做业务逻辑 (service);
 * @note 成员协程经 Bind 生成 HandleFunc (存于路由树), controller
 *       对象生命周期须覆盖全部请求 (与 service/dao 同置于
 *       RunHttpServer 协程帧即可).
 */

#include "http/httpserver.h"
#include "server/service/user.h" // service 层接口 (webserver::service::IUserService)

#include <string>

class Session;

namespace webserver
{
namespace controller
{

class UserController final
{
  public:
    UserController(service::IUserService &svc, std::string csrf_session_key);
    ~UserController() = default;
    UserController(const UserController &) = delete;
    UserController &operator=(const UserController &) = delete;

    /**
     * @brief GET /api/me: 返回当前登录用户名.
     *        未登录或会话数据不完整时返回 401.
     */
    uco::task<void> CurrentUser(HttpContext *ctx);

    /**
     * @brief GET /register: 注册页.
     *        已登录则 302 /welcome, 未登录发注册页.
     */
    uco::task<void> RegisterPage(HttpContext *ctx);

    /**
     * @brief POST /register: 注册 (username/password 表单), 成功即登录.
     *        code=0 跳 /welcome; code=1 账号已建但自动登录失败, 跳 /login;
     *        用户名被占 409; 参数非法 400.
     */
    uco::task<void> Register(HttpContext *ctx);

    /**
     * @brief 中间件: session 有 vid (已登录) 则 409 + Abort,
     *        挂于 Sessions() 之后 (仅匿名路由, 如 POST /register).
     */
    uco::task<void> RequireAnonymous(HttpContext *ctx);

    /**
     * @brief GET /login: 登录页.
     *        已登录 (session 有 vid) 则 302 /welcome, 不再发登录页
     *        (≈ gin 模式: 页面访问权收进服务端).
     */
    uco::task<void> LoginPage(HttpContext *ctx);

    /**
     * @brief POST /login: 登录 (username/password 表单).
     *        成功 200 + 建立登录态并轮换 session ID;
     *        凭证错误 401 (统一文案防枚举); 落盘失败 500.
     */
    uco::task<void> Login(HttpContext *ctx);

    /**
     * @brief POST /logout: 登出, 幂等 (DEL Redis + cookie 过期).
     */
    uco::task<void> Logout(HttpContext *ctx);

    /**
     * @brief GET /welcome: 欢迎页 (需登录).
     *        未登录 (session 无 vid) 则 302 /login.
     */
    uco::task<void> WelcomePage(HttpContext *ctx);

  private:
    /**
     * @brief 建立登录态: 写 vid/username, 刷新 CSRF token, 并轮换
     *        session ID (防 session fixation).
     * @param s        当前请求的会话 (匿名).
     * @param vid      用户 ID.
     * @param username 用户名.
     * @return 成功 true; 失败 false 且会话回滚为原样 (应答 500).
     */
    uco::task<bool> EstablishLoginSession(Session *s, uint64_t vid,
                                          const std::string &username);

    service::IUserService &m_svc; ///< 业务接口 (引用: 非空契约).
    std::string m_csrfSessionKey; ///< Session 中保存 CSRF token 的配置键名.
};

} // namespace controller
} // namespace webserver
