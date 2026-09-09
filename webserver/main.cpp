#include "controller/user.h"
#include "dao/user_mysql.h"
#include "service/user.h"
#include "httpserver.h"
#include "session.h"
#include "uco.h"
#include "udaemon.h"
#include "ulog.h"
#include "demo.pb.h"
#include <cstdio>
#include <sys/eventfd.h>
#include <unistd.h>
#include "usql.h"
#include "uredis.h"

HttpServer httpserver;

using namespace uco;

task<void> echo(HttpContext *context)
{
    auto __msg = context->GetRawData();
    __msg += "\r\n:) responce from Tiny Echo Server!";
    context->String(200, __msg);
    co_return;
}

task<void> middleware1(HttpContext *context)
{
    LOGDBG("before");
    co_await context->Next();
    LOGDBG("after");
    co_return;
}

task<void> middleware2(HttpContext *context)
{
    LOGDBG("before");
    co_await context->Next();
    LOGDBG("after");
    co_return;
}

task<void> middleware3(HttpContext *context)
{
    LOGDBG("before");
    LOGDBG("after");
    co_return;
}

// task<void> hello(HttpContext *context)
// {
//     User user;
//     user.set_username("joker");
//     user.set_age(26);
//     context->Json(200, user);
//     co_return;
// }

task<void> status_page(HttpContext *context)
{
    context->Status(200);
    co_return;
}

// mustache is quiet slow.
task<void> template_page(HttpContext *context)
{
    JokeTemplate user;
    user.set_name("小丑🤡");
    user.set_age(30);
    Address *address1 = user.add_addresses();
    address1->set_street("洪山区珞喻路129号");
    address1->set_city("武汉");
    Address *address2 = user.add_addresses();
    address2->set_street("海淀区双清路30号");
    address2->set_city("北京");
    context->HTML(200, "templates/joke.html", user);
    co_return;
}

task<void> redirect(HttpContext *context)
{
    context->Redirect(302, "/hello");
    co_return;
}

task<void> head(HttpContext *context)
{
    context->File("/video/xxx.mp4");
    co_return;
}

// task<void> post(HttpContext *context)
// {
//     User user;
//     context->BindJSON(user);
//     LOGMSG(NR(user.username()));
//     LOGMSG(NR(user));
//     context->Json(200, user);
//     co_return;
// }

auto MakeMySqLPool()
{
    usql::upool::config config;
    config.host = "127.0.0.1";
    config.user = "root";
    config.pass = "123456";
    config.db = "webserver";
    config.max_size = 16;
    return usql::upool(config);
}

auto MakeRedisPool()
{
    uredis::upool::config config;
    config.max_size = 16;
    return uredis::upool(config);
}

task<void> RunHttpServer()
{
    auto mysql_pool = MakeMySqLPool();
    auto redis_pool = MakeRedisPool();
    webserver::dao::MySqlUserDao userDAO(mysql_pool);
    webserver::service::UserService userService(userDAO);
    webserver::controller::UserController userController(userService);

    SessionStore::Config session_cfg;
    session_cfg.secret_key = "uco-session-secret";
    SessionStore store(session_cfg, redis_pool);
    const std::string kSessionName = "uco_session";

    // ---- 鉴权路由 (cookie + session + csrf, 中间件挂链首) ----
    httpserver.GET("/api/csrf", Sessions(&store, kSessionName),
                   MakeHandler(userController, Csrf));
    httpserver.GET("/api/me", Sessions(&store, kSessionName),
                   MakeHandler(userController, CurrentUser));
    httpserver.GET("/register", Sessions(&store, kSessionName),
                   MakeHandler(userController, RegisterPage));
    httpserver.POST("/register", Sessions(&store, kSessionName),
                    MakeHandler(userController, Register));
    httpserver.GET("/login", Sessions(&store, kSessionName),
                   MakeHandler(userController, LoginPage));
    httpserver.POST("/login", Sessions(&store, kSessionName),
                    MakeHandler(userController, Login));
    httpserver.POST("/logout", Sessions(&store, kSessionName),
                    MakeHandler(userController, Logout));
    httpserver.GET("/welcome", Sessions(&store, kSessionName),
                   MakeHandler(userController, WelcomePage));

    int ret = co_await httpserver.Run();
    if (ret != 0)
    {
        LOGFTL("Server run error: %d", ret);
    }
}

int main(int argc, const char *argv[])
{
    uco::OpenLog("ucohttpsvr", LogLevel::INFO, LogMode::CONSOLE, true);
    uco::InitProcess(false, "ucohttpsvr");
    httpserver.Static("/css/*");
    httpserver.Static("/fonts/*filename");
    httpserver.Static("/images/*filename");
    httpserver.Static("/js/*filename");
    httpserver.Static("/video/*filename");
    httpserver.Static("/music/*filename");

    httpserver.Static("/index.html");
    httpserver.Static("/video.html");
    httpserver.Static("/picture.html");

    httpserver.ErrorPage(400, "400.html");
    httpserver.ErrorPage(403, "403.html");
    httpserver.ErrorPage(404, "404.html");
    httpserver.ErrorPage(500, "500.html");
    httpserver.ErrorTemplate("templates/error.html");

    httpserver.Forward(eGet, "/", "/index.html");
    httpserver.Forward(eGet, "/index", "/index.html");
    httpserver.Forward(eGet, "/video", "/video.html");
    httpserver.Forward(eGet, "/picture", "/picture.html");
    // 直链 .html 一律经动态路由 (登录态检查), 防绕过.
    httpserver.Forward(eGet, "/login.html", "/login");
    httpserver.Forward(eGet, "/register.html", "/register");
    httpserver.Forward(eGet, "/welcome.html", "/welcome");

    // httpserver.GET("/hello", middleware1, middleware2, middleware3, hello);
    httpserver.GET("/redirect", redirect);
    httpserver.GET("/status", status_page);
    httpserver.GET("/template", template_page);
    httpserver.HEAD("/head", head);
    // httpserver.POST("/post", post);
    httpserver.Init(8080, 4, 100, 30);
    go RunHttpServer();
    return 0;
}
