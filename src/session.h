#pragma once

/**
 * @file session.h
 * @brief 基于 Redis 的 HTTP 会话 (cookie + session) 支持.
 *
 * 整体结构对齐 gin-contrib/sessions 的三件套:
 *
 *   SessionStore       ≈ store:
 *     全局单例, 持有配置, 负责与 Redis 的读写
 *     (连接经 uredis::upool 获取, UredisGuard RAII 归还),
 *     以及 cookie 的 HMAC-SHA256 签名/验签;
 *
 *   Session            ≈ session:
 *     每请求一个; Set/Get/Delete/Clear 仅操作内存并置脏标记,
 *     Save() 才落盘 (写 Redis + 下发/删除 Set-Cookie);
 *
 *   SessionMiddleware  ≈ sessions.Sessions(name, store):
 *     挂在路由链首; 加载会话并注册到请求上下文,
 *     链结束 (含 Abort 异常路径) 清理注册表.
 *
 * 典型用法:
 * @code
 *   // main 中, 先于 httpserver.Run() (Redis 池由 Run 初始化):
 *   SessionStore::Config cfg;
 *   cfg.secret_key = "..."; // 生产环境必配
 *   SessionStore::GetInstance().Init(cfg);
 *
 *   // 路由 (中间件挂链首, 同 gin 的 r.Use(sessions.Sessions(...))):
 *   httpserver.GET("/api/me", SessionMiddleware, show_me);
 *
 *   // handler 内:
 *   Session *s = Session::FromContext(ctx); // ≈ sessions.Default(c)
 *   if (s->Get("user_id").empty()) { ctx->Abort(401, "unauthorized"); co_return; }
 *   s->Set("last_seen", "2026-09-08");
 *   co_await s->Save(); // 或交给链尾 auto_save 兜底
 * @endcode
 *
 * @note cookie 只存 "会话ID.HMAC签名", 数据本体在 Redis;
 * @note Redis 中 key = <key_prefix><ID>, value 为 flat JSON (redis-cli 可读),
 *       TTL 与 cookie Max-Age 同源, 双端同时过期;
 * @note 只读请求 Save() 为空操作 (脏标记短路, 同 gin 的 written 优化);
 * @note Clear() + Save() 即登出: 数据为空走删除分支
 *       (DEL Redis key + 浏览器 cookie 立即过期);
 * @note SetMaxAge() + Save() 修改过期时间 (重写 Redis TTL 与 cookie);
 * @note 与 gin 的两点差异:
 *       1) 加载为 eager (请求进入即读 Redis), 换取 Get() 的同步语义;
 *       2) SetMaxAge() 也置脏, 纯改 TTL 无需再 Set 一下;
 * @note Redis 故障 (区别于 key 过期) 时老会话拒绝覆盖式保存,
 *       防止 Redis 抖动把用户会话误清;
 * @note cookie 经 HttpContext::SetCookie 下发 (gin 对齐实现,
 *       配合 SetSameSite 输出 SameSite=Lax);
 * @note Session 指针仅在当前请求链内有效, 勿跨请求持有.
 */

#include "httpserver.h"
#include "uio.h" // uco_time_t / uco::task (Redis 客户端在 session.cpp 引入).

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

class Session;

/// 会话中间件 (≈ sessions.Sessions(name, store)), 挂在路由链首.
uco::task<void> SessionMiddleware(HttpContext *ctx);

/// 会话存储 (≈ gin 的 store): 全局单例.
class SessionStore
{
  public:
    /// Redis 读取结果.
    enum class LoadResult
    {
        kOK,      ///< 命中并取回数据.
        kMissing, ///< key 不存在 (会话已过期, 正常路径).
        kError,   ///< 命令失败 (连接/超时等故障).
    };

    /// 配置.
    struct Config
    {
        std::string cookie_name = "uco_session"; ///< cookie 名.
        std::string key_prefix = "uco:sess:";    ///< Redis key 前缀.
        std::string secret_key;                  ///< HMAC 签名密钥 (生产必配, 空则自动生成临时密钥).
        int max_age = 86400 * 30;                ///< 默认过期秒数 (Redis TTL + cookie Max-Age).
        std::string path = "/";                  ///< cookie Path.
        std::string domain;                      ///< cookie Domain (空则不下发).
        bool secure = false;                     ///< cookie Secure (仅 HTTPS).
        bool http_only = true;                   ///< cookie HttpOnly (JS 不可读).
        bool auto_save = true;                   ///< 链结束时自动补存脏数据 (gin 无此兜底).
        bool sliding = false;                    ///< 命中即续期 (每次加载成功后 EXPIRE).
        uco_time_t ts = {10, 0};                 ///< Redis 命令超时.
    };

    static SessionStore &GetInstance();

    /// 初始化配置 (main 中先于 httpserver.Run() 调用一次).
    void Init(const Config &cfg);

