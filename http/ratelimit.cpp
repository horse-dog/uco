/**
 * @file ratelimit.cpp
 * @brief 限流中间件实现, 见 ratelimit.h.
 */

#include "http/ratelimit.h"

#include "http/session.h"
#include "core/ulog.h"
#include "core/ustring.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ratelimit
{

namespace
{

/// 计数表条目数上限, 超过则触发懒清理 (见 FixedWindowLimiter::Allow).
constexpr size_t kMaxKeys = 65536;

/**
 * @brief 固定窗口计数器 (每挂载点一个, 工厂闭包经 shared_ptr 持有).
 *
 * 并发: HttpServer 多 worker 线程共享进程, 计数经 std::mutex 保护;
 * 临界区为纯内存操作 (无 co_await/IO), 持锁纳秒级, 不会实质阻塞
 * 事件循环 (umutex 为协程锁, 让出语义对纯内存短临界区无必要).
 */
class FixedWindowLimiter
{
  public:
    FixedWindowLimiter(int limit, int window_sec)
        : m_iLimit(limit),
          m_iWindowMs(static_cast<uint64_t>(window_sec) * 1000)
    {
        if (limit <= 0)
        {
            throw std::invalid_argument("ratelimit: limit must be positive");
        }
        if (window_sec <= 0)
        {
            throw std::invalid_argument(
                "ratelimit: window_sec must be positive");
        }
    }

    /**
     * @brief 消费一个配额.
     * @param key 计数 key (IP / session ID / 账号名).
     * @return {是否放行, 窗口剩余秒数 (拒绝时的 Retry-After, >= 1)}.
     */
    std::pair<bool, int> Allow(const std::string &key)
    {
        // steady_clock: 不受系统时钟跳变影响; 窗口全局对齐
        // (按毫秒整除窗口宽划分), 与首个请求开窗相比, Retry-After
        // 计算确定且各 key 窗口边界一致.
        const uint64_t now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        const uint64_t cur_window = now_ms / m_iWindowMs;

        std::lock_guard<std::mutex> guard(m_mutex);

        // 懒清理: 条目数超阈值时清除所有已结束窗口的条目. 该 key
        // 重新请求会建新条目从 0 计数, 与留在表内跨窗重置等价,
        // 删除无语义损失. 直连部署下 peer IP 不可伪造、session
        // ID/账号名受服务端约束, key 数受真实主体数约束, 阈值只
        // 兜异常场景.
        if (m_mapEntries.size() > kMaxKeys)
        {
            for (auto it = m_mapEntries.begin(); it != m_mapEntries.end();)
            {
                it = (it->second.window < cur_window) ? m_mapEntries.erase(it)
                                                      : ++it;
            }
        }

        Entry &e = m_mapEntries[key];
        if (e.window != cur_window)
        { // 新窗口 (首次或跨窗): 惰性过期, 计数重置.
            e.window = cur_window;
            e.count = 0;
        }
        if (e.count >= m_iLimit)
        { // 拒绝: 到下一窗口边界的剩余秒数, 向上取整且 >= 1.
            const uint64_t remain_ms = (cur_window + 1) * m_iWindowMs - now_ms;
            int retry_sec = static_cast<int>((remain_ms + 999) / 1000);
            return {false, retry_sec < 1 ? 1 : retry_sec};
        }
        ++e.count;
        return {true, 0};
    }

  private:
    struct Entry
    {
        uint64_t window = 0; ///< 窗口序号 (steady 毫秒 / 窗口宽).
        uint64_t count = 0;  ///< 窗口内已放行请求数.
    };

    int m_iLimit;
    uint64_t m_iWindowMs;
    std::mutex m_mutex;
    std::unordered_map<std::string, Entry> m_mapEntries;
};

/// 从 urlencoded 表单体提取并解码字段值; 不消费 body,
/// 见 HttpContext::PeekRawData.
std::string ExtractFormField(std::string_view body, std::string_view name)
{
    // 字段名比较按完整段进行 (起始或 '&' 后到 '=' 前), 防止
    // "xusername=" 之类前缀误命中. 名称和值均按表单规则解码,
    // 保证限流 key 与后续 BindForm 看到的账号一致.
    for (size_t pos = 0; pos < body.size();)
    {
        const size_t next = body.find('&', pos);
        const size_t end =
            (next == std::string_view::npos) ? body.size() : next;
        const std::string_view item = body.substr(pos, end - pos);
        const size_t equal = item.find('=');
        if (equal != std::string_view::npos &&
            uco::UrlDecode(std::string(item.substr(0, equal)), true) == name)
        {
            return uco::UrlDecode(std::string(item.substr(equal + 1)), true);
        }
        pos = end + 1;
    }
    return "";
}

/// 按维度提取计数 key; 空 key 表示无法提取 (不计入, 交由链上
/// 后续校验拒绝).
std::string ExtractKey(KeyBy by, HttpContext *ctx)
{
    switch (by)
    {
    case KeyBy::IP:
        return ctx->ClientIP();
    case KeyBy::SessionID:
        return Session::FromContext(ctx)->ID();
    case KeyBy::Account:
        return ExtractFormField(ctx->PeekRawData(), "username");
    case KeyBy::NewSessionIP:
        // 仅新建会话计数; 老访客返回空 key, 复用空 key 跳过约定.
        if (Session::FromContext(ctx)->IsNew())
        {
            return ctx->ClientIP();
        }
        return "";
    }
    return "";
}

/// 中间件实现 (工厂经闭包捕获 limiter 转发, 同 Sessions/csrf 模式).
uco::task<void> Impl(const std::shared_ptr<FixedWindowLimiter> &limiter,
                     KeyBy by, HttpContext *ctx)
{
    const std::string key = ExtractKey(by, ctx);
    if (!key.empty())
    {
        auto [ok, retry_sec] = limiter->Allow(key);
        if (!ok)
        {
            LOGWRN("ratelimit: blocked, url:", ctx->GetRequestUrl(),
                   ", key:", key, ", retry_after:", retry_sec);
            ctx->SetHeader("Retry-After", std::to_string(retry_sec));
            ctx->Data(429, "application/json",
                      "{\"code\":429,\"msg\":\"too many requests\"}");
            ctx->Abort();
        }
    }
    co_await ctx->Next();
}

} // namespace

HttpServer::HandleFunc FixedWindow(int limit, int window_sec, KeyBy key)
{
    auto limiter = std::make_shared<FixedWindowLimiter>(limit, window_sec);
    return [limiter, key](HttpContext *ctx) -> uco::task<void> {
        return Impl(limiter, key, ctx);
    };
}

} // namespace ratelimit
