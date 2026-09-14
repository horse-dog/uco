#pragma once

/**
 * @file ratelimit.h
 * @brief HTTP 限流中间件接口及固定窗口实现。
 *
 * 限流方法只描述计数维度，不感知注册、登录、登出等业务路由：
 *   - ByKey：由用户注入 KeyGetter；
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

#include "http/httpserver.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ratelimit
{

/**
 * @brief 从当前请求提取限流键；结果原样传给 RejectHandler。
 * @note 默认 RejectHandler 会跳过空键；自定义 RejectHandler 可自行定义空键语义。
 *       回调会被复制到中间件中，其捕获对象须满足正常的值语义。
 */
using KeyGetter = std::function<std::string(HttpContext *)>;

/** @brief 单个固定窗口限流中间件的并发安全计数器。 */
class Counter
{
  public:
    Counter(int limit, int window_sec);

    Counter(const Counter &) = delete;
    Counter &operator=(const Counter &) = delete;
    Counter(Counter &&) = delete;
    Counter &operator=(Counter &&) = delete;

    /** @brief 记录 key 的本次请求并判断是否允许；建议重试秒数经 RetryAfter 获取。 */
    bool Allow(const std::string &key);

    /**
     * @brief 无副作用地计算当前固定窗口的剩余秒数。
     * @return 向上取整后的剩余秒数，最小为 1。
     */
    int RetryAfter() const;

  private:
    struct Entry
    {
        uint64_t window = 0;
        uint64_t count = 0;
    };

    int m_limit;
    uint64_t m_windowMs;
    std::mutex m_mutex;
    std::unordered_map<std::string, Entry> m_entries;
};

/**
 * @brief 单个限流中间件的自定义处理器。
 * @param ctx 当前请求上下文。
 * @param counter 当前中间件独占的计数器。
 * @param key 本次请求的限流键，KeyGetter 返回空串时这里也为空。
 * @return true 表示拒绝请求，false 表示继续请求链。
 * @note 处理器负责调用 Counter::Allow 并按需生成响应，且不得调用 Next()。
 *       可以返回 true，也可以调用 Abort() 拒绝请求；限流器都会重新获取最新
 *       重试时间、覆盖 Retry-After 并确保请求链中止。
 */
using RejectHandler = std::function<
    uco::task<bool>(HttpContext *, Counter &, const std::string &)>;

/**
 * @brief 固定窗口限流实现。
 *
 * 构造时保存配置快照；调用 ByXXX() 时从 rate_limit.* 读取对应维度阈值，
 * 并创建一个由返回闭包持有的独立固定窗口计数器。
 */
class FixedWindow
{
  public:
    FixedWindow();
    ~FixedWindow() = default;

    FixedWindow(const FixedWindow &) = delete;
    FixedWindow &operator=(const FixedWindow &) = delete;
    FixedWindow(FixedWindow &&) = delete;
    FixedWindow &operator=(FixedWindow &&) = delete;

    /**
     * @brief 使用自定义键提取器创建固定窗口限流处理函数。
     * @param key_getter 每个请求调用一次，返回值原样传给 reject_handler。
     * @param limit 窗口内允许的最大请求数，必须大于 0。
     * @param window_sec 固定窗口秒数，必须大于 0。
     * @param reject_handler 当前中间件的拒绝处理器；为空时使用默认逻辑。
     * @return 独占当前路由挂载点计数器的处理函数。
     */
    HttpServer::HandleFunc
    ByKey(KeyGetter key_getter, int limit, int window_sec,
          RejectHandler reject_handler = {});

    /**
     * @brief 按客户端 IP 计数的处理函数。
     * @return 独占当前路由挂载点计数器的处理函数。
     */
    HttpServer::HandleFunc
    ByIP(int limit = 0, int window_sec = 60,
         RejectHandler reject_handler = {});

    /**
     * @brief 按 Session ID 计数的处理函数。
     * @return 独占当前路由挂载点计数器的处理函数。
     */
    HttpServer::HandleFunc
    BySession(int limit = 0, int window_sec = 60,
              RejectHandler reject_handler = {});
};

} // namespace ratelimit
