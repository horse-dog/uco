#include "httpconnection.h"
#include "httprequest.h"
#include "httpresponce.h"
#include "httpserver.h"
#include "uio.h"
#include "ulog.h"
#include "url.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/types.h>
#include <unordered_map>

namespace fs = std::filesystem;

static std::string path_join(const std::string &a, const std::string &b)
{
    fs::path pa(a);
    fs::path pb;
    if (!b.empty() && b.front() == '/')
    {
        pb = std::string_view(b.c_str() + 1, b.size() - 1);
    }
    else
    {
        pb = std::string_view(b.c_str(), b.size());
    }
    fs::path pc = pa / pb;
    return pc.string();
}

static bool advance_iov(struct iovec *&iov, int &iovcnt, ssize_t written)
{
    while (written > 0 && iovcnt > 0)
    {
        if (written >= (ssize_t)iov->iov_len)
        {
            // 这一段完全写完，跳到下一个
            written -= iov->iov_len;
            ++iov;
            --iovcnt;
        }
        else
        {
            // 部分写入，调整当前 iovec
            iov->iov_base = static_cast<char *>(iov->iov_base) + written;
            iov->iov_len -= written;
            written = 0;
        }
    }
    return iovcnt > 0;
}

static void
split_path_and_query(const std::string &url, std::string &path,
                     std::unordered_map<std::string, std::string> &queryParams)
{
    size_t qpos = url.find('?');
    if (qpos == std::string::npos)
    {
        path = url;
        queryParams.clear();
        return;
    }

    path = url.substr(0, qpos);
    queryParams.clear();

    std::string query = url.substr(qpos + 1);
    std::stringstream ss(query);
    std::string kv;
    while (std::getline(ss, kv, '&'))
    {
        size_t eq = kv.find('=');
        if (eq != std::string::npos)
        {
            std::string key = kv.substr(0, eq);
            std::string value = kv.substr(eq + 1);
            queryParams[key] = value;
        }
        else
        {
            // 没有 '=' 的情况
            queryParams[kv] = "";
        }
    }
}

static bool parse_range(const std::string &range_value, int &from, int &to,
                        size_t filesz)
{
    const std::string prefix = "bytes=";
    if (range_value.rfind(prefix, 0) != 0)
    {
        from = to = 0;
        return false; // 必须以 bytes= 开头
    }

    std::string range = range_value.substr(prefix.size());
    size_t dash_pos = range.find('-');
    if (dash_pos == std::string::npos)
    {
        from = to = 0;
        return false;
    }

    std::string start_str = range.substr(0, dash_pos);
    std::string end_str = range.substr(dash_pos + 1);

    from = 0;
    to = filesz - 1;

    try
    {
        if (start_str.empty())
        {
            // "-N" → 最后 N 个字节
            size_t lastN = std::stoull(end_str);
            if (lastN > filesz)
                lastN = filesz;
            from = filesz - lastN;
        }
        else
        {
            from = std::stoull(start_str);
            if (!end_str.empty())
            {
                to = std::stoull(end_str);
            }
        }

        if (from > to || from >= filesz)
        {
            from = to = 0;
            return false;
        }
        if (to >= filesz)
            to = filesz - 1;
        return true;
    }
    catch (...)
    {
        from = to = 0;
        return false; // 非法数字
    }
}

HttpConnection::HttpConnection(int sock, sockaddr_in &addr,
                               HttpServer::HttpServerInstance *server)
    : m_iSocket(sock), m_sockAddr(addr), m_pHttpServer(server),
      m_httpRequest(server->GetRecvTimeout()),
      m_httpResponse(server->GetSendTimeout(), server->GetKeepAliveCount(), server->GetKeepAliveTimeout())
{
}

static uco_time_t advance_ts(uco_time_t ts, int64_t ns)
{
    int64_t sec = ns / 1'000'000'000LL;
    int64_t nsec = ns % 1'000'000'000LL;

    ts.tv_sec -= sec;
    ts.tv_nsec -= nsec;

    if (ts.tv_nsec < 0)
    {
        ts.tv_sec -= 1;
        ts.tv_nsec += 1'000'000'000LL;
    }

    if (ts.tv_sec < 0)
    {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    }
    return ts;
}

