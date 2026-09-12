/**
 * @file session.cpp
 * @brief 基于 Redis 的 HTTP 会话实现, 详见 session.h 头部说明.
 */

#include "http/session.h"

#include "core/uconfig.h"
#include "core/ulog.h"
#include "core/uredis.h" // upool / UredisGuard / 命令.

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <random>
#include <utility>
#include <vector>

// ==================== flat JSON 序列化 ====================
// 会话数据以 flat JSON 存入 Redis (redis-cli 可读, 跨语言可解析).

namespace
{

static const char *kHexDigits = "0123456789abcdef";
static const char *kSessionKey = "uco:session"; ///< HttpContext 用户数据键: 当前请求的 Session.

/// 追加一个 JSON 转义后的字符串 (含引号).
static void JsonEscapeAppend(std::string &out, const std::string &s)
{
    out += '"';
    for (unsigned char ch : s)
    {
        switch (ch)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) // 其余控制字符.
            {
                out += "\\u00";
                out += kHexDigits[ch >> 4];
                out += kHexDigits[ch & 0xf];
            }
            else
            {
                out += (char)ch; // UTF-8 原样透传.
            }
            break;
        }
    }
    out += '"';
}

/// map -> {"k":"v",...}
static std::string DumpSessionJson(const std::map<std::string, std::string> &kv)
{
    std::string out = "{";
    bool first = true;
    for (const auto &[k, v] : kv)
    {
        if (!first)
        {
            out += ',';
        }
        first = false;
        JsonEscapeAppend(out, k);
        out += ':';
        JsonEscapeAppend(out, v);
    }
    out += '}';
    return out;
}

/// 递归下降解析 flat JSON object ({"k":"v",...}).
struct JsonFlatParser
{
    const std::string &s;
    size_t i = 0;

    explicit JsonFlatParser(const std::string &str) : s(str) {}

