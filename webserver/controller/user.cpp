/**
 * @file user.cpp
 * @brief HTTP 控制器实现 (UserController), 见 controller/user.h.
 *
 * 鉴权模式: cookie + session + CSRF token (synchronizer token):
 *   1. GET /api/csrf 建立匿名 session, 签发 token (存 session);
 *   2. POST /login 携带 username/password/csrf_token, 与 session 中
 *      的 token 比对, 通过后才进入业务;
 *   3. 登录成功写 session (vid/username) 并刷新 token (防复用),
 *      auto_save 兜底落盘 + cookie 下发.
 */

#include "controller/user.h"

#include "session.h"
#include "ulog.h"

#include <openssl/rand.h>

#include <string>
#include <unordered_map>

namespace webserver
{
namespace controller
{

/// session 中的键.
static constexpr const char *kKeyCsrf = "csrf";
static constexpr const char *kKeyVid = "vid";
static constexpr const char *kKeyUsername = "username";

/// 一次性跳转提示 cookie (未登录被弹回登录页时种下; 仅 /login 路径,
/// 60 秒, 非 HttpOnly 供 JS 读取, 读后即焚 — 手输/外链无法伪造).
static constexpr const char *kNoticeCookie = "login_notice";

UserController::UserController(service::IUserService &svc) : m_svc(svc)
{
}

std::string UserController::NewCsrfToken()
{
    unsigned char rnd[32];
    if (RAND_bytes(rnd, sizeof(rnd)) != 1)
    {
        LOGFTL("controller: rand_bytes failed for csrf token");
        return "";
    }
    static const char hex[] = "0123456789abcdef";
    std::string token;
    token.reserve(sizeof(rnd) * 2);
    for (unsigned char b : rnd)
    {
        token += hex[b >> 4];
        token += hex[b & 0xf];
    }
    return token;
}

static user::HttpLoginRsp MakeLoginResp(int32_t code, const std::string &msg,
                                         uint64_t vid = 0,
                                         const std::string &username = "")
{
    user::HttpLoginRsp resp;
    resp.set_code(code);
    resp.set_msg(msg);
    resp.set_vid(vid);
    resp.set_username(username);
    return resp;
}

static user::HttpRegisterRsp MakeRegisterResp(int32_t code,
                                              const std::string &msg,
                                              uint64_t vid = 0,
                                              const std::string &username = "")
{
    user::HttpRegisterRsp resp;
    resp.set_code(code);
    resp.set_msg(msg);
    resp.set_vid(vid);
    resp.set_username(username);
    return resp;
}

static user::HttpLogoutRsp MakeLogoutResp(int32_t code, const std::string &msg)
{
    user::HttpLogoutRsp resp;
    resp.set_code(code);
    resp.set_msg(msg);
    return resp;
}

/**
 * @brief CSRF 校验样板 (与 session 内 token 比对):
 *        无 token/不匹配同为 403 (不区分原因, 防探测) + 统一 JSON.
 * @return 校验通过 true; 已写 403 响应 false.
 */
static bool CheckCsrf(Session *s, const std::string &submitted,
                      HttpContext *ctx, const std::string &who)
{
    if (s->Get(kKeyCsrf).empty() || s->Get(kKeyCsrf) != submitted)
    {
        LOGWRN(who, ": csrf check failed, reason:",
               (s->Get(kKeyCsrf).empty() ? "no token" : "token mismatch"));
        ctx->Json(403, MakeLoginResp(403, "csrf check failed"));
        return false;
    }
    return true;
}

uco::task<void> UserController::Csrf(HttpContext *ctx)
{
    // FromContext 保证非空: 中间件未挂时它已抛 HttpException(500).
    Session *s = Session::FromContext(ctx);

    std::string token = s->Get(kKeyCsrf);
    if (token.empty())
    { // 首次: 签发并存 session (中间件 auto_save 兜底落盘+下发 cookie).
        token = NewCsrfToken();
        s->Set(kKeyCsrf, token);
        LOGDBG("csrf: token issued, session:", s->ID());
    }

    user::HttpCsrfRsp resp;
    resp.set_csrf_token(token);
    ctx->Json(200, resp);
    co_return;
}

uco::task<void> UserController::CurrentUser(HttpContext *ctx)
{
    Session *s = Session::FromContext(ctx);
    const std::string &vid = s->Get(kKeyVid);
    const std::string &username = s->Get(kKeyUsername);

    user::HttpCurrentUserRsp resp;
    if (vid.empty() || username.empty())
    {
        ctx->Json(401, resp);
        co_return;
    }

    resp.set_username(username);
    ctx->Json(200, resp);
    co_return;
}

uco::task<void> UserController::RegisterPage(HttpContext *ctx)
{
    // FromContext 保证非空: 中间件未挂时它已抛 HttpException(500).
    Session *s = Session::FromContext(ctx);
    if (!s->Get(kKeyVid).empty())
    { // 已登录: 不再给注册页, 直接跳欢迎页.
        LOGDBG("register page: already logged in, redirect to /welcome");
        ctx->Redirect(302, "/welcome");
        co_return;
    }
    ctx->File("register.html");
    co_return;
}

uco::task<void> UserController::Register(HttpContext *ctx)
{
    // 1. 表单绑定 (失败由 BindForm 内部 400 + Abort).
    std::unordered_map<std::string, std::string> fields;
    ctx->BindForm(fields);

    auto it_user = fields.find("username");
    auto it_pass = fields.find("password");
    auto it_csrf = fields.find("csrf_token");
    if (it_user == fields.end() || it_pass == fields.end() ||
        it_csrf == fields.end())
    {
        LOGWRN("register: missing field, url:", ctx->GetRequestUrl());
        ctx->Json(400, MakeRegisterResp(400, "missing field"));
        co_return;
    }

    // 2. CSRF 校验.
    Session *s = Session::FromContext(ctx);
    if (!CheckCsrf(s, it_csrf->second, ctx, "register"))
    {
        co_return;
    }

    // 3. 业务 (service).
    user::RegisterReq req;
    req.set_username(it_user->second);
    req.set_password(it_pass->second);
    user::RegisterRsp rsp;
    int ret = co_await m_svc.Register(req, rsp);

    switch (ret)
    {
    case 0:
        // 注册成功后要求重新登录：销毁注册阶段的匿名 CSRF session，
        // 防止它继续占用 Redis，也确保登录页重新签发 token/session.
        s->Destroy();
        LOGMSG("register: user created, login required, vid:", rsp.vid(),
               ", username:", rsp.username());
        ctx->Json(200, MakeRegisterResp(0, "ok", rsp.vid(), rsp.username()));
        break;
    case 1: // 用户名已被占用 (注册场景无枚举顾虑, 明示冲突).
        LOGWRN("register: rejected, username:", it_user->second,
               ", svc ret:", ret);
        ctx->Json(409, MakeRegisterResp(409, "用户名已被占用"));
        break;
    case 2: // 参数非法 (长度; service 已记日志).
        LOGWRN("register: invalid params, username:", it_user->second);
        ctx->Json(400, MakeRegisterResp(400, "用户名或密码格式不符合要求"));
        break;
    case 3: // 密码过于简单 (黑名单/含用户名; 注册场景明示, 无枚举顾虑).
        LOGWRN("register: password too weak, username:", it_user->second);
        ctx->Json(400, MakeRegisterResp(400, "密码过于简单, 请更换"));
        break;
    case -1: // 系统错误: 不可重试 (service 已记详细日志).
    case -2: // 系统错误: 可重试.
        LOGERR("register: service error, username:", it_user->second,
               ", svc ret:", ret);
        ctx->Json(500, MakeRegisterResp(500, "internal error"));
        break;
    default:
        LOGERR("register: unexpected svc ret:", ret);
        ctx->Json(500, MakeRegisterResp(500, "internal error"));
        break;
    }
    co_return;
}

uco::task<void> UserController::LoginPage(HttpContext *ctx)
{
    // FromContext 保证非空: 中间件未挂时它已抛 HttpException(500).
    Session *s = Session::FromContext(ctx);
    if (!s->Get(kKeyVid).empty())
    { // 已登录: 不再给登录页, 直接跳欢迎页 (页面访问权在服务端).
        LOGDBG("login page: already logged in, redirect to /welcome");
        ctx->Redirect(302, "/welcome");
        co_return;
    }
    ctx->File("login.html");
    co_return;
}

uco::task<void> UserController::Logout(HttpContext *ctx)
{
    // 1. 表单绑定 (失败由 BindForm 内部 400 + Abort).
    std::unordered_map<std::string, std::string> fields;
    ctx->BindForm(fields);

    auto it_csrf = fields.find("csrf_token");
    if (it_csrf == fields.end())
    {
        LOGWRN("logout: missing csrf_token");
        ctx->Json(400, MakeLogoutResp(400, "missing field"));
        co_return;
    }

    // 2. CSRF 校验 (防 CSRF 强制登出).
    Session *s = Session::FromContext(ctx);
    if (!CheckCsrf(s, it_csrf->second, ctx, "logout"))
    {
        co_return;
    }

    // 3. 销毁会话: 标记后由中间件 auto_save 兜底执行
    //    (DEL Redis key + 浏览器 cookie 立即过期).
    //    幂等: 未登录 (无 vid) 销毁空会话亦无害.
    s->Destroy();
    LOGMSG("logout: session destroyed, session:", s->ID());
    ctx->Json(200, MakeLogoutResp(0, "ok"));
    co_return;
}

uco::task<void> UserController::WelcomePage(HttpContext *ctx)
{
    // FromContext 保证非空: 中间件未挂时它已抛 HttpException(500).
    Session *s = Session::FromContext(ctx);
    if (s->Get(kKeyVid).empty())
    { // 未登录: 种一次性提示标记后弹回登录页 (URL 保持干净,
        // 直接访问 /login 无 cookie 即无提示).
        LOGDBG("welcome page: not logged in, redirect to /login");
        ctx->SetSameSite(HttpContext::eSameSiteLax); // 顶级导航携带.
        ctx->SetCookie(kNoticeCookie, "not_logged_in", 60, "/login");
        ctx->Redirect(302, "/login");
        co_return;
    }
    ctx->File("welcome.html");
    co_return;
}

uco::task<void> UserController::Login(HttpContext *ctx)
{
    // 1. 表单绑定 (失败由 BindForm 内部 400 + Abort).
    std::unordered_map<std::string, std::string> fields;
    ctx->BindForm(fields);

    auto it_user = fields.find("username");
    auto it_pass = fields.find("password");
    auto it_csrf = fields.find("csrf_token");
    if (it_user == fields.end() || it_pass == fields.end() ||
        it_csrf == fields.end())
    {
        LOGWRN("login: missing field, url:", ctx->GetRequestUrl());
        ctx->Json(400, MakeLoginResp(400, "missing field"));
        co_return;
    }

    // 2. CSRF 校验.
    Session *s = Session::FromContext(ctx);
    if (!CheckCsrf(s, it_csrf->second, ctx, "login"))
    {
        co_return;
    }

    // 3. 业务 (service).
    user::LoginReq req;
    req.set_username(it_user->second);
    req.set_password(it_pass->second);
    user::LoginRsp rsp;
    int ret = co_await m_svc.Login(req, rsp);

    switch (ret)
    {
    case 0:
        // 写登录态 + 刷新 token (登录成功后旧 token 作废, 防复用).
        s->Set(kKeyVid, std::to_string(rsp.vid()));
        s->Set(kKeyUsername, rsp.username());
        s->Set(kKeyCsrf, NewCsrfToken());
        LOGMSG("login: session updated, vid:", rsp.vid(),
               ", username:", rsp.username());
        ctx->Json(200, MakeLoginResp(0, "ok", rsp.vid(), rsp.username()));
        break;
    case 1: // 用户不存在 (service 返回码).
    case 2: // 密码错误 (service 返回码).
        LOGWRN("login: rejected, username:", it_user->second,
               ", svc ret:", ret);
        ctx->Json(401, MakeLoginResp(401, "wrong username or password"));
        break;
    default:
        LOGERR("login: service error, username:", it_user->second,
               ", svc ret:", ret);
        ctx->Json(500, MakeLoginResp(500, "internal error"));
        break;
    }
    co_return;
}

} // namespace controller
} // namespace webserver
