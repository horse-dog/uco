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
 *       MakeHandler(limiter, ByIP),
 *       MakeHandler(store, Sessions),
 *       MakeHandler(csrf, SessionCheck),
 *       MakeHandler(limiter, ByAccount),
 *       MakeHandler(controller, Login));
 * @endcode
 */

#include "core/uconfig.h"
#include "http/httpserver.h"

namespace ratelimit
{

/** @brief 按计数维度创建限流中间件的接口。 */
class Limiter
{
  public:
    virtual ~Limiter() = default;

    /**
     * @brief 创建按客户端 IP 计数的限流处理函数。
     * @return 独占当前路由挂载点计数器的处理函数。
     */
    virtual HttpServer::HandleFunc ByIP() = 0;

    /**
     * @brief 创建仅对新匿名会话按客户端 IP 计数的限流处理函数。
     * @return 独占当前路由挂载点计数器的处理函数。
     */
    virtual HttpServer::HandleFunc ByNewSessionIP() = 0;

    /**
     * @brief 创建按 Session ID 计数的限流处理函数。
     * @return 独占当前路由挂载点计数器的处理函数。
     */
    virtual HttpServer::HandleFunc BySession() = 0;

    /**
     * @brief 创建按登录表单账号计数的限流处理函数。
     * @return 独占当前路由挂载点计数器的处理函数。
     */
    virtual HttpServer::HandleFunc ByAccount() = 0;
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
    ~FixedWindow() override = default;

    FixedWindow(const FixedWindow &) = delete;
    FixedWindow &operator=(const FixedWindow &) = delete;
    FixedWindow(FixedWindow &&) = delete;
    FixedWindow &operator=(FixedWindow &&) = delete;

    /** @copydoc Limiter::ByIP */
    HttpServer::HandleFunc ByIP() override;

    /** @copydoc Limiter::ByNewSessionIP */
    HttpServer::HandleFunc ByNewSessionIP() override;

    /** @copydoc Limiter::BySession */
    HttpServer::HandleFunc BySession() override;

    /** @copydoc Limiter::ByAccount */
    HttpServer::HandleFunc ByAccount() override;

  private:
    uco::YamlConfig m_config;
};

} // namespace ratelimit