    void SkipWs()
    {
        while (i < s.size() &&
               (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        {
            i++;
        }
    }

    bool Eat(char c)
    {
        SkipWs();
        if (i < s.size() && s[i] == c)
        {
            i++;
            return true;
        }
        return false;
    }

    /// 码点 -> UTF-8 追加.
    static void Utf8Append(std::string &out, uint32_t cp)
    {
        if (cp <= 0x7F)
        {
            out += (char)cp;
        }
        else if (cp <= 0x7FF)
        {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else if (cp <= 0xFFFF)
        {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    /// 解析 \uXXXX 的 4 位十六进制.
    bool ParseHex4(uint32_t &v)
    {
        if (i + 4 > s.size())
        {
            return false;
        }
        v = 0;
        for (int k = 0; k < 4; k++)
        {
            char c = s[i + k];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= (uint32_t)(c - 'A' + 10);
            else
                return false;
        }
        i += 4;
        return true;
    }

    bool ParseString(std::string &out)
    {
        if (!Eat('"'))
        {
            return false;
        }
        out.clear();
        while (i < s.size())
        {
            char c = s[i++];
            if (c == '"')
            {
                return true; // 闭合.
            }
            if ((unsigned char)c < 0x20)
            {
                return false; // 裸控制字符非法.
            }
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (i >= s.size())
            {
                return false;
            }
            char e = s[i++];
            switch (e)
            {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
            {
                uint32_t cp = 0;
                if (!ParseHex4(cp))
                {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF &&
                    i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u')
                {
                    // 代理对: 高位 + \u 低位合并为完整码点.
                    size_t save = i;
                    i += 2;
                    uint32_t lo = 0;
                    if (ParseHex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                    {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    else
                    {
                        i = save; // 低位非法, 高位以替换符处理.
                        cp = 0xFFFD;
                    }
                }
                else if (cp >= 0xD800 && cp <= 0xDFFF)
                {
                    cp = 0xFFFD; // 孤立代理.
                }
                Utf8Append(out, cp);
                break;
            }
            default:
                return false;
            }
        }
        return false; // 未闭合.
    }

    bool Parse(std::map<std::string, std::string> &kv)
    {
        kv.clear();
        if (!Eat('{'))
        {
            return false;
        }
        SkipWs();
        if (i < s.size() && s[i] == '}')
        {
            i++;
            return true; // 空对象.
        }
        while (true)
        {
            std::string k, v;
            if (!ParseString(k) || !Eat(':') || !ParseString(v))
            {
                return false;
            }
            kv.emplace(std::move(k), std::move(v));
            if (Eat(','))
            {
                continue;
            }
            if (Eat('}'))
            {
                return true;
            }
            return false;
        }
    }
};

/// 解析 flat JSON, 要求整串恰好为一个对象 (尾部不得有垃圾).
static bool ParseSessionJson(const std::string &in,
                             std::map<std::string, std::string> &kv)
{
    JsonFlatParser p(in);
    if (!p.Parse(kv))
    {
        return false;
    }
    p.SkipWs();
    return p.i == in.size();
}

/// vector<string> -> JSON 数组字符串 (闪存编码).
static std::string DumpFlashArray(const std::vector<std::string> &v)
{
    std::string out = "[";
    bool first = true;
    for (const std::string &s : v)
    {
        if (!first)
        {
            out += ',';
        }
        first = false;
        JsonEscapeAppend(out, s);
    }
    out += ']';
    return out;
}

/// JSON 数组字符串 -> vector<string> (闪存解码), 整串校验, 失败 false.
static bool ParseFlashArray(const std::string &in, std::vector<std::string> &out)
{
    out.clear();
    if (in.empty())
    {
        return false;
    }
    JsonFlatParser p(in);
    if (!p.Eat('['))
    {
        return false;
    }
    p.SkipWs();
    if (p.Eat(']'))
    {
        return true;
    }
    while (true)
    {
        std::string s;
        if (!p.ParseString(s))
        {
            return false;
        }
        out.push_back(std::move(s));
        if (p.Eat(','))
        {
            continue;
        }
        if (p.Eat(']'))
        {
            break;
        }
        return false;
    }
    p.SkipWs();
    return p.i == in.size();
}

} // namespace

// ==================== SessionStore ====================

SessionStore::SessionStore(const uco::YamlConfig &config,
                           uredis::upool &pool)
    : m_keyPrefix(
          config.Get<std::string>("session.redis_key_prefix", "uco:sess:")),
      m_secretKey(config.Get<std::string>("session.secret_key",
                                          "uco-session-secret")),
      m_cookieName(
          config.Get<std::string>("session.cookie_name", "uco_session")),
      m_flashKey(config.Get<std::string>("session.flash_key", "_flash")),
      m_maxAge(config.Get<int>("session.max_age_sec", 86400 * 30)),
      m_maxAnonymousAge(config.Get<int>("session.anonymous_max_age_sec", 600)),
      m_cookiePath(config.Get<std::string>("session.cookie_path", "/")),
      m_cookieDomain(config.Get<std::string>("session.cookie_domain", "")),
      m_secure(config.Get<bool>("session.cookie_secure", false)),
      m_httpOnly(config.Get<bool>("session.cookie_http_only", true)),
      m_autoSave(config.Get<bool>("session.auto_save", true)),
      m_sliding(config.Get<bool>("session.sliding", false)), m_pool(pool)
{
    m_redisTimeout.tv_sec =
        config.Get<time_t>("session.redis_timeout_sec", 10);
    if (m_keyPrefix.empty())
    {
        m_keyPrefix = "uco:sess:";
    }
    if (m_maxAge <= 0)
    {
        m_maxAge = 86400 * 30;
    }
    if (m_secretKey.empty())
    {
        // 未配置签名密钥: 生成进程级临时密钥兜底.
        // 副作用: 重启后旧会话全部验签失败 (相当于强制全员下线),
        // 生产环境务必显式配置固定密钥.
        unsigned char buf[32];
        if (RAND_bytes(buf, sizeof(buf)) == 1)
        {
            for (unsigned char b : buf)
            {
                m_secretKey += kHexDigits[b >> 4];
                m_secretKey += kHexDigits[b & 0xf];
            }
        }
        else
        {
            m_secretKey = "uco-session-fallback-secret";
        }
        LOGWRN("session: secret_key empty, ephemeral key generated"
               " (all sessions invalidated on restart)");
    }
}

SessionStore::~SessionStore() = default; // 请求全部结束后才析构, 注册表应已清空.

// ---- Redis I/O ----

uco::task<SessionStore::LoadResult> SessionStore::LoadValues(const std::string &id,
                                                             std::string &out)
{
    uredis::uconnection *conn =
        co_await m_pool.Acquire();
    if (conn == nullptr)
    {
        LOGERR("session: redis acquire failed");
        co_return LoadResult::kError;
    }
    uredis::UredisGuard guard(m_pool, conn); // RAII 归还连接.

    uredis::Reply r =
        co_await uredis::uget(conn, m_keyPrefix + id, m_redisTimeout);
    if (r.ret_code != 0)
    {
        LOGERR("session: redis get failed:", r.err_msg);
        co_return LoadResult::kError;
    }
    if (r.type == uredis::Reply::NIL)
    {
        co_return LoadResult::kMissing; // 已过期/不存在 (正常).
    }
    out = std::move(r.str);
    co_return LoadResult::kOK;
}

uco::task<bool> SessionStore::SaveValues(const std::string &id,
                                         const std::string &json, int ttl)
{
    if (ttl <= 0)
    {
        // 会话语义 (MaxAge=0) 的 Redis 侧回退默认值, 同 gin 的 DefaultMaxAge.
        ttl = m_maxAge;
    }

    uredis::uconnection *conn =
        co_await m_pool.Acquire();
    if (conn == nullptr)
    {
        LOGERR("session: redis acquire failed");
        co_return false;
    }
    uredis::UredisGuard guard(m_pool, conn);

    // SET key json EX ttl: 数据与 TTL 整体重写 (改过期时间即改完重存).
    std::vector<std::string> args;
    args.reserve(5);
    args.emplace_back("SET");
    args.emplace_back(m_keyPrefix + id);
    args.emplace_back(json);
    args.emplace_back("EX");
    args.emplace_back(std::to_string(ttl));

    uredis::Reply r =
        co_await uredis::ucommand(conn, std::move(args), m_redisTimeout);
    if (r.ret_code != 0 || r.type != uredis::Reply::STATUS || r.str != "OK")
    {
        LOGERR("session: redis set failed:", r.err_msg);
        co_return false;
    }
    co_return true;
}

uco::task<bool> SessionStore::RemoveKey(const std::string &id)
{
    uredis::uconnection *conn =
        co_await m_pool.Acquire();
    if (conn == nullptr)
    {
        LOGERR("session: redis acquire failed");
        co_return false;
    }
    uredis::UredisGuard guard(m_pool, conn);

    std::vector<std::string> keys;
    keys.emplace_back(m_keyPrefix + id);
    uredis::Reply r = co_await uredis::udel(conn, std::move(keys), m_redisTimeout);
    if (r.ret_code != 0)
    {
        LOGERR("session: redis del failed:", r.err_msg);
        co_return false;
    }
    co_return true;
}

uco::task<bool> SessionStore::TouchTTL(const std::string &id, int ttl)
{
    if (ttl <= 0)
    {
        ttl = m_maxAge;
    }

    uredis::uconnection *conn =
        co_await m_pool.Acquire();
    if (conn == nullptr)
    {
        LOGERR("session: redis acquire failed");
        co_return false;
    }
    uredis::UredisGuard guard(m_pool, conn);

    uredis::Reply r =
        co_await uredis::uexpire(conn, m_keyPrefix + id, ttl, m_redisTimeout);
    if (r.ret_code != 0)
    {
        LOGERR("session: redis expire failed:", r.err_msg);
        co_return false;
    }
    co_return true;
}

// ---- cookie 签名 ----

std::string SessionStore::NewSessionID()
{
    unsigned char buf[16]; // 128 bit -> 32 字符 hex.
    if (RAND_bytes(buf, sizeof(buf)) != 1)
    {
        // 密码学随机失败 (几乎不可达): random_device 退化.
        LOGWRN("session: RAND_bytes failed, fallback to random_device");
        std::random_device rd;
        for (unsigned char &b : buf)
        {
            b = (unsigned char)rd();
        }
    }

    std::string id;
    id.reserve(32);
    for (unsigned char b : buf)
    {
        id += kHexDigits[b >> 4];
        id += kHexDigits[b & 0xf];
    }
    return id;
}

std::string SessionStore::SignID(const std::string &id)
{
    unsigned char mac[EVP_MAX_MD_SIZE] = {0};
    unsigned int maclen = 0;
    HMAC(EVP_sha256(), m_secretKey.data(), (int)m_secretKey.size(),
         (const unsigned char *)id.data(), id.size(), mac, &maclen);

    std::string sig;
    sig.reserve((size_t)maclen * 2);
    for (unsigned int i = 0; i < maclen; i++)
    {
        sig += kHexDigits[mac[i] >> 4];
        sig += kHexDigits[mac[i] & 0xf];
    }
    return sig;
}

bool SessionStore::ParseSignedCookie(const std::string &cookie, std::string &id)
{
    const size_t dot = cookie.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= cookie.size())
    {
        return false;
    }
    id = cookie.substr(0, dot);
    const std::string got = cookie.substr(dot + 1);

    // 防御性: ID 只允许 hex (ID 由本框架生成).
    for (char c : id)
    {
        if (std::isxdigit((unsigned char)c) == 0)
        {
            return false;
        }
    }

    const std::string expect = SignID(id);
    if (got.size() != expect.size())
    {
        return false;
    }
    // 恒时比较, 防时序侧信道:
    // memcmp/== 在首个不匹配字节即返回, 耗时取决于匹配前缀长度,
    // 攻击者可反复伪造签名并测量响应耗时, 逐字节逼近正确签名
    // (timing attack); CRYPTO_memcmp (OpenSSL) 无论差异在哪都扫完
    // 全部字节, 耗时与内容无关. 对cookie/HMAC 等密文比较是标准做法.
    return CRYPTO_memcmp(got.data(), expect.data(), got.size()) == 0;
}

// ==================== Session ====================

Session *Session::FromContext(HttpContext *ctx)
{
    if (ctx == nullptr)
    { // handler 链内的 ctx 恒非空, 传空属调用方编程错误:
        // fail-fast (FTL 终止进程), 不以 500 容错掩盖 bug.
        LOGFTL("session: FromContext called with null ctx");
    }
    Session *s = static_cast<Session *>(ctx->Load(kSessionKey));
    if (s == nullptr)
    { // handler 能执行到这里说明链存在, 但 session 从未注册
        // → 中间件不在链上 (路由忘挂 Sessions), 部署错误:
        // 显式失败并留 ERR 日志, 勿静默走"未登录"分支 (≈ gin 的 MustGet).
        LOGERR("session: middleware missing, "
               "route should mount Sessions");
        ctx->Status(500);
        throw HttpException("session middleware missing");
    }
    return s;
}

Session::Session(SessionStore *store, const std::string &name,
                 HttpContext *ctx)
    : m_ptrStore(store), m_sName(name), m_ptrCtx(ctx)
{
}

// ---- 内存操作 ----

std::string Session::Get(const std::string &key, const std::string &def) const
{
    auto it = m_mapValues.find(key);
    return it == m_mapValues.end() ? def : it->second;
}

int64_t Session::GetInt64(const std::string &key, int64_t def) const
{
    auto it = m_mapValues.find(key);
    if (it == m_mapValues.end() || it->second.empty())
    {
        return def;
    }
    char *end = nullptr;
    errno = 0;
    long long v = std::strtoll(it->second.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0')
    {
        return def;
    }
    return (int64_t)v;
}

bool Session::GetBool(const std::string &key, bool def) const
{
    auto it = m_mapValues.find(key);
    if (it == m_mapValues.end())
    {
        return def;
    }
    const std::string &v = it->second;
    if (v == "1" || v == "true" || v == "yes" || v == "on")
    {
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off" || v.empty())
    {
        return false;
    }
    return def;
}

bool Session::Has(const std::string &key) const
{
    return m_mapValues.find(key) != m_mapValues.end();
}

void Session::Set(const std::string &key, const std::string &value)
{
    m_mapValues[key] = value;
    m_bWritten = true;
}

void Session::SetInt64(const std::string &key, int64_t value)
{
    Set(key, std::to_string(value));
}

void Session::SetBool(const std::string &key, bool value)
{
    Set(key, value ? "1" : "0");
}

void Session::Delete(const std::string &key)
{
    if (m_mapValues.erase(key) > 0)
    {
        m_bWritten = true;
    }
}

void Session::Clear()
{
    if (!m_mapValues.empty())
    {
        m_mapValues.clear();
        m_bWritten = true;
    }
}

void Session::AddFlash(const std::string &value, const std::string &inkey)
{
    const std::string& key = inkey.empty() ? m_ptrStore->m_flashKey : inkey;
    std::vector<std::string> items;
    ParseFlashArray(Get(key), items); // 旧值缺失/损坏均视作空 (损坏自愈).
    items.push_back(value);
    Set(key, DumpFlashArray(items));
}

std::vector<std::string> Session::Flashes(const std::string &inkey)
{
    const std::string& key = inkey.empty() ? m_ptrStore->m_flashKey : inkey;
    std::vector<std::string> items;
    if (!ParseFlashArray(Get(key), items))
    {
        return {}; // 无闪存或数据损坏: 均当空, 不置脏.
    }
    Delete(key); // 读即焚; 清除随 Save() (或 auto_save) 落盘.
    return items;
}

void Session::SetMaxAge(int seconds)
{
    // 负值 = 立即删除: 入口处归一化为销毁标记.
    // 注: m_iMaxAgeOverride 的 -1 保留专职作"未覆盖"哨兵,
    //     负值不能存进去 (否则 EffectiveMaxAge() 取不出来,
    //     语义会退化为"恢复默认", 而不是删除).
    if (seconds < 0)
    {
        m_bDestroy = true;
        m_bWritten = true;
        return;
    }
    m_iMaxAgeOverride = seconds;
    m_bWritten = true; // 与 gin 不同: 纯改 TTL 也触发落盘.
}

void Session::Destroy()
{
    m_bDestroy = true;
    m_bWritten = true;
}

// ---- 加载与保存 ----

uco::task<void> Session::InitLoad()
{
    SessionStore &store = *m_ptrStore;

    // 1. cookie 解析 + 验签 (无 cookie / 验签失败 -> 全新会话).
    const std::string cookie = m_ptrCtx->GetCookie(m_sName);
    if (cookie.empty())
    {
        co_return;
    }
    std::string id;
    if (!store.ParseSignedCookie(cookie, id))
    {
        LOGWRN("session: cookie signature verify failed, treat as new");
        co_return;
    }
    m_sID = std::move(id);

    // 2. Redis 加载.
    std::string raw;
    switch (co_await store.LoadValues(m_sID, raw))
    {
    case SessionStore::LoadResult::kOK:
        if (ParseSessionJson(raw, m_mapValues))
        {
            m_bNew = false;
            m_bLoadOK = true;
        }
        else
        {
            // 数据损坏: 当作过期处理, 后续保存可覆盖自愈.
            LOGWRN("session: corrupted data, treat as new, id:", m_sID);
        }
        break;
    case SessionStore::LoadResult::kMissing:
        break; // 已过期/不存在: 全新会话 (正常路径).
    case SessionStore::LoadResult::kError:
        // Redis 故障: 降级为未登录; Save 时拒绝覆盖 (见 Save 内检查).
        LOGWRN("session: redis error, degraded to new, id:", m_sID);
        break;
    }

    // 3. 滑动续期: 命中即 EXPIRE (只刷 TTL, 不重写数据).
    if (store.m_sliding && !m_bNew)
    {
        if (!co_await store.TouchTTL(m_sID, EffectiveMaxAge()))
        {
            LOGWRN("session: sliding renew failed, id:", m_sID);
        }
    }
}

int Session::EffectiveMaxAge() const
{
    return m_iMaxAgeOverride >= 0 ? m_iMaxAgeOverride : m_ptrStore->m_maxAge;
}

int Session::MaxAnonymousAge() const
{
    return m_ptrStore->m_maxAge;
}


uco::task<bool> Session::Save()
{
    // 0. 脏检查: 只读请求空操作 (同 gin 的 written 短路优化).
    if (!m_bWritten)
    {
        co_return true;
    }

    SessionStore &store = *m_ptrStore;

    // 1. 老会话但先前加载失败 (Redis 故障): 拒绝覆盖,
    //    防止抖动期间误清/误覆盖用户会话.
    if (!m_bNew && !m_bLoadOK)
    {
        LOGWRN("session: save skipped (load failed earlier), id:", m_sID);
        co_return false;
    }

    m_bWritten = false;

    // 2. 销毁 -> 删除分支.
    //    销毁来源: Destroy() 或 SetMaxAge(负值) (后者已在入口归一化,
    //    EffectiveMaxAge() 契约为恒 >= 0, 此处无需再查).
    if (m_bDestroy || m_mapValues.empty())
    {
        // 删除分支 (登出): DEL Redis + cookie 立即过期.
        if (!m_sID.empty())
        {
            if (!co_await store.RemoveKey(m_sID))
            {
                co_return false;
            }
        }
        ExpireCookie();
        m_sID.clear();
        m_bNew = true;
        m_bLoadOK = false;
        m_bDestroy = false;
        co_return true;
    }

    // 3. 新会话生成 ID.
    if (m_sID.empty())
    {
        m_sID = store.NewSessionID();
    }

    // 4. 写 Redis (数据 + TTL 整体重写).
    const int age = EffectiveMaxAge();
    if (!co_await store.SaveValues(m_sID, DumpSessionJson(m_mapValues), age))
    {
        co_return false;
    }

    // 5. 下发 cookie (age = 0 为会话 cookie: 无 Max-Age, 浏览器关闭失效).
    WriteCookie(age);
    co_return true;
}

uco::task<bool> Session::Rotate()
{
    // 1. 记旧 ID 与状态 (空 ID = 全新会话: 无旧 key 可删, 退化为直建;
    //    状态快照用于失败回滚 — 回滚后会话与轮换前完全一致,
    //    调用方后续 Destroy()/重试均正确作用于旧会话).
    const std::string old_id = m_sID;
    const bool was_new = m_bNew;
    const bool was_written = m_bWritten;
    const int old_max_age = m_iMaxAgeOverride;

    // 2. 服务端生成新 ID (不接受外部传值, 防碰撞/可预测).
    //    m_bNew 置位: 轮换写的是全新 key, 不触发 "加载失败拒绝覆盖"
    //    检查 (该检查防的是覆盖老会话数据, 与写新 key 无关).
    //    TTL 覆盖重置: 轮换 = 新会话, 策略从头来 — 防同请求内
    //    SetMaxAge(如匿名签发的短 TTL) 泄漏进升级后的会话
    //    (Redis TTL 与 cookie Max-Age 双端同染).
    m_sID = m_ptrStore->NewSessionID();
    m_bNew = true;
    m_bWritten = true; // 置脏, 保证 Save 落盘
    m_iMaxAgeOverride = -1;

    // 3. 落盘: 写新 key + 下发新 cookie (签名现算, 自动跟随新 ID).
    //    登录态数据已由调用方 Set 进 m_mapValues, 只落新 key —
    //    登录态永不落旧 key, 此为防 session fixation 的核心不变量.
    if (!co_await Save())
    {
        // 写失败: 新 key 未落盘, 旧 key 未动; 回滚内存状态
        // (注意 m_sID 必须还原 — 否则调用方随后的 Destroy() 会
        // DEL 从未存在的新 ID, 真正的旧会话 key 反而存活到 TTL).
        m_sID = old_id;
        m_bNew = was_new;
        m_bWritten = was_written;
        m_iMaxAgeOverride = old_max_age;
        co_return false;
    }

    // 4. 删旧 key (写成功后才删). 与 3 非原子, 但顺序保证失败模式良性:
    //    残留旧 key 是纯匿名数据, TTL 兜底过期, 无法升级为已登录.
    if (!old_id.empty() && !co_await m_ptrStore->RemoveKey(old_id))
    {
        LOGWRN("session: rotate del old key failed, id:", old_id);
    }
    co_return true;
}

// ---- cookie 下发 ----

void Session::WriteCookie(int max_age)
{
    SessionStore &store = *m_ptrStore;

    // cookie 值 = "id.sig": 只存 ID 与签名, 数据本体在 Redis.
    // SameSite=Lax 为 CSRF 缓解默认 (gin 同款机制: SetSameSite 作用于 SetCookie).
    m_ptrCtx->SetSameSite(HttpContext::eSameSiteLax);
    m_ptrCtx->SetCookie(m_sName, m_sID + "." + store.SignID(m_sID),
                        max_age, store.m_cookiePath, store.m_cookieDomain,
                        store.m_secure, store.m_httpOnly);
}

void Session::ExpireCookie()
{
    SessionStore &store = *m_ptrStore;

    // 删除 cookie (gin 同款写法: 空值 + 负 MaxAge -> "Max-Age=0").
    m_ptrCtx->SetSameSite(HttpContext::eSameSiteLax);
    m_ptrCtx->SetCookie(m_sName, "", -1, store.m_cookiePath,
                        store.m_cookieDomain, store.m_secure,
                        store.m_httpOnly);
}

// ==================== 中间件 ====================

uco::task<void> SessionStore::Sessions(HttpContext *ctx)
{
    // 1. 创建并加载会话 (cookie 验签 + Redis 读取).
    //    与 gin 的惰性加载不同: 请求进入即加载, 换取 Get() 同步语义.
    // make_unique 无法访问私有构造 (它不是友元), 友元内直接 new.
    std::unique_ptr<Session> session(new Session(this, m_cookieName, ctx));
    co_await session->InitLoad();

    // 2. 挂到请求上下文 (≈ gin 的 c.Set(DefaultKey, s)),
    //    handler 经 Session::FromContext(ctx) 取同一指针;
    //    守卫析构自动摘除 (异常路径栈展开亦然), 防 Session 悬垂.
    auto guard = ctx->Store(kSessionKey, session.get());

    co_await ctx->Next();

    // 3. 链正常结束: 补存忘保存的脏数据 (C++ 场景默认兜底,
    //    gin 要求手动 Save; 可经 session.auto_save 关闭以对齐 gin).
    if (m_autoSave && session->m_bWritten)
    {
        if (!co_await session->Save())
        {
            LOGWRN("session: auto save failed");
        }
    }

    // 4. guard 析构摘除; unique_ptr 析构, Session 指针自此失效.
    co_return;
}