#define TIMEOUT 10
uco::task<bool> HttpConnection::Read()
{
    auto &&buf = m_httpRequest.m_buffer;
    buf.Compact();
    if (m_httpRequest.m_state != HttpRequest::eParsingStart)
    {
        SYSDBG("read more and extend buffer");
        buf.DoubleSize();
    }
    uco_time_t ts = m_httpRequest.m_readReqTime;
    if (ts.tv_sec == 0 && ts.tv_nsec == 0)
    {
        // timeout.
        co_return false;
    }

    auto tp1 = std::chrono::steady_clock::now();
    auto ret = co_await uread(m_iSocket, buf.CurWritePos(), buf.WritableBytes(),
                              0, ts);
    auto tp2 = std::chrono::steady_clock::now();
    auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(tp2 - tp1);
    m_httpRequest.m_readReqTime =
        advance_ts(m_httpRequest.m_readReqTime, dur.count());
    if (ret > 0)
    {
        buf.Advance(ret);
    }
    co_return (ret > 0);
}

uco::task<bool> HttpConnection::Write()
{
    struct iovec iov[2];
    int iovcnt = 0;
    if (true)
    {
        iov[iovcnt].iov_base =
            m_httpResponse.m_httpRspHeaderBuffer.CurReadPos();
        iov[iovcnt].iov_len =
            m_httpResponse.m_httpRspHeaderBuffer.ReadableBytes();
        ++iovcnt;
    }

    bool sendfile = false;
    auto method = HttpServer::HttpMethodStr2Enum(m_httpRequest.m_sMethod);
    if (method != eHead)
    {
        // Only one of file and body is allowed.
        if (m_httpResponse.m_pCache)
        {
            sendfile = true;
        }
        else if (m_httpResponse.m_httpRspContentBuffer.ReadableBytes() != 0)
        {
            iov[iovcnt].iov_base =
                m_httpResponse.m_httpRspContentBuffer.CurReadPos();
            iov[iovcnt].iov_len =
                m_httpResponse.m_httpRspContentBuffer.ReadableBytes();
            ++iovcnt;
        }
    }

    auto iov1 = iov;
    uco_time_t ts = {m_httpResponse.m_iSendTimeoutSec, 0};
    while (iovcnt > 0)
    {
        auto tp1 = std::chrono::steady_clock::now();
        auto ret = co_await uwritev(m_iSocket, iov1, iovcnt, 0, ts);
        auto tp2 = std::chrono::steady_clock::now();
        auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(tp2 - tp1);
        ts = advance_ts(ts, dur.count());
        if (ts.tv_sec == 0 && ts.tv_nsec == 0)
            co_return false;
        if (ret == -ECANCELED)
            continue;
        if (ret < 0)
        {
            SYSERR("writev error:", strerror(-ret));
            co_return false;
        }
        advance_iov(iov1, iovcnt, ret);
    }

    if (sendfile)
    {
        off_t offset = 0;
        int sendfd = m_httpResponse.m_pCache->first;
        size_t iTotal = m_httpResponse.m_pCache->second;
        size_t endpoint = iTotal;

        if (m_httpResponse.m_iPartialFrom != 0 ||
            m_httpResponse.m_iPartialTo != 0)
        {
            offset = m_httpResponse.m_iPartialFrom;
            iTotal = m_httpResponse.m_iPartialTo - m_httpResponse.m_iPartialFrom + 1;
            endpoint = m_httpResponse.m_iPartialTo + 1;
        }

        off_t oldoffset = offset;
        while (offset < endpoint)
        {
            auto tp1 = std::chrono::steady_clock::now();
            auto ret = co_await usendfile(m_iSocket, sendfd, &offset, iTotal, ts);
            auto tp2 = std::chrono::steady_clock::now();
            auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(tp2 - tp1);
            ts = advance_ts(ts, dur.count());
            iTotal -= (oldoffset - offset);
            if (ts.tv_sec == 0 && ts.tv_nsec == 0)
                co_return false;
            if (ret == -ECANCELED)
                continue;
            if (ret < 0)
            {
                // EPIPE means client closed.
                if (ret == -EPIPE || ret == -ECONNRESET)
                {
                    SYSDBG(strerror(-ret));
                }
                else
                {
                    SYSERR(strerror(-ret));
                }
                co_return false;
            }
        }
    }

    if (m_httpResponse.IsKeepAlive())
    {
        ++m_httpResponse.m_iCurrentReqCount;
        if (m_httpResponse.m_iCurrentReqCount >=
            m_httpResponse.m_iKeepAliveCount)
        {
            co_return false;
        }
        m_httpRequest.m_iReadTimeoutSec = m_pHttpServer->GetKeepAliveTimeout();
        m_httpRequest.Reset();
        m_httpResponse.Reset();
        co_return true;
    }
    // if not keep alive, close connection.
    co_return false;
}

