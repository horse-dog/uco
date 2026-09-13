#include "core/thread_pool.h"
#include "server/controller/user.h"
#include "dao/user_mysql.h"
#include "service/user.h"
#include "security/password_hasher.h"
#include "http/csrf.h"
#include "http/httpserver.h"
#include "http/ratelimit.h"
#include "http/session.h"
#include "core/uco.h"
#include "core/uconfig.h"
#include "core/udaemon.h"
#include "core/ulog.h"
#include "demo/demo.pb.h"
#include <cstdio>
#include <utility>
#include <sys/eventfd.h>
#include <unistd.h>
#include "core/usql.h"
#include "core/uredis.h"

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
    demo::User usr;
    usr.set_username("joker");
    usr.set_age(100);

    // key store.
    auto guard = context->Store("user", &usr);
    co_await context->Next();
    LOGDBG("after");
    demo::Address& addr = context->Get<demo::Address>("addr");
    LOGDBG(NR(addr));
    co_return;
}

task<void> middleware2(HttpContext *context)
{
    LOGDBG("before");
    demo::User* usr = (demo::User*)context->Load("user");
    if (usr) LOGDBG(NR(*usr));

    demo::Address addr;
    addr.set_city("New York");
    addr.set_street("Wall Street");

    // key set.
    context->Set("addr", addr);

    co_await context->Next();
    LOGDBG("after");
    co_return;
}

