#include "httpserver.h"
#include "uco.h"
#include "udaemon.h"
#include "ulog.h"
#include "user.pb.h"
#include <cstdio>
#include <sys/eventfd.h>
#include <unistd.h>

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
    CTXNEXT(context);
    LOGDBG("after");
    co_return;
}

task<void> middleware2(HttpContext *context)
{
    LOGDBG("before");
    CTXNEXT(context);
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
    User user;
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

task<void> post(HttpContext *context)
{
    User user;
    BindJSON(context, user);
    LOGMSG(NR(user.username()));
    LOGMSG(NR(user));
    context->Json(200, user);
    co_return;
}

int main(int argc, const char *argv[])
{
    uco::OpenLog("ucohttpsvr", LogLevel::INFO, LogMode::FILE, true);
    uco::InitProcess(false, "ucohttpsvr");
    httpserver.Static("/css/*");
    httpserver.Static("/fonts/*filename");
    httpserver.Static("/images/*filename");
    httpserver.Static("/js/*filename");
    httpserver.Static("/video/*filename");
    httpserver.Static("/music/*filename");

    httpserver.Static("/index.html");
    httpserver.Static("/register.html");
    httpserver.Static("/login.html");
    httpserver.Static("/welcome.html");
    httpserver.Static("/video.html");
    httpserver.Static("/picture.html");

    httpserver.ErrorPage(400, "400.html");
    httpserver.ErrorPage(403, "403.html");
    httpserver.ErrorPage(404, "404.html");
    httpserver.ErrorPage(500, "500.html");
    httpserver.ErrorTemplate("templates/error.html");

    httpserver.Forward("/", "/index.html");
    httpserver.Forward("/index", "/index.html");
    httpserver.Forward("/register", "/register.html");
    httpserver.Forward("/login", "/login.html");
    httpserver.Forward("/welcome", "/welcome.html");
    httpserver.Forward("/video", "/video.html");
    httpserver.Forward("/picture", "/picture.html");

    httpserver.GET("/hello", middleware1, middleware2, middleware3, hello);
    httpserver.GET("/redirect", redirect);
    httpserver.GET("/status", status_page);
    httpserver.GET("/template", template_page);
    httpserver.HEAD("/head", head);
    httpserver.POST("/post", post);
    httpserver.Init(8080, 4, 100, 30);
    httpserver.Run();
    return 0;
}
