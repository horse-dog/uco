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

task<void> RunHttpServer(uco::YamlConfig app_config)
{
    using namespace webserver;
    // 1. 定义 server 实例.
    HttpServer httpserver(app_config);

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
    csrf::Csrf csrf(app_config);

    // 8. 定义 SessionStore.
    SessionStore store(app_config, redis_pool);

    // 9. 定义限流器.
    ratelimit::FixedWindow limiter(app_config);

    // 10. 定义 Controller 实例.
    /// TODO: 这里依赖 csrf.SessionKey(), 感觉不好.
    controller::UserController userController(userService, csrf.SessionKey());

    // 11. 定义业务接口.
    PrepareStaticResource(httpserver);
    PrepareForward(httpserver);
    PrepareErrorPage(httpserver);
    PrepareDemo(httpserver);

    httpserver.GET(
        "/api/csrf",
        MakeHandler(store, Sessions),
        MakeHandler(limiter, ByNewSessionIP),
        MakeHandler(csrf, SessionIssue)
    );
    httpserver.GET(
        "/api/me",
        MakeHandler(store, Sessions),
        MakeHandler(userController, CurrentUser)
    );
    httpserver.GET(
        "/register",
        MakeHandler(store, Sessions),
        MakeHandler(userController, RegisterPage)
    );
    httpserver.POST(
        "/register",
        MakeHandler(limiter, ByIP),
        MakeHandler(store, Sessions),
        MakeHandler(userController, RequireAnonymous),
        MakeHandler(limiter, BySession),
        MakeHandler(csrf, SessionCheck),
        MakeHandler(userController, Register)
    );
    httpserver.GET(
        "/login",
        MakeHandler(store, Sessions),
        MakeHandler(userController, LoginPage)
    );
    httpserver.POST(
        "/login",
        MakeHandler(limiter, ByIP),
        MakeHandler(store, Sessions),
        MakeHandler(limiter, ByAccount),
        MakeHandler(csrf, SessionCheck),
        MakeHandler(userController, Login)
    );
    httpserver.POST(
        "/logout",
        MakeHandler(limiter, ByIP),
        MakeHandler(store, Sessions),
        MakeHandler(limiter, BySession),
        MakeHandler(csrf, SessionCheck),
        MakeHandler(userController, Logout)
    );
    httpserver.GET(
        "/welcome",
        MakeHandler(store, Sessions),
        MakeHandler(userController, WelcomePage)
    );

    // 9. 阻塞等待服务运行结束.
    co_await httpserver.Run();
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
    const auto blocked_signals =
        config.GetList<int>("process.blocked_signals");
    uco::InitProcess(daemonize, module, blocked_signals);

    // 5. 启动服务, 异步任务需要值传递 config.
    go RunHttpServer(std::move(config));
    return 0;
}
