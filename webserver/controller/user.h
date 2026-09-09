#pragma once

/**
 * @file user.h
 * @brief HTTP 控制器 (controller 层): UserController.
 *
 * 路由注册 (装配层用 MakeHandler 绑定成员协程, ≈ gin 的方法值):
 * @code
 *   httpserver.GET("/login",
 *                  MakeSessionMiddleware(&store, "uco_session"),
 *                  MAKE_HANDLER(userController, LoginPage));
 * @endcode
 *
 * @note controller 只做: 参数绑定/校验, 登录态与 CSRF 检查,
 *       调 service, 错误码到 HTTP 的翻译; 不做业务逻辑 (service);
 * @note 成员协程经 Bind 生成 HandleFunc (存于路由树), controller
 *       对象生命周期须覆盖全部请求 (与 service/dao 同置于
 *       RunHttpServer 协程帧即可).
 */

#include "httpserver.h"

#include "service/user.h" // service 层接口 (webserver::service::IUserService)

namespace webserver
{
namespace controller
{

class UserController final
{
  public:
    explicit UserController(service::IUserService &svc);
    ~UserController() = default;
    UserController(const UserController &) = delete;
    UserController &operator=(const UserController &) = delete;

    /**
     * @brief GET /api/csrf: 签发 CSRF token (存 session, 随 cookie 下发).
     *        已有 token 则复用; 无 session 时建立匿名 session.
     */
    uco::task<void> Csrf(HttpContext *ctx);

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
     * @brief POST /register: 表单注册 (username/password/csrf_token).
     *        CSRF 校验失败 403; 参数非法 400; 用户名被占 409;
     *        成功 200 + 销毁匿名 session，要求用户重新登录.
     */
    uco::task<void> Register(HttpContext *ctx);

    /**
     * @brief GET /login: 登录页.
     *        已登录 (session 有 vid) 则 302 /welcome, 不再发登录页
     *        (≈ gin 模式: 页面访问权收进服务端).
     */
    uco::task<void> LoginPage(HttpContext *ctx);

    /**
     * @brief POST /login: 表单登录 (username/password/csrf_token).
     *        CSRF 校验失败 403; 凭证错误 401 (统一文案防枚举);
     *        成功 200 + 写 session (vid/username) 并刷新 token.
     */
    uco::task<void> Login(HttpContext *ctx);

    /**
     * @brief POST /logout: 登出 (csrf_token 校验, 销毁会话).
     *        幂等: 未登录也成功; 销毁 = DEL Redis + cookie 立即过期.
     */
    uco::task<void> Logout(HttpContext *ctx);

    /**
     * @brief GET /welcome: 欢迎页 (需登录).
     *        未登录 (session 无 vid) 则 302 /login.
     */
    uco::task<void> WelcomePage(HttpContext *ctx);

  private:
    static std::string NewCsrfToken();

    service::IUserService &m_svc; ///< 业务接口 (引用: 非空契约).
};

} // namespace controller
} // namespace webserver