    SessionStore(const SessionStore &) = delete;
    SessionStore &operator=(const SessionStore &) = delete;
    SessionStore(SessionStore &&) = delete;
    SessionStore &operator=(SessionStore &&) = delete;

    const Config &Cfg() const { return m_cfg; }

  private:
    friend class Session;
    friend uco::task<void> SessionMiddleware(HttpContext *ctx);

    SessionStore() = default;
    ~SessionStore() = default;

    // ---- Redis I/O (连接经 upool 获取, UredisGuard 归还) ----

    /// GET 会话数据.
    uco::task<LoadResult> LoadValues(const std::string &id, std::string &out);

    /// SET key json EX ttl, ttl <= 0 时回退默认 max_age.
    uco::task<bool> SaveValues(const std::string &id, const std::string &json,
                               int ttl);

    /// DEL 会话 key.
    uco::task<bool> RemoveKey(const std::string &id);

    /// EXPIRE (滑动续期用).
    uco::task<bool> TouchTTL(const std::string &id, int ttl);

    // ---- cookie 签名 ----

    /// 生成 32 字符 hex 的随机会话 ID.
    std::string NewSessionID();

    /// HMAC-SHA256(secret, id) 的 hex 签名.
    std::string SignID(const std::string &id);

    /// 拆解 "id.sig" 并恒时验签, 成功写出 id.
    bool ParseSignedCookie(const std::string &cookie, std::string &id);

    // ---- 请求注册表 (≈ gin 的 c.Set(DefaultKey, s)) ----

    void Register(HttpContext *ctx, Session *s);
    void Unregister(HttpContext *ctx, Session *s); // 仅当注册者匹配时删除.
    Session *Find(HttpContext *ctx);

    Config m_cfg;
    std::mutex m_mtxRegistry; ///< 保护注册表 (临界区内无 co_await).
    std::unordered_map<HttpContext *, Session *> m_mapRegistry;
};

/// 每请求一个的会话对象 (≈ sessions.Default(c) 返回的对象).
class Session
{
  public:
    /// 取当前请求的会话; 路由未挂 SessionMiddleware 时返回 nullptr.
    static Session *FromContext(HttpContext *ctx);

    // ---- 内存操作 (不落盘, 变更置脏, 需 Save 生效) ----

    std::string Get(const std::string &key, const std::string &def = "") const;
    int64_t GetInt64(const std::string &key, int64_t def = 0) const;
    bool GetBool(const std::string &key, bool def = false) const;
    bool Has(const std::string &key) const;

    void Set(const std::string &key, const std::string &value);
    void SetInt64(const std::string &key, int64_t value);
    void SetBool(const std::string &key, bool value);
    void Delete(const std::string &key);
    void Clear(); ///< 清空数据; 随后 Save() 走删除分支 (登出).

    // ---- 属性 ----

    bool IsNew() const { return m_bNew; }
    const std::string &ID() const { return m_sID; }
    bool Dirty() const { return m_bWritten; }

    /// 修改过期时间(秒), 需再 Save() 生效:
    ///   > 0: Redis TTL 与 cookie Max-Age;
    ///   = 0: 会话 cookie (浏览器关闭失效), Redis TTL 回退默认;
    ///   < 0: 立即删除 (等价 Destroy, 入口处即归一化为销毁标记).
    void SetMaxAge(int seconds);

    /// 标记销毁: Save() 时 DEL Redis + 过期 cookie.
    void Destroy();

    // ---- 协程操作 ----

    /// 持久化: 脏检查 + (必要时) 写 Redis + 下发/删除 cookie.
    /// @return 成功 true; 未变更时也是 true (空操作).
    uco::task<bool> Save();

  private:
    friend uco::task<void> SessionMiddleware(HttpContext *ctx);
    friend class SessionStore;

    explicit Session(HttpContext *ctx);

    /// 中间件调用: cookie 验签 + Redis 加载.
    uco::task<void> InitLoad();

    /// 解析生效的过期秒数 (-1 = 未覆盖, 取 store 默认).
    int EffectiveMaxAge() const;

    void WriteCookie(int max_age); ///< Set-Cookie (正 age) / 会话 cookie (0).
    void ExpireCookie();           ///< Set-Cookie 立即过期 (Max-Age=0 + Expires).

    HttpContext *m_ptrCtx = 0; ///< 所属请求 (链内有效).
    std::string m_sID;         ///< 会话 ID (空 = 全新未落盘).
    std::map<std::string, std::string> m_mapValues; ///< 会话数据 (有序, 序列化稳定).
    int m_iMaxAgeOverride = -1; ///< 过期覆盖值, -1 = 用 store 默认.
    bool m_bNew = true;         ///< 全新会话 (无 cookie / 验签失败 / 已过期).
    bool m_bWritten = false;    ///< 脏标记: Save() 短路与补存依据.
    bool m_bLoadOK = false;     ///< 老会话数据加载成功 (故障时拒绝覆盖).
    bool m_bDestroy = false;    ///< 显式销毁标记.
};
