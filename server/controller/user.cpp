/**
 * @file user.cpp
 * @brief HTTP 控制器实现 (UserController), 见 controller/user.h.
 *
 * 鉴权模式: cookie + session + CSRF token (synchronizer token):
 *   1. GET /api/csrf (框架 SessionIssue) 建立匿名 session 并签发
 *      token; 校验由框架 SessionCheck 中间件在链上完成
 *      (均挂于 Sessions 之后, 见 csrf.h), 表单体只含业务字段;
 *   2. 登录成功写 session (vid/username), 刷新 token (防复用) 并轮换
 *      session ID (防 session fixation); Rotate 内含落盘 + cookie 下发;
 *      注册成功即登录, 与登录共用同一路径 (EstablishLoginSession).
 */

#include "server/controller/user.h"
#include "server/controller/user.pb.h" // Http*Rsp (package webserver.controller)

#include "http/csrf.h"
#include "http/session.h"
#include "core/ulog.h"

#include <string>
#include <unordered_map>
#include <utility>

namespace webserver
{
namespace controller
{

/// session 中的键.
static constexpr const char *kKeyVid = "vid";
static constexpr const char *kKeyUsername = "username";

/// 一次性跳转提示 cookie (未登录被弹回登录页时种下; 仅 /login 路径,
/// 60 秒, 非 HttpOnly 供 JS 读取, 读后即焚 — 手输/外链无法伪造).
static constexpr const char *kNoticeCookie = "login_notice";

/// 一次性跳转提示 cookie (已登录访问登录/注册页被弹回时种下; 仅
/// /welcome 路径, 60 秒, 非 HttpOnly 供 JS 读取, 读后即焚).
static constexpr const char *kWelcomeNoticeCookie = "welcome_notice";

UserController::UserController(service::IUserService &svc,
                               std::string csrf_session_key)
    : m_svc(svc), m_csrfSessionKey(std::move(csrf_session_key))
{
}

// 中间件: 已登录则 409 拒绝 (断链式注册).
uco::task<void> UserController::RequireAnonymous(HttpContext *ctx)
{
    const char* data = "{\"code\":409,\"msg\":\"already logged in\"}";
    if (!Session::FromContext(ctx)->Get(kKeyVid).empty())
    {
        ctx->Data(409, "application/json", data);
        ctx->Abort();
    }
    co_await ctx->Next();
}

uco::task<bool> UserController::EstablishLoginSession(Session *s, uint64_t vid,
                                                      const std::string &username)
{
    // 清旧账号残留 key, 防跨账号泄漏.
    s->Clear();
    // 写登录态 + 刷新 token (旧 token 随轮换掉的旧会话作废, 防复用),
    // 随后 Rotate: 换新 session ID → 新 key 落盘 + 删旧 key
    // (防 session fixation, 纵深防御; 登录态只落新 key).
    s->Set(kKeyVid, std::to_string(vid));
    s->Set(kKeyUsername, username);
    s->Set(m_csrfSessionKey, csrf::Csrf::NewToken());
    co_return co_await s->Rotate();
}

static controller::HttpLoginRsp MakeLoginResp(int32_t code, const std::string &msg,
                                         uint64_t vid = 0,
                                         const std::string &username = "")
{
    controller::HttpLoginRsp resp;
    resp.set_code(code);
    resp.set_msg(msg);
    if (vid != 0 || !username.empty())
    { // 有载荷才下发 body (错误响应省略, 见 proto 信封注释).
        auto *body = resp.mutable_body();
        body->set_vid(vid);
        body->set_username(username);
    }
    return resp;
}

static controller::HttpRegisterRsp MakeRegisterResp(int32_t code,
                                              const std::string &msg,
                                              uint64_t vid = 0,
                                              const std::string &username = "")
{
    controller::HttpRegisterRsp resp;
    resp.set_code(code);
    resp.set_msg(msg);
    if (vid != 0 || !username.empty())
    { // 有载荷才下发 body (错误响应省略, 见 proto 信封注释).
        auto *body = resp.mutable_body();
        body->set_vid(vid);
        body->set_username(username);
    }
    return resp;
}

static controller::HttpLogoutRsp MakeLogoutResp(int32_t code, const std::string &msg)
{
    controller::HttpLogoutRsp resp;
    resp.set_code(code);
    resp.set_msg(msg);
    return resp;
}

uco::task<void> UserController::CurrentUser(HttpContext *ctx)
{
    Session *s = Session::FromContext(ctx);
    const std::string &vid = s->Get(kKeyVid);
    const std::string &username = s->Get(kKeyUsername);

    controller::HttpCurrentUserRsp resp;
    if (vid.empty() || username.empty())
    {
        resp.set_code(401);
        resp.set_msg("unauthorized");
        ctx->Json(401, resp);
        co_return;
    }

    resp.set_code(0);
    resp.set_msg("ok");
    resp.mutable_body()->set_username(username);
    ctx->Json(200, resp);
    co_return;
}

uco::task<void> UserController::RegisterPage(HttpContext *ctx)
{
    Session *s = Session::FromContext(ctx);
    if (!s->Get(kKeyVid).empty())
    { // 已登录: 种一次性提示后跳欢迎页.
        LOGDBG("register page: already logged in, redirect to /welcome");
        ctx->SetSameSite(HttpContext::eSameSiteLax); // 顶级导航携带.
        ctx->SetCookie(kWelcomeNoticeCookie, "already_logged_in", 60,
                       "/welcome");
        ctx->Redirect(302, "/welcome");
        co_return;
    }
    ctx->File("register.html");
    co_return;
}

uco::task<void> UserController::Register(HttpContext *ctx)
{
    // 1. 表单绑定 (CSRF 已由链上 CsrfProtect 中间件校验; 失败由
    //    BindForm 内部 400 + Abort).
    std::unordered_map<std::string, std::string> fields;
    ctx->BindForm(fields);

    auto it_user = fields.find("username");
    auto it_pass = fields.find("password");
    if (it_user == fields.end() || it_pass == fields.end())
    {
        LOGWRN("register: missing field, url:", ctx->GetRequestUrl());
        ctx->Json(400, MakeRegisterResp(400, "missing field"));
        co_return;
    }
    Session *s = Session::FromContext(ctx);

    // 3. 业务 (service).
    service::RegisterReq req;
    req.set_username(it_user->second);
    req.set_password(it_pass->second);
    service::RegisterRsp rsp;
    int ret = co_await m_svc.Register(req, rsp);

    switch (ret)
    {
    case 0:
        // 注册即登录: 写登录态 + 轮换 session ID (与 Login 共用
        // EstablishLoginSession). 旧匿名 CSRF 会话随旧 key 一并删除
        // (token 作废防复用), 登录态只落新 key.
        if (!co_await EstablishLoginSession(s, rsp.vid(), rsp.username()))
        {
            // 自动登录失败 (多为 Redis 故障): 账号已建不可回滚, 降级为
            // "注册成功待登录" — 销毁匿名会话 (Rotate 已回滚内存状态,
            // Destroy 作用于旧会话; cookie 必清, Redis DEL 失败由 TTL
            // 兜底), code=1 让前端跳登录页手动登录.
            LOGERR("register: auto login failed, manual login required,",
                   "vid:", rsp.vid(), "username:", rsp.username());
            s->Destroy();
            ctx->Json(200, MakeRegisterResp(1, "注册成功, 请登录",
                                            rsp.vid(), rsp.username()));
            co_return;
        }
        LOGMSG("register ok vid:", rsp.vid(), "username:", rsp.username());
        ctx->Json(200, MakeRegisterResp(0, "ok", rsp.vid(), rsp.username()));
        break;
    case 1: // 用户名已被占用 (注册场景无枚举顾虑, 明示冲突).
        LOGWRN("register: rejected, username:", it_user->second, "ret:", ret);
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
        LOGERR("register: service error, username:", it_user->second, "ret:", ret);
        ctx->Json(500, MakeRegisterResp(500, "internal error"));
        break;
    default:
        LOGERR("register: unexpected ret:", ret);
        ctx->Json(500, MakeRegisterResp(500, "internal error"));
        break;
    }
    co_return;
}

uco::task<void> UserController::LoginPage(HttpContext *ctx)
{
    Session *s = Session::FromContext(ctx);
    if (!s->Get(kKeyVid).empty())
    { // 已登录: 种一次性提示后跳欢迎页 (页面访问权在服务端).
        LOGDBG("login page: already logged in, redirect to /welcome");
        ctx->SetSameSite(HttpContext::eSameSiteLax); // 顶级导航携带.
        ctx->SetCookie(kWelcomeNoticeCookie, "already_logged_in", 60,
                       "/welcome");
        ctx->Redirect(302, "/welcome");
        co_return;
    }
    ctx->File("login.html");
    co_return;
}

uco::task<void> UserController::Logout(HttpContext *ctx)
{
    // CSRF 已由链上 CsrfProtect 中间件校验 (防强制登出, 无表单字段).
    Session *s = Session::FromContext(ctx);

    // 1. 销毁会话: 标记后由中间件 auto_save 兜底执行
    //    (DEL Redis key + 浏览器 cookie 立即过期).
    //    幂等: 未登录 (无 vid) 销毁空会话亦无害.
    s->Destroy();
    LOGMSG("logout: session destroyed, session:", s->ID());
    ctx->Json(200, MakeLogoutResp(0, "ok"));
    co_return;
}

uco::task<void> UserController::WelcomePage(HttpContext *ctx)
{
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
    // 1. 表单绑定 (CSRF 已由链上 CsrfProtect 中间件校验; 失败由
    //    BindForm 内部 400 + Abort).
    std::unordered_map<std::string, std::string> fields;
    ctx->BindForm(fields);

    auto it_user = fields.find("username");
    auto it_pass = fields.find("password");
    if (it_user == fields.end() || it_pass == fields.end())
    {
        LOGWRN("login: missing field, url:", ctx->GetRequestUrl());
        ctx->Json(400, MakeLoginResp(400, "missing field"));
        co_return;
    }
    Session *s = Session::FromContext(ctx);

    // 3. 业务 (service).
    service::LoginReq req;
    req.set_username(it_user->second);
    req.set_password(it_pass->second);
    service::LoginRsp rsp;
    int ret = co_await m_svc.Login(req, rsp);

    switch (ret)
    {
    case 0:
        if (!co_await EstablishLoginSession(s, rsp.vid(), rsp.username()))
        {
            LOGERR("login: establish session failed, vid:", rsp.vid(),
                   "username:", rsp.username());
            ctx->Json(500, MakeLoginResp(500, "internal error"));
            co_return;
        }
        LOGMSG("login: session established (rotated), vid:", rsp.vid(),
               "username:", rsp.username());
        ctx->Json(200, MakeLoginResp(0, "ok", rsp.vid(), rsp.username()));
        break;
    case 1: // 用户不存在 (service 返回码).
    case 2: // 密码错误 (service 返回码).
        LOGWRN("login: rejected, username:", it_user->second, "ret:", ret);
        ctx->Json(401, MakeLoginResp(401, "wrong username or password"));
        break;
    default:
        LOGERR("login: service error, username:", it_user->second, "ret:", ret);
        ctx->Json(500, MakeLoginResp(500, "internal error"));
        break;
    }
    co_return;
}

} // namespace controller
} // namespace webserver
