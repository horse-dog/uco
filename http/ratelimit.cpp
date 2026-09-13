#include "http/ratelimit.h"

#include "core/uconfig.h"
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

/// 计数表条目数上限，超过则触发懒清理。
constexpr size_t kMaxKeys = 65536;

struct FixedWindowDetail
{
    enum class KeyBy
    {
        IP,
        NewSessionIP,
        Session,
        Account,
    };

    struct Entry
    {
        uint64_t window = 0;
        uint64_t count = 0;
    };

    class Counter
    {
      public:
        Counter(int limit, int window_sec)
            : m_limit(limit),
              m_windowMs(static_cast<uint64_t>(window_sec) * 1000)
        {
            if (limit <= 0)
            {
                throw std::invalid_argument(
                    "ratelimit: limit must be positive");
            }
            if (window_sec <= 0)
            {
                throw std::invalid_argument(
                    "ratelimit: window_sec must be positive");
            }
        }

        std::pair<bool, int> Allow(const std::string &key)
        {
            const uint64_t now_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            const uint64_t current_window = now_ms / m_windowMs;

            std::lock_guard<std::mutex> guard(m_mutex);
            if (m_entries.size() > kMaxKeys)
            {
                for (auto it = m_entries.begin(); it != m_entries.end();)
                {
                    it = it->second.window < current_window
                             ? m_entries.erase(it)
                             : ++it;
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
                const uint64_t remain_ms =
                    (current_window + 1) * m_windowMs - now_ms;
                const int retry_sec =
                    static_cast<int>((remain_ms + 999) / 1000);
                return {false, retry_sec < 1 ? 1 : retry_sec};
            }
            ++entry.count;
            return {true, 0};
        }

      private:
        int m_limit;
        uint64_t m_windowMs;
        std::mutex m_mutex;
        std::unordered_map<std::string, Entry> m_entries;
    };

    static std::string ExtractKey(KeyBy key_by, HttpContext *ctx)
    {
        switch (key_by)
        {
        case KeyBy::IP:
            return ctx->ClientIP();
        case KeyBy::NewSessionIP:
            return Session::FromContext(ctx)->IsNew() ? ctx->ClientIP() : "";
        case KeyBy::Session:
            return Session::FromContext(ctx)->ID();
        case KeyBy::Account:
            {
                auto&& form = ctx->BindForm();
                auto it = form.find("username");
                if (it == form.end())
                {
                    ctx->Status(400);
                    ctx->Abort("username field not exist in form");
                }
                auto&& username = it->second;
                if (username.empty())
                {
                    ctx->Status(400);
                    ctx->Abort("empty username field in form");
                }
                LOGERR(NR(username));
                return it->second;
            }
        }
        return "";
    }

    static uco::task<void> Handle(const std::shared_ptr<Counter> &counter,
                                  KeyBy key_by, HttpContext *ctx)
    {
        const std::string key = ExtractKey(key_by, ctx);
        if (!key.empty())
        {
            auto [allowed, retry_sec] = counter->Allow(key);
            if (!allowed)
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

    static HttpServer::HandleFunc Make(const uco::YamlConfig &config,
                                       const std::string &limit_name,
                                       int default_limit, KeyBy key_by)
    {
        const int window_sec =
            config.Get<int>("rate_limit.window_sec", 60);
        const int limit = config.Get<int>("rate_limit." + limit_name,
                                          default_limit);
        auto counter = std::make_shared<Counter>(limit, window_sec);
        return [counter, key_by](HttpContext *ctx) -> uco::task<void> {
            co_await Handle(counter, key_by, ctx);
        };
    }
};

} // namespace

FixedWindow::FixedWindow(const uco::YamlConfig &config) : m_config(config) {}

HttpServer::HandleFunc FixedWindow::ByIP()
{
    return FixedWindowDetail::Make(m_config, "ip_per_window", 256,
                                   FixedWindowDetail::KeyBy::IP);
}

HttpServer::HandleFunc FixedWindow::ByNewSessionIP()
{
    return FixedWindowDetail::Make(m_config, "new_session_ip_per_window", 30,
                                   FixedWindowDetail::KeyBy::NewSessionIP);
}

HttpServer::HandleFunc FixedWindow::BySession()
{
    return FixedWindowDetail::Make(m_config, "session_per_window", 10,
                                   FixedWindowDetail::KeyBy::Session);
}

HttpServer::HandleFunc FixedWindow::ByAccount()
{
    return FixedWindowDetail::Make(m_config, "account_per_window", 10,
                                   FixedWindowDetail::KeyBy::Account);
}

} // namespace ratelimit
