#pragma once

/**
 * @file ratelimit.h
 * @brief HTTP 限流中间件接口及固定窗口实现。
 *
 * 限流方法只描述计数维度，不感知注册、登录、登出等业务路由：
 *   - ByIP：按客户端 IP；
 *   - ByNewSessionIP：仅新建匿名会话时按客户端 IP；
 *   - BySession：按 Session ID；
 *   - ByAccount：按登录表单 username。
 *
 * 每次调用 ByXXX() 都创建并由返回闭包独占一个计数器，因此不同路由挂载点
 * 的额度互相隔离；闭包通过 shared_ptr 保证计数器生命周期覆盖请求处理。
 *
 * @code
 *   ratelimit::FixedWindow limiter(config);
 *   httpserver.POST("/login",
 *       limiter.ByIP(),
 *       MakeHandler(store, Sessions),
 *       MakeHandler(csrf, SessionCheck),
 *       limiter.ByAccount(),
 *       MakeHandler(controller, Login));
 * @endcode
 */

#include "http/httpserver.h"

#include <memory>

namespace uco
{
class YamlConfig;
}

namespace ratelimit
{

/** @brief 按计数维度创建限流中间件的接口。 */
class Limiter
{
  public:
    virtual ~Limiter() = default;

    virtual HttpServer::HandleFunc ByIP(HttpContext *ctx) = 0;
    virtual HttpServer::HandleFunc ByNewSessionIP(HttpContext *ctx) = 0;
    virtual HttpServer::HandleFunc BySession(HttpContext *ctx) = 0;
    virtual HttpServer::HandleFunc ByAccount(HttpContext *ctx) = 0;
};

/**
 * @brief 固定窗口限流实现。
 *
 * 构造时保存配置快照；调用 ByXXX() 时从 rate_limit.* 读取对应维度阈值，
 * 并创建一个由返回闭包持有的独立固定窗口计数器。
 */
class FixedWindow final : public Limiter
{
  public:
    explicit FixedWindow(const uco::YamlConfig &config);
    ~FixedWindow() override;

    FixedWindow(const FixedWindow &) = delete;
    FixedWindow &operator=(const FixedWindow &) = delete;
    FixedWindow(FixedWindow &&) = delete;
    FixedWindow &operator=(FixedWindow &&) = delete;

    HttpServer::HandleFunc ByIP(HttpContext *ctx) override;
    HttpServer::HandleFunc ByNewSessionIP(HttpContext *ctx) override;
    HttpServer::HandleFunc BySession(HttpContext *ctx) override;
    HttpServer::HandleFunc ByAccount(HttpContext *ctx) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ratelimit