uco::task<void> HttpConnection::CloseConnection()
{
    if (m_iSocket > 0)
    {
        co_await uclose(m_iSocket);
    }
    co_return;
}

uco::task<void> HttpConnection::GenErrorPage(int code)
{
    // first search in static dict.
    auto errorPagePath = m_pHttpServer->GetErrorPagePath(code);
    if (!errorPagePath.empty())
    {
        auto cache = co_await StaticResourcePool::GetInstance().GetResource(
            path_join(m_pHttpServer->GetResourceDir(), errorPagePath));
        if (cache->first != -1)
        {
            m_httpResponse.m_httpRspContentType =
                HttpResponse::PathSuffix2FileType(".html");
            m_httpResponse.m_pCache = cache;
            co_return;
        }
        else
        {
            LOGWRN(code, "error page", errorPagePath, "not valid");
        }
    }

    // if static html not exists, use template if exist.
    auto sTemplatePath = m_pHttpServer->GetErrorTemplatePath();
    if (sTemplatePath.empty())
    {
        // use hardcode error.
        m_httpResponse.GenErrorPageDefault(code);
        co_return;
    }
    sTemplatePath = path_join(m_pHttpServer->GetResourceDir(), sTemplatePath);
    co_await m_httpResponse.GenErrorPage(code, sTemplatePath);
}

static bool isSafePath(const std::string &urlPath)
{
    size_t n = urlPath.size();
    size_t i = 0;
    while (i < n)
    {
        while (i < n && urlPath[i] == '/')
        {
            ++i;
        }

        size_t start = i;
        while (i < n && urlPath[i] != '/')
        {
            ++i;
        }
        size_t len = i - start;

        if (len == 1 && urlPath[start] == '.')
        {
            return false;
        }
        if (len == 2 && urlPath[start] == '.' && urlPath[start + 1] == '.')
        {
            return false;
        }
    }
    return true;
}

