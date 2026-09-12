/**
 * @file csrf.cpp
 * @brief CSRF 机制实现, 见 csrf.h.
 */

#include "http/csrf.h"

#include "core/ulog.h"
#include "http/session.h"

#include <openssl/crypto.h> // CRYPTO_memcmp (恒时比对)
#include <openssl/rand.h>

namespace csrf
{

std::string Csrf::kCsrfKey = "csrf";

Csrf::~Csrf() = default;

// ---- 中间件实现 (成员协程经 MakeHandler 绑定, 同 Sessions 模式) ----

/// 签发: session[key] 为空则生成, 已有则复用.
uco::task<void> Csrf::SessionIssue(HttpContext *ctx)
{
    Session *s = Session::FromContext(ctx);

    std::string token = s->Get(Csrf::kCsrfKey);
    if (token.empty())
    { // 首次: 签发并存 session (链尾 auto_save 兜底落盘+下发 cookie).
        token = NewToken();
        s->Set(Csrf::kCsrfKey, token);
        // 匿名会话 (本请求新生, 仅含 token) 用短 TTL: 无 cookie 洪水
        // 每请求一个 key, 短驻把驻留时间从 30 天压到分钟级; 已存在
        // 会话保持原 TTL; 登录时 Rotate 重置覆盖, 升级会话回默认.
        if (s->IsNew())
        {
            s->SetMaxAge(s->MaxAnonymousAge());
        }
        LOGDBG("csrf: token issued, session:", s->ID());
    }

    // token 为纯 hex, 无需转义; 信封结构见 proto/user.proto 的
    // HttpCsrfRsp (单一文档源).
    std::string body =
        "{\"code\":0,\"msg\":\"ok\",\"body\":{\"csrf_token\":\"" + token +
        "\"}}";
    ctx->Data(200, "application/json", body);
    co_return;
}

/// 校验: 头与 session[key] 恒时比对.
uco::task<void> Csrf::SessionCheck(HttpContext *ctx)
{
    // 恒时比对 (CRYPTO_memcmp), 与 cookie 验签 (session.cpp) 保持一致.
    const std::string expected = Session::FromContext(ctx)->Get(Csrf::kCsrfKey);
    const std::string submitted = ctx->GetHeader("X-CSRF-Token");
    const bool match = expected.size() == submitted.size() &&
                       CRYPTO_memcmp(expected.data(), submitted.data(),
                                     expected.size()) == 0;
    if (expected.empty() || !match)
    {
        LOGWRN("csrf: check failed, url:", ctx->GetRequestUrl(),
               ", reason:",
               (expected.empty() ? "no token" : "token mismatch"));
        const std::string body = "{\"code\":403,\"msg\":\"csrf check failed\"}";
        ctx->Data(403, "application/json", body);
        ctx->Abort();
    }
    co_await ctx->Next();
}

std::string Csrf::NewToken()
{
    unsigned char rnd[32];
    if (RAND_bytes(rnd, sizeof(rnd)) != 1)
    {
        LOGFTL("csrf: rand_bytes failed for token");
        return "";
    }
    static const char hex[] = "0123456789abcdef";
    std::string token;
    token.reserve(sizeof(rnd) * 2);
    for (unsigned char b : rnd)
    {
        token += hex[b >> 4];
        token += hex[b & 0xf];
    }
    return token;
}

} // namespace csrf