task<void> middleware3(HttpContext *context)
{
    LOGDBG("before");
    demo::Address& addr = context->Get<demo::Address>("addr");
    LOGDBG(NR(addr));
    addr.set_street("null");
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

void PrepareStaticResource(HttpServer& svr)
{
    {
        auto g = svr.Group("", CacheControl("public, max-age=3600"));
        g.Static("/css/*");
        g.Static("/fonts/*filename");
        g.Static("/images/*filename");
        g.Static("/js/*filename");
    }

    {
        auto g = svr.Group("", CacheControl("public, max-age=86400"));
        g.Static("/video/*filename");
        g.Static("/music/*filename");
    }

    {
        // HTML 需要及时获取新版本，允许存储但每次使用前必须验证。
        auto g = svr.Group("", CacheControl("no-cache"));
        g.Static("/index.html");
        g.Static("/video.html");
        g.Static("/picture.html");
    }
}

void PrepareForward(HttpServer& svr)
{
    svr.Forward(eGet, "/", "/index.html");
    svr.Forward(eGet, "/index", "/index.html");
    svr.Forward(eGet, "/video", "/video.html");
    svr.Forward(eGet, "/picture", "/picture.html");
    // 直链 .html 一律经动态路由 (登录态检查), 防绕过.
    svr.Forward(eGet, "/login.html", "/login");
    svr.Forward(eGet, "/register.html", "/register");
    svr.Forward(eGet, "/welcome.html", "/welcome");
}

void PrepareErrorPage(HttpServer& svr)
{
    // 静态错误页面.
    svr.ErrorPage(400, "400.html");
    svr.ErrorPage(403, "403.html");
    svr.ErrorPage(404, "404.html");
    svr.ErrorPage(500, "500.html");

    // 通用错误模板.
    svr.ErrorTemplate("templates/error.html");
}

void PrepareDemo(HttpServer& svr)
{
    auto dynamic = svr.Group("", CacheControl("no-cache"));
    dynamic.GET("/hello", middleware1, middleware2, middleware3, hello);
    dynamic.GET("/redirect", redirect);
    dynamic.GET("/status", status_page);
    dynamic.GET("/ping", ping);
    dynamic.GET("/template", template_page);
    dynamic.HEAD("/head", head);
    dynamic.POST("/post", post);
}

task<void> RunHttpServer(uco::YamlConfig app_config)
{
    using namespace webserver;

    // 1. 定义 server 实例.
    HttpServer svr(app_config);

    // 2. 定义 cpu 线程池.
    size_t cpu_threads = app_config.Get<size_t>("cpu_pool.threads", 1);
    size_t cpu_max_pending = app_config.Get<size_t>("cpu_pool.max_pending", 32);
    uco::thread_pool thread_pool(cpu_threads, cpu_max_pending);

    // 3. 定义密码哈希组件 (重 cpu 逻辑, 依赖注入 cpu 线程池).
    security::BcryptPasswordHasher hasher(thread_pool);

    // 4. 定义连接池.
    usql::upool mysql_pool(app_config);
    uredis::upool redis_pool(app_config);

    // 5. 定义 DAO 实例 (依赖 SQL 连接池).
    dao::MySqlUserDao userDAO(mysql_pool);

    // 6. 定义 Service 实例 (依赖 DAO).
    service::UserService userService(userDAO, hasher);

    // 7. CSRF 组件.
    csrf::Csrf csrf;

    // 8. 定义 SessionStore.
    SessionStore store(app_config, redis_pool);

    // 9. 定义限流器.
    ratelimit::FixedWindow limiter(app_config);

    // 10. 定义 Controller 实例.
    controller::UserController userController(userService);

    // 11. 定义业务接口.
    PrepareStaticResource(svr);
    PrepareForward(svr);
    PrepareErrorPage(svr);
    PrepareDemo(svr);

    // 会话及认证相关响应（包括中间件提前返回的 4xx）均禁止存储。
    auto baseGrp = svr.Group(
        "",
        CacheControl("private, no-store"),
        MakeHandler(store, Sessions)
    );
    auto postGrp = baseGrp.Group(
        "", MakeHandler(csrf, SessionCheck)
    );

    baseGrp.GET("/api/me", MakeHandler(userController, CurrentUser));
    baseGrp.GET("/register", MakeHandler(userController, RegisterPage));
    baseGrp.GET("/login", MakeHandler(userController, LoginPage));
    baseGrp.GET("/welcome", MakeHandler(userController, WelcomePage));
    baseGrp.GET(
        "/api/csrf",
        MakeHandler(limiter, ByNewSessionIP),
        MakeHandler(csrf, SessionIssue)
    );
    postGrp.POST(
        "/register",
        MakeHandler(limiter, ByIP),
        MakeHandler(limiter, BySession),
        MakeHandler(userController, RequireAnonymous),
        MakeHandler(userController, Register)
    );
    postGrp.POST(
        "/login",
        MakeHandler(limiter, ByIP),
        MakeHandler(limiter, ByAccount),
        MakeHandler(userController, Login)
    );
    postGrp.POST(
        "/logout",
        MakeHandler(limiter, ByIP),
        MakeHandler(limiter, BySession),
        MakeHandler(userController, Logout)
    );

    // 9. 阻塞等待服务运行结束.
    co_await svr.Run();
}

void ParseArgs(int argc, const char* argv[], bool& daemonize, std::string& config_path)
{
    daemonize = false; // default value.
    config_path = PROJECT_SOURCE_DIR "/server/config.yaml"; // default value.
    for (int i = 1; i < argc; ++i)
    {
        const std::string option = argv[i];
        if (option == "-c" && i + 1 < argc)
        {
            config_path = argv[++i];
        }
        else if (option == "-d" && i + 1 < argc)
        {
            const std::string value = argv[++i];
            if (value == "0")
                daemonize = false;
            else if (value == "1")
                daemonize = true;
            else
            {
                fprintf(stderr, "-d must be 0 or 1\n");
                exit(1);
            }
        }
        else
        {
            fprintf(stderr, "usage: %s [-c config] [-d 0|1]\n", argv[0]);
            exit(1);
        }
    }
}

int main(int argc, const char *argv[])
{
    // 1. 解析命令行参数.
    bool daemonize = false;
    std::string config_path;
    ParseArgs(argc, argv, daemonize, config_path);

    // 2. 加载配置文件.
    uco::YamlConfig config;
    if (!config.Load(config_path))
    {
        fprintf(stderr, "failed to load server config: %s\n",
                config_path.c_str());
        return 1;
    }

    auto module = config.Get<std::string>("module_name", "ucohttpsvr");

    // 3. 开启日志系统.
    auto level = config.Get<LogLevel>("logging.level", LogLevel::INFO);
    auto mode = config.Get<LogMode>("logging.mode", LogMode::CONSOLE);
    auto syslog = config.Get<bool>("logging.enable_system_logs", true);
    uco::OpenLog(module, level, mode, syslog);

    // 4. 进程初始化，并在创建任何线程前屏蔽服务信号；后续线程继承掩码.
    const auto blocked_signals = config.GetList<int>("process.blocked_signals");
    uco::InitProcess(daemonize, module, blocked_signals);

    // 5. 启动服务, 异步任务需要值传递 config.
    go RunHttpServer(std::move(config));
    return 0;
}