uco::task<bool> HttpConnection::Process()
{
    auto httpStatus = m_httpRequest.Parse();
    switch (httpStatus)
    {
    case HttpRequest::eRequestIncomplete:
        co_return false;
    case HttpRequest::eRequestBad:
        m_httpResponse.SetKeepAlive(false);
        co_await GenErrorPage(400);
        m_httpResponse.GenHttpHeader();
        co_return true;
    default:
        m_httpResponse.SetHttpRetCode(200);
        m_httpResponse.SetKeepAlive(m_httpRequest.m_bKeepAlive);
        break;
    }

    m_httpResponse.m_httpRspContentType = "text/html"; // default as html.
    auto method = HttpServer::HttpMethodStr2Enum(m_httpRequest.m_sMethod);

    if (m_httpRequest.m_sPath.size() > 1 && m_httpRequest.m_sPath.back() == '/')
    {
        m_httpRequest.m_sPath.pop_back();
        m_httpResponse.SetHttpRetCode(301);
        m_httpResponse.AddHeader("Location", m_httpRequest.m_sPath);
        m_httpResponse.GenHttpHeader();
        co_return true;
    }

    // url_decode path.
    auto sDecodePath = url_decode(m_httpRequest.m_sPath);

    // dir check.
    if (!isSafePath(sDecodePath))
    {
        m_httpResponse.SetKeepAlive(false);
        co_await GenErrorPage(400);
        m_httpResponse.GenHttpHeader();
        co_return true;
    }

    // url_decode body if necessary.
    if (m_httpRequest.m_sContentType == "application/x-www-form-urlencoded")
    {
        auto tmp = std::string(m_httpRequest.m_buffer.CurReadPos(),
                               m_httpRequest.m_buffer.CurReadPos() +
                                   m_httpRequest.m_iHttpRequsetContentLength);
        m_httpRequest.m_buffer.RetriveAll();
        m_httpRequest.m_buffer.Append(url_decode(tmp));
    }

    // Process /search?q=golang&page=2.
    std::string sDecodePathWithoutQuery;
    std::unordered_map<std::string, std::string> mapQueryParams;
    split_path_and_query(sDecodePath, sDecodePathWithoutQuery, mapQueryParams);

    // Forward has the highest priority.
    auto fwdpath = m_pHttpServer->GetForward(sDecodePathWithoutQuery);
    if (!fwdpath.empty())
    {
        m_httpRequest.m_sPath = fwdpath;
    }
    else
    {
        m_httpRequest.m_sPath = sDecodePathWithoutQuery;
    }

    auto [handles, params] =
        m_pHttpServer->GetHandles(method, m_httpRequest.m_sPath);

    if (!handles.empty())
    {
        HttpContext ctx(&m_httpRequest, &m_httpResponse, handles, params,
                        mapQueryParams);
        co_await ctx.Next();
    }
    else
    {
        co_await GenErrorPage(404);
    }

    if (m_httpResponse.m_bShouldGenErrorPage)
    {
        co_await GenErrorPage(m_httpResponse.m_iHttpRetCode);
    }

    if (!m_httpResponse.m_httpRspStaticResourcePath.empty())
    {
        auto cache = co_await StaticResourcePool::GetInstance().GetResource(
            path_join(m_pHttpServer->GetResourceDir(),
                      m_httpResponse.m_httpRspStaticResourcePath));
        if (cache->first != -1)
        {
            if (!m_httpResponse.m_bResourceIsTemplate)
            {
                m_httpResponse.m_httpRspContentType =
                    HttpResponse::GetFileTypeByPath(
                        m_httpResponse.m_httpRspStaticResourcePath);
                m_httpResponse.m_pCache = cache;

                // Process range.
                const int chunksz = 1024 * 256; // hardcode as 256 Kb.
                auto it = m_httpRequest.m_mapHeader.find("Range");
                if (it != m_httpRequest.m_mapHeader.end())
                {
                    bool bret =
                        parse_range(it->second, m_httpResponse.m_iPartialFrom,
                                    m_httpResponse.m_iPartialTo, cache->second);
                    if (bret == false)
                    {
                        m_httpResponse.m_iPartialFrom =
                            m_httpResponse.m_iPartialTo = 0;
                        m_httpResponse.m_pCache.reset();
                        co_await GenErrorPage(400);
                    }
                    else
                    {
                        m_httpResponse.m_iPartialTo = std::min(
                            m_httpResponse.m_iPartialTo,
                            m_httpResponse.m_iPartialFrom + chunksz - 1);

                        m_httpResponse.AddHeader("Accept-Ranges", "bytes");
                        m_httpResponse.AddHeader(
                            "Content-Range",
                            "bytes " +
                                std::to_string(m_httpResponse.m_iPartialFrom) +
                                '-' +
                                std::to_string(m_httpResponse.m_iPartialTo) +
                                '/' +
                                std::to_string(
                                    m_httpResponse.m_pCache->second));

                        m_httpResponse.m_iHttpRetCode = 206; // Partial Content.
                    }
                }
            }
            else
            {
                bool bGen = co_await m_httpResponse.GenHtmlTemplate(
                    cache->first, cache->second);
                if (bGen == false)
                {
                    co_await GenErrorPage(500);
                }
            }
        }
        else
        {
            co_await GenErrorPage(cache->second);
        }
    }
    else if (m_httpResponse.m_httpRspContentType ==
             "application/x-www-form-urlencoded")
    {
        auto tmp =
            std::string(m_httpResponse.m_httpRspContentBuffer.CurReadPos(),
                        m_httpResponse.m_httpRspContentBuffer.CurWritePos());
        m_httpResponse.m_httpRspContentBuffer.RetriveAll();
        m_httpResponse.m_httpRspContentBuffer.Append(url_encode(tmp));
    }

    m_httpResponse.GenHttpHeader();
    co_return true;
}
