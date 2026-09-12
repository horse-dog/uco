#pragma once

/**
 * @file csrf.h
 * @brief CSRF 机制 (基于 session 的 synchronizer token 实现).
 *
 * 与 SessionStore::Sessions() 配套 (依赖 session, 中间件须挂于
 * MakeHandler(store, Sessions) 之后):
 * @code
 *   csrf::Csrf csrf(config); // 读取 csrf.* 配置
 *   httpserver.GET("/api/csrf", MakeHandler(store, Sessions),
 *                  MakeHandler(csrf, SessionIssue));
 *   httpserver.POST("/login", MakeHandler(store, Sessions),
 *                  MakeHandler(csrf, SessionCheck),
 *                  MakeHandler(ctrl, Login));
 * @endcode
 *
 * @note token 存 session[key], 经 X-CSRF-Token 请求头提交 (跨站 JS
 *       无法携带自定义头, 发出前即被 CORS preflight 拦截);
 * @note 校验失败统一 403 JSON (不区分无 token/不匹配, 防探测);
 * @note 机制细节 (键名/响应) 由 csrf.* 配置与通用 JSON 固化,
 *       与业务 DTO 解耦;
 * @note 未来其他机制 (如双提交 cookie, 不依赖 session) 以并列
 *       成员函数新增, 不改调用方语义.
 */

#include "http/httpserver.h" // HttpContext / MakeHandler / YamlConfig.

#include <string>

namespace csrf
{

/**
 * @brief CSRF 机制配置对象 (≈ SessionStore 的定位): 普通对象,
 *        由使用方构造持有 (构造注入 YamlConfig), 中间件成员协程经
 *        MakeHandler(csrf, SessionXXX) 绑定 (≈ gin 的方法值).
 */
class Csrf
{
  public:
    /// 从 csrf.* 读取配置并实体化 (构造时复制所需字段).
    /// @param config 应用 YAML 配置.
    explicit Csrf(const uco::YamlConfig &config);
    ~Csrf();

    Csrf(const Csrf &) = delete;
    Csrf &operator=(const Csrf &) = delete;
    Csrf(Csrf &&) = delete;
    Csrf &operator=(Csrf &&) = delete;

    /**
     * @brief 签发端点: session[key] 为空则生成新 token (已有则复用),
     *        响应走统一信封; 可经 MakeHandler(csrf, SessionIssue) 绑定.
     */
    uco::task<void> SessionIssue(HttpContext *ctx);

    /**
     * @brief 校验中间件: X-CSRF-Token 头与 session 内 token 恒时比对,
     *        失败写 403 并 Abort, 通过续链; 可经
     *        MakeHandler(csrf, SessionCheck) 绑定 (挂于 Sessions 之后).
     */
    uco::task<void> SessionCheck(HttpContext *ctx);

    /** @brief token 的 session 键名 (签发/校验/登录态刷新共用). */
    const std::string &SessionKey() const { return m_sessionKey; }

    /**
     * @brief 生成随机 CSRF token (32 字节 CSPRNG 的 64 字符 hex).
     * @return 64 字符 hex; CSPRNG 故障时 LOGFTL 并返回空串.
     */
    static std::string NewToken();

  private:
    std::string m_sessionKey; ///< token 的 session 键名 (csrf.session_key).
    int m_anonMaxAge = 600;   ///< 新生匿名会话 TTL 秒 (csrf.anonymous_max_age_sec).
};

} // namespace csrf
