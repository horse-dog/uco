#include "core/thread_pool.h"
#include "core/usync.h"
#include "server/controller/user.h"
#include "dao/user_mysql.h"
#include "service/user.h"
#include "security/password_hasher.h"
#include "http/csrf.h"
#include "http/httpserver.h"
#include "http/ratelimit.h"
#include "http/session.h"
#include "core/uco.h"
#include "core/udaemon.h"
#include "core/ulog.h"
#include "demo/demo.pb.h"
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include "core/usql.h"
#include "core/uredis.h"

using namespace uco;

void BlockServerSignals()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGPIPE);

    const int ret = pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    if (ret != 0)
    {
        SYSFTL("pthread_sigmask:", strerror(ret));
    }
}

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

task<void> hello(HttpContext *context)
{
    demo::User user;
    user.set_username("joker");
    user.set_age(26);
    context->Json(200, user);
    co_return;
}

task<void> status_page(HttpContext *context)
{
    context->Status(200);
    co_return;
}

// 存活探测: 无状态, 无日志噪音.
task<void> ping(HttpContext *context)
{
    context->DisableLog();
    context->String(200, "pong");
    co_return;
}

// mustache is quiet slow.
task<void> template_page(HttpContext *context)
{
    demo::JokeTemplate user;
    user.set_name("小丑🤡");
    user.set_age(30);
    demo::Address *address1 = user.add_addresses();
    address1->set_street("洪山区珞喻路129号");
    address1->set_city("武汉");
    demo::Address *address2 = user.add_addresses();
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

task<void> post(HttpContext *context)
{
    demo::User user;
    context->BindJSON(user);
    LOGMSG(NR(user.username()));
    LOGMSG(NR(user));
    context->Json(200, user);
    co_return;
}

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

void PrepareStaticResource(HttpServer& httpserver)
{
    // 静态资源.
    httpserver.Static("/css/*");
    httpserver.Static("/fonts/*filename");
    httpserver.Static("/images/*filename");
    httpserver.Static("/js/*filename");
    httpserver.Static("/video/*filename");
    httpserver.Static("/music/*filename");

    // 静态页面.
    httpserver.Static("/index.html");
    httpserver.Static("/video.html");
    httpserver.Static("/picture.html");
}

void PrepareForward(HttpServer& httpserver)
{
    httpserver.Forward(eGet, "/", "/index.html");
    httpserver.Forward(eGet, "/index", "/index.html");
    httpserver.Forward(eGet, "/video", "/video.html");
    httpserver.Forward(eGet, "/picture", "/picture.html");
    // 直链 .html 一律经动态路由 (登录态检查), 防绕过.
    httpserver.Forward(eGet, "/login.html", "/login");
    httpserver.Forward(eGet, "/register.html", "/register");
    httpserver.Forward(eGet, "/welcome.html", "/welcome");
}

void PrepareErrorPage(HttpServer& httpserver)
{
    // 静态错误页面.
    httpserver.ErrorPage(400, "400.html");
    httpserver.ErrorPage(403, "403.html");
    httpserver.ErrorPage(404, "404.html");
    httpserver.ErrorPage(500, "500.html");

    // 通用错误模板.
    httpserver.ErrorTemplate("templates/error.html");
}

void PrepareDemo(HttpServer& httpserver)
{
    httpserver.GET("/hello", middleware1, middleware2, middleware3, hello);
    httpserver.GET("/redirect", redirect);
    httpserver.GET("/status", status_page);
    httpserver.GET("/ping", ping);
    httpserver.GET("/template", template_page);
    httpserver.HEAD("/head", head);
    httpserver.POST("/post", post);
}

task<void> RunHttpServer(HttpServer& httpserver, uco::uthread_pool& thread_pool)
{
    // 1. 启动服务，等待其运行结束 (信号通知).
    co_await httpserver.Run();
    // 2. 关闭线程池.
    co_await thread_pool.close();
}

task<void> RunHttpServer()
{
    using namespace webserver;
    // 7. 定义 Server 实例.
    HttpServer httpserver(8080, 4, 100, 30);

    // 0. 定义 CPU 线程池.
    uco::uthread_pool thread_pool(1, 32);

    // 定义密码哈希器.
    security::BcryptPasswordHasher hasher(thread_pool);

    // 1. 创建连接池.
    auto mysql_pool = MakeMySqLPool();
    auto redis_pool = MakeRedisPool();

    // 2. 定义 DAO 实例 (依赖 SQL 连接池).
    dao::MySqlUserDao userDAO(mysql_pool);

    // 3. 定义 Service 实例 (依赖 DAO).
    service::UserService userService(userDAO, hasher);

    // 4. 定义 Controller 实例 (依赖 Service).
    controller::UserController userController(userService);

    // 5. 定义 Session.
    SessionStore::Config session_cfg;
    // 密钥一般存于配置文件，此处偷懒直接 hardcode.
    session_cfg.secret_key = "uco-session-secret";
    // SessionStore 依赖 Redis 连接池.
    SessionStore store(session_cfg, redis_pool);
    // cookie 中存 session 的 key 名.
    const std::string kSessionName = "uco_session";
    // session 中存 csrf_token 的 key 名.
    const std::string &kCsrfKey = controller::kCsrfSessionKey;

    // 6. 限流配置.
    constexpr int kIpRatePerMin = 256; // IP 层限流阈值 (次/分钟).
    constexpr int kNewSessionIpRatePerMin = 30; // 仅新建匿名会话按 IP 计数, 已有会话不限制频率.

    // 8. 定义业务接口.
    PrepareStaticResource(httpserver);
    PrepareForward(httpserver);
    PrepareErrorPage(httpserver);
    PrepareDemo(httpserver);

    httpserver.GET(
        "/api/csrf",
        Sessions(&store, kSessionName),
        ratelimit::FixedWindow(kNewSessionIpRatePerMin, 60, ratelimit::KeyBy::NewSessionIP),
        csrf::SessionIssue(kCsrfKey, 600)
    );
    httpserver.GET(
        "/api/me",
        Sessions(&store, kSessionName),
        MakeHandler(userController, CurrentUser)
    );
    httpserver.GET(
        "/register",
        Sessions(&store, kSessionName),
        MakeHandler(userController, RegisterPage)
    );
    httpserver.POST(
        "/register",
        ratelimit::FixedWindow(kIpRatePerMin, 60),
        Sessions(&store, kSessionName),
        MakeHandler(userController, RequireAnonymous),
        ratelimit::FixedWindow(5, 60, ratelimit::KeyBy::SessionID),
        csrf::SessionCheck(kCsrfKey),
        MakeHandler(userController, Register)
    );
    httpserver.GET(
        "/login",
        Sessions(&store, kSessionName),
        MakeHandler(userController, LoginPage)
    );
    httpserver.POST(
        "/login",
        ratelimit::FixedWindow(kIpRatePerMin, 60),
        Sessions(&store, kSessionName),
        ratelimit::FixedWindow(10, 60, ratelimit::KeyBy::Account),
        csrf::SessionCheck(kCsrfKey),
        MakeHandler(userController, Login)
    );
    httpserver.POST(
        "/logout",
        ratelimit::FixedWindow(kIpRatePerMin, 60),
        Sessions(&store, kSessionName),
        ratelimit::FixedWindow(10, 60, ratelimit::KeyBy::SessionID),
        csrf::SessionCheck(kCsrfKey),
        MakeHandler(userController, Logout)
    );
    httpserver.GET(
        "/welcome",
        Sessions(&store, kSessionName),
        MakeHandler(userController, WelcomePage)
    );

    // 9. 阻塞等待服务运行结束.
    // 此方案主线程不会闲置，作为一个CPU工作线程存在.
    uco::cobatch batchrunner;
    batchrunner.add(thread_pool.add_current());
    batchrunner.add(RunHttpServer(httpserver, thread_pool));
    co_await batchrunner.run();

    // 此方案主线程会闲置.
    // co_await httpserver.Run();
    // co_await thread_pool.close();
}

int main(int argc, const char *argv[])
{
    // 1. 开启日志系统.
    uco::OpenLog(
        "ucohttpsvr",
        LogLevel::INFO,
        LogMode::CONSOLE,
        true
    );

    // 2. 在创建任何线程前屏蔽服务信号，后续线程继承该信号掩码.
    BlockServerSignals();

    // 3. 进程初始化.
    uco::InitProcess(false, "ucohttpsvr");

    // 4. 启动服务.
    go RunHttpServer();

    return 0;
}
