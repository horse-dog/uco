#include "http/ratelimit.h"

#include "core/ulog.h"
#include "http/session.h"

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace ratelimit
{
namespace
{

/// 计数表条目数上限，达到上限时触发懒清理。
constexpr size_t kMaxKeys = 65536;

} // namespace

Counter::Counter(int limit, int window_sec)
    : m_limit(limit),
      m_windowMs(static_cast<uint64_t>(window_sec) * 1000)
{
    if (limit <= 0)
    {
        throw std::invalid_argument("ratelimit: limit must be positive");
    }
    if (window_sec <= 0)
    {
        throw std::invalid_argument("ratelimit: window_sec must be positive");
    }
}

int Counter::RetryAfter() const
{
    const uint64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const uint64_t current_window = now_ms / m_windowMs;
    const uint64_t remain_ms =
        (current_window + 1) * m_windowMs - now_ms;
    const int retry_sec = static_cast<int>((remain_ms + 999) / 1000);
    return retry_sec < 1 ? 1 : retry_sec;
}

bool Counter::Allow(const std::string &key)
{
    const uint64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const uint64_t current_window = now_ms / m_windowMs;

    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_entries.find(key) == m_entries.end() &&
        m_entries.size() >= kMaxKeys)
    {
        for (auto it = m_entries.begin(); it != m_entries.end();)
        {
            it = it->second.window < current_window ? m_entries.erase(it)
                                                    : ++it;
        }
        if (m_entries.size() >= kMaxKeys)
        {
            LOGWRN("entries full");
            return false;
        }
    }

    Entry &entry = m_entries[key];
    if (entry.window != current_window)
    {
        entry.window = current_window;
        entry.count = 0;
    }
    if (entry.count >= static_cast<uint64_t>(m_limit))
    {
        return false;
    }
    ++entry.count;
    return true;
}

namespace
{

struct FixedWindowDetail
{

    static uco::task<bool> DefaultRejectHandler(HttpContext *ctx,
                                                Counter &counter,
                                                const std::string &key)
    {
        if (key.empty())
        {
            co_return false;
        }
        if (counter.Allow(key))
        {
            co_return false;
        }
        LOGWRN("ratelimit: blocked, route:", ctx->GetRoutePattern(),
               ", retry_after:", counter.RetryAfter());
        ctx->Data(429, "application/json",
                  "{\"code\":429,\"msg\":\"too many requests\"}");
        co_return true;
    }

    static void SetRetryAfter(Counter &counter, HttpContext *ctx)
    {
        ctx->SetHeader("Retry-After", std::to_string(counter.RetryAfter()));
    }

    static uco::task<void>
    Handle(const std::shared_ptr<Counter> &counter,
           const KeyGetter &key_getter, const RejectHandler &reject_handler,
           HttpContext *ctx)
    {
        const std::string key = key_getter(ctx);
        try
        {
            if (!co_await reject_handler(ctx, *counter, key))
            {
                co_await ctx->Next();
                co_return;
            }
        }
        catch (const HttpException &)
        {
            SetRetryAfter(*counter, ctx);
            throw;
        }

        SetRetryAfter(*counter, ctx);
        ctx->Abort("too many requests");
    }

    static HttpServer::HandleFunc
    Make(KeyGetter key_getter, int limit, int window_sec,
         RejectHandler reject_handler)
    {
        if (!key_getter)
        {
            throw std::invalid_argument("ratelimit: key_getter is empty");
        }
        if (!reject_handler)
        {
            reject_handler = DefaultRejectHandler;
        }
        auto counter = std::make_shared<Counter>(limit, window_sec);
        return [counter, key_getter = std::move(key_getter),
                reject_handler = std::move(reject_handler)](
                   HttpContext *ctx) -> uco::task<void> {
            co_await Handle(counter, key_getter, reject_handler, ctx);
        };
    }
};

} // namespace

FixedWindow::FixedWindow() {}

HttpServer::HandleFunc
FixedWindow::ByKey(KeyGetter key_getter, int limit, int window_sec,
                   RejectHandler reject_handler)
{
    return FixedWindowDetail::Make(std::move(key_getter), limit, window_sec,
                                   std::move(reject_handler));
}

HttpServer::HandleFunc
FixedWindow::ByIP(int limit, int window_sec, RejectHandler reject_handler)
{
    return ByKey([](HttpContext *ctx) { return ctx->ClientIP(); },
                 limit, window_sec, std::move(reject_handler));
}

HttpServer::HandleFunc FixedWindow::BySession(int limit, int window_sec,
                                              RejectHandler reject_handler)
{
    return ByKey(
        [](HttpContext *ctx) { return Session::FromContext(ctx)->ID(); },
        limit, window_sec, std::move(reject_handler));
}

} // namespace ratelimit
