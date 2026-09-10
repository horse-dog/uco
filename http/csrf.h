#pragma once

/**
 * @file csrf.h
 * @brief CSRF 机制 (基于 session 的 synchronizer token 实现).
 *
 * 与 session.h 的 Sessions() 配套 (依赖 session, 中间件须挂于
 * Sessions() 之后):
 * @code
 *   httpserver.GET("/api/csrf", Sessions(&store, name),
 *                  csrf::SessionIssue(key));
 *   httpserver.POST("/login", Sessions(&store, name),
 *                  csrf::SessionCheck(key), MakeHandler(ctrl, Login));
 * @endcode
 *
 * @note token 存 session[key], 经 X-CSRF-Token 请求头提交 (跨站 JS
 *       无法携带自定义头, 发出前即被 CORS preflight 拦截);
 * @note 校验失败统一 403 JSON (不区分无 token/不匹配, 防探测);
 * @note 机制细节 (键名/响应) 由入参与通用 JSON 固化, 与业务 DTO 解耦;
 * @note 未来其他机制 (如双提交 cookie, 不依赖 session) 以并列工厂
 *       新增, 不改调用方语义.
 */

#include "http/httpserver.h" // HttpServer::HandleFunc

#include <string>

namespace csrf
{

/**
 * @brief 签发端点: session[key] 为空则生成新 token (已有则复用),
 *        响应走统一信封.
 * @param key          token 的 session 键名 (与 SessionCheck 一致).
 * @param anon_max_age 新生会话的 TTL 秒数 (默认 600).
 * @return 签发端点中间件.
 */
HttpServer::HandleFunc SessionIssue(const std::string &key,
                                    int anon_max_age = 600);

/**
 * @brief 校验中间件: X-CSRF-Token 头与 session 内 token 恒时比对,
 *        失败写 403 并 Abort, 通过续链.
 * @param key token 的 session 键名 (与 SessionIssue 一致).
 * @return 校验中间件 (挂于 Sessions 之后).
 */
HttpServer::HandleFunc SessionCheck(const std::string &key);

/**
 * @brief 生成随机 CSRF token (32 字节 CSPRNG 的 64 字符 hex).
 * @return 64 字符 hex; CSPRNG 故障时 LOGFTL 并返回空串.
 */
std::string NewToken();

} // namespace csrf
