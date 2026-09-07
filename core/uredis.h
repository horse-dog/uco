#pragma once

/**
 * @file uredis.h
 * @brief 基于 uco (io_uring + C++20 协程) 的 Redis 异步基本 API.
 *
 * @note 直接实现 RESP2 协议, 不依赖 hiredis;
 *       所有接口默认超时 10s, ts = {0, 0} 表示不超时 (慎用);
 *       超时时结果中 ok = false 且 timeout = true;
 *       命令以参数数组下发, 天然免疫注入, 无需转义;
 *       传输层错误或超时后连接标记为 broken, 不可再复用;
 *       host 仅支持点分 IPv4, 域名请自行解析后传入.
 */

#include "uio.h"
#include "usync.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace uredis
{

/// 连接句柄 (实现细节隐藏在 uredis.cpp).
struct uconnection;

/// 操作错误信息.
struct RedisError
{
    bool ok = true;        ///< 是否成功.
    bool timeout = false;  ///< 是否超时失败.
    int err_no = 0;        ///< 错误码 (errno / -1 服务端错误).
    std::string err_msg;   ///< 错误描述.
};

/// RESP 回复.
struct Reply : RedisError
{
    enum Type
    {
        NIL = 0,  ///< 空 bulk ($-1) 或空数组 (*-1).
        STRING,   ///< bulk string ($n\r\n...\r\n).
        STATUS,   ///< 简单字符串 (+OK) 或错误 (-ERR ...).
        INTEGER,  ///< 整数 (:42).
        ARRAY,    ///< 数组 (*n).
    };

    Type type = NIL;              ///< 回复类型.
    int64_t integer = 0;          ///< INTEGER 的值.
    std::string str;              ///< STRING / STATUS 的值.
    std::vector<Reply> elements;  ///< ARRAY 的元素.
};

// ==================== 连接 ====================

/**
 * @brief 建立连接: TCP 建连 + (可选) AUTH + (可选) SELECT db.
 * @return 连接句柄, 失败返回 nullptr.
 */
uco::task<uconnection *> uconnect(const char *host, unsigned int port,
                                  const char *pass = "", int db = 0,
                                  uco_time_t ts = {10, 0});

/// 关闭连接并释放资源.
void uclose(uconnection *c);

// ==================== 命令 ====================

/**
 * @brief 异步执行任意命令 (RESP2 参数数组形式, 天然免疫注入).
 * @return 服务端回复; 服务端错误回复 (如 -WRONGTYPE) 时 ok = false.
 */
uco::task<Reply> ucommand(uconnection *c, std::vector<std::string> args,
                          uco_time_t ts = {10, 0});

/// 异步 SET key value, 成功时 type = STATUS, str = "OK".
uco::task<Reply> uset(uconnection *c, const std::string &key,
                      const std::string &value, uco_time_t ts = {10, 0});

/// 异步 GET key, 不存在时 type = NIL, 存在时 type = STRING.
uco::task<Reply> uget(uconnection *c, const std::string &key,
                      uco_time_t ts = {10, 0});

/// 异步 DEL key..., 返回删除个数 (INTEGER).
uco::task<Reply> udel(uconnection *c, std::vector<std::string> keys,
                      uco_time_t ts = {10, 0});

/// 异步 EXISTS key..., 返回存在个数 (INTEGER).
uco::task<Reply> uexists(uconnection *c, std::vector<std::string> keys,
                         uco_time_t ts = {10, 0});

/// 异步 INCR key, 返回自增后的值 (INTEGER).
uco::task<Reply> uincr(uconnection *c, const std::string &key,
                       uco_time_t ts = {10, 0});

/// 异步 EXPIRE key seconds, 成功时 integer = 1.
uco::task<Reply> uexpire(uconnection *c, const std::string &key,
                         int64_t seconds, uco_time_t ts = {10, 0});

/// 异步 PING, 成功时 type = STATUS, str = "PONG".
uco::task<Reply> uping(uconnection *c, uco_time_t ts = {10, 0});

// ==================== 连接池 ====================

/// 连接池: 复用连接, max_size 限制并发, 后台协程周期缩容 idle.
class upool
{
  public:
    /// 配置.
    struct config
    {
        std::string host = "127.0.0.1"; ///< 主机 (点分 IPv4).
        unsigned int port = 6379;       ///< 端口.
        std::string pass;               ///< 密码 (空则不 AUTH).
        int db = 0;                     ///< 库编号 (0 则不 SELECT).
        size_t max_size = 16;           ///< 最大连接数 (并发限制).
        size_t min_idle = 1;            ///< idle 保底数, 低于等于此值不再关闭.
        uint64_t reap_interval_ms = 60000; ///< 缩容间隔 (ms), 每次关闭一个 idle.
        uco_time_t ts = {10, 0};        ///< 建连超时.
    };

    static upool &GetInstance();

    void Init(const config &cfg);

    upool(const upool &) = delete;
    upool &operator=(const upool &) = delete;
    upool(upool &&) = delete;
    upool &operator=(upool &&) = delete;

    /**
     * @brief 获取连接: 优先复用空闲, 否则新建; 达到 max_size 则等待.
     * @return 连接句柄; 池已关闭或建连失败返回 nullptr.
     */
    uco::task<uconnection *> Acquire();

    /// 归还连接; 死活自动判定: broken (传输错误/超时) 销毁,
    /// 服务端错误或无错误则回收复用.
    void Release(uconnection *c);

    /// 关闭池并释放全部连接 (含借出中的, 进程退出时调用).
    void Close();

  private:
     upool();
    ~upool();

    /// 内部状态: shared_ptr 共享所有权 (reaper 协程与进行中的 acquire 各持
    /// 一份), 保证池析构后协程仍能安全访问状态并自行退出.
    struct state
    {
        state(const config &c, size_t permits_cnt)
            : cfg(c), permits(permits_cnt)
        {
        }

        config cfg;
        std::atomic<bool> closed{false};
        std::mutex mtx;                     ///< 保护 idle/all (临界区内无 co_await).
        uco::usema permits;                 ///< 槽位信号量, 容量 = max_size.
        std::deque<uconnection *> idle;     ///< 空闲连接 (队首最老).
        std::set<uconnection *> all;        ///< 全部存活连接 (含借出中).
        uco::usleeper waker;                  ///< 可取消定时器, 供 reaper 睡眠.
    };

    /// 后台缩容协程: 每 reap_interval_ms 关闭一个 idle, 保底 min_idle.
    /// 睡在可取消定时器上, close() 立即唤醒 (无需轮询, 不拖垮调度器收尾).
    /// static: 不依赖池对象生命周期, 仅通过 st 访问状态.
    static uco::task<void> reaper(std::shared_ptr<state> st);

    std::shared_ptr<state> st_;
};

// ==================== 分布式锁 ====================

/**
 * @brief 基于 Redis 的分布式锁:
 *        加锁 SET key token NX PX ttl (原子),
 *        释放/续期 Lua 脚本校验 token (原子, 防误删他人锁),
 *        可选看门狗后台协程周期续期 (防止业务未完成锁先过期).
 *
 * @note 锁内部独占一条自建连接 (看门狗与用户命令经 umutex 串行化,
 *       避免同一连接上命令交错), 不占用连接池;
 * @note token 为持有者唯一令牌, 仅释放/续期自己持有的锁;
 * @note 命令失败 (网络断开等) 后本地标记未持有, 服务端靠 TTL 自动过期兜底;
 * @note close() 后锁不可复用 (与 upool::close 一致);
 * @note 进程崩溃后锁靠 TTL 过期自动恢复, 看门狗场景建议 ttl >= 3 * renew.
 */
class ulock
{
  public:
    /// 配置.
    struct config
    {
        std::string host = "127.0.0.1"; ///< Redis 主机 (点分 IPv4).
        unsigned int port = 6379;       ///< 端口.
        std::string pass;               ///< 密码 (空则不 AUTH).
        int db = 0;                     ///< 库编号.
        std::string key;                ///< 锁键名.
        uint64_t ttl_ms = 10'000;       ///< 锁自动过期时间 (ms).
        uint64_t retry_interval_ms = 100; ///< 抢锁失败重试间隔, 0 = 单次尝试.
        uint64_t renew_interval_ms = 0;   ///< 看门狗续期间隔, 0 = 不启用; 建议 ttl/3.
        uco_time_t ts = {10, 0};        ///< 建连/命令超时.
    };

    explicit ulock(const config &cfg);
    ~ulock();

    ulock(const ulock &) = delete;
    ulock &operator=(const ulock &) = delete;

    /**
     * @brief 非阻塞尝试获取一次.
     * @return 成功 true; 被他人持有或失败 false.
     */
    uco::task<bool> TryAcquire();

    /**
     * @brief 阻塞获取: 失败按 retry_interval_ms 重试,
     *        直到成功 / 总超时 / 停止.
     * @note retry_interval_ms = 0 时等价 try_acquire;
     * @note 连接断开会自动重建重试.
     * @param ts 总超时, {0, 0} 表示一直等 (慎用).
     */
    uco::task<bool> Acquire(uco_time_t ts = {10, 0});

    /**
     * @brief 释放锁 (Lua 原子校验 token, 只释放自己持有的).
     * @return 成功释放 true; 本就未持有 / 锁已过期 / 命令失败 false.
     */
    uco::task<bool> Release();

    /**
     * @brief 手动续期 (仅自己持有时生效), 看门狗内部同样走此逻辑.
     * @return 续期成功 true; 锁已丢失或失败 false (并标记未持有).
     */
    uco::task<bool> ReNew();

    /// 是否持有 (看门狗发现锁丢失会自动置 false).
    bool Owns() const;

    /// 停止看门狗并断开连接 (不发命令, 进程退出时调用), 之后锁不可复用.
    void Close();

  private:
    /// 内部状态: shared_ptr 共享所有权 (看门狗协程持一份),
    /// 保证锁析构后协程仍能安全访问并自行退出.
    struct state
    {
        config cfg;
        std::atomic<bool> stopped{false};
        bool watchdog_armed = false;   ///< 是否已启动看门狗协程.
        uco::umutex cmd_mtx;           ///< 命令串行化 (与看门狗互斥).
        std::mutex token_mtx;          ///< 保护 token (临界区内无 co_await).
        std::string token;             ///< 当前持有者令牌 (空 = 未持有).
        uconnection *c = nullptr;      ///< 内部独占连接.
        std::unique_ptr<uco::usleeper> waker; ///< 看门狗定时器 (启用看门狗时创建).
    };

    /// 后台看门狗: 每 renew_interval_ms 续期一次, 睡在可取消定时器上,
    /// close() 立即唤醒, 锁停止时由看门狗负责关闭连接并退出.
    static uco::task<void> watchdog(std::shared_ptr<state> st);

    /// 续期实现 (持 cmd_mtx, 供 renew() 与看门狗共用).
    static uco::task<bool> do_renew(std::shared_ptr<state> st);

    std::shared_ptr<state> st_;
};

class UredisGuard
{
public:
    UredisGuard(uconnection *conn) : conn_(conn) {}
    ~UredisGuard() { upool::GetInstance().Release(conn_); }
private:
    uconnection *conn_ = 0;
};

} // namespace uredis
