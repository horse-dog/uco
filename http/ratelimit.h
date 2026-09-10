#pragma once

/**
 * @file ratelimit.h
 * @brief HTTP 限流中间件 (进程内固定窗口计数, 多维度 key).
 *
 * 设计对齐业界惯例 "外层按 IP 宽松, 内层按身份严格" (防御纵深):
 *   - IP 维度: 挂链首 (先于 Sessions), 宽阈值只挡脚本洪水, 为
 *     NAT 共享出口留余量; 被拒请求不建 session, 不触达 Redis/DB;
 *   - SessionID 维度: 挂 Sessions() 之后, NAT 用户各持独立会话,
 *     互不连坐; 攻击者换会话绕行须先过 /api/csrf 的 IP 层;
 *   - Account 维度: 挂 Sessions() 之后 (登录场景), 按表单
 *     username 字段计数, 单账号爆破被按账号封堵, 不影响 NAT 邻居;
 *   - NewSessionIP 维度: 挂 Sessions() 之后, 仅会话为新建 (无
 *     cookie/验签失败/已过期) 时按 IP 计数, 守会话创建入口:
 *     老访客不占配额 (NAT 免疫), 被拒走 Abort 路径新会话不落盘.
 *
 * 工厂模式与 Sessions()/csrf::SessionCheck() 一致:
 * @code
 *   httpserver.POST("/login",
 *       ratelimit::FixedWindow(30, 60), // IP 宽松 (链首)
 *       Sessions(&store, name),
 *       ratelimit::FixedWindow(5, 60, ratelimit::ByAccount),
 *       csrf::SessionCheck(key), MakeHandler(ctrl, Login));
 * @endcode
 *
 * @note 算法为固定窗口计数: 实现最简、内存 O(主体数) 且常数小;
 *       代价是窗口边界处至多 2*limit 的突刺 (防滥用/防爆破可
 *       接受); 需平滑限速时以并列工厂新增 (如令牌桶), 不改调用方;
 * @note 计数为进程内共享 (worker 线程间互斥访问); 多实例部署时
 *       各实例独立限额, 总量为实例数倍 (横向扩展时需迁移 Redis);
 * @note 各挂载点 (工厂调用) 独立计数, 互不共享额度;
 * @note 超限统一 429 JSON + Retry-After 头并 Abort, 拒绝响应
 *       风格与 csrf 一致;
 * @note Account 维度经 PeekRawData 非破坏性解析, 不消费 body,
 *       不影响后续 BindForm; 字段名和值按 urlencoded 表单规则解码,
 *       保证等价编码使用同一个计数 key; 解析不到字段则该请求不计入
 *       (由链上后续表单校验拒绝).
 */

#include "http/httpserver.h" // HttpServer::HandleFunc

namespace ratelimit
{

/// 计数维度.
enum class KeyBy
{
    IP,        ///< 客户端 IP (HttpContext::ClientIP), 任何位置可挂.
    SessionID, ///< 会话 ID, 须挂于 Sessions() 之后.
    Account,   ///< 表单 username 字段 (登录/注册), 须挂于 Sessions() 之后.
    /// 会话为新建 (无 cookie/验签失败/已过期) 时才按 IP 计数,
    /// 须挂于 Sessions() 之后; 用于守会话创建入口: 老访客 (已有
    /// 有效会话) 空 key 跳过计数, 不占 NAT 配额.
    NewSessionIP,
};

/**
 * @brief 固定窗口限流中间件.
 * @param limit      窗口内允许的最大请求数 (>0).
 * @param window_sec 窗口宽度秒数 (>0).
 * @param key        计数维度 (默认 IP); 每次调用产生独立计数器.
 * @return 限流中间件.
 */
HttpServer::HandleFunc FixedWindow(int limit, int window_sec,
                                   KeyBy key = KeyBy::IP);

} // namespace ratelimit
