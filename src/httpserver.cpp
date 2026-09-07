#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <netinet/tcp.h>

#include "httpconnection.h"
#include "httprequest.h"
#include "httpresponce.h"
#include "httpserver.h"
#include "json.h"
#include "uco.h"
#include "uio.h"
#include "ulog.h"
#include "uredis.h"
#include "usql.h"
#include "string_utils.h"

HttpContext::HttpContext(
    HttpRequest *ptrReq, HttpResponse *ptrRsp,
    std::vector<HttpServer::HandleFunc> &handles,
    std::unordered_map<std::string, std::string> &params,
    std::unordered_map<std::string, std::string> &queryParams)
    : m_ptrReq(ptrReq), m_ptrRsp(ptrRsp), m_handles(std::move(handles)),
      m_mapParams(std::move(params)), m_mapQueryParams(std::move(queryParams))
{
}

std::string HttpContext::GetRequestUrl() const { return m_ptrReq->m_sPath; }

#define CHECK_HAS_SET_RSPCONTENT                                               \
    if (m_bHasSetRspContent)                                                   \
    {                                                                          \
        LOGERR("already set responce body");                                   \
        return;                                                                \
    }                                                                          \
    m_bHasSetRspContent = true

void HttpContext::Json(int httpRetCode,
                       const google::protobuf::Message &message)
{
    if (m_ptrRsp)
    {
        CHECK_HAS_SET_RSPCONTENT;
        m_ptrRsp->m_httpRspContentType =
            HttpResponse::PathSuffix2FileType(".json");
        m_ptrRsp->m_iHttpRetCode = httpRetCode;
        std::string json;
        if (MessageToJson(message, json))
        {
            m_ptrRsp->m_httpRspContentBuffer.Append(json);
        }
        else
        {
            LOGERR("message: %s", message.ShortDebugString().c_str());
            m_ptrRsp->ShouldGenErrorPage(500);
        }
    }
}

void HttpContext::String(int httpRetCode, const std::string &msg)
{
    if (m_ptrRsp)
    {
        CHECK_HAS_SET_RSPCONTENT;
        m_ptrRsp->m_httpRspContentType =
            HttpResponse::PathSuffix2FileType(".txt");
        m_ptrRsp->m_iHttpRetCode = httpRetCode;
        m_ptrRsp->m_httpRspContentBuffer.Append(msg);
    }
}

void HttpContext::Data(int httpRetCode, const std::string &contentType,
                       const std::string &data)
{
    if (m_ptrRsp)
    {
        CHECK_HAS_SET_RSPCONTENT;
        m_ptrRsp->m_httpRspContentType = contentType;
        m_ptrRsp->m_iHttpRetCode = httpRetCode;
        m_ptrRsp->m_httpRspContentBuffer.Append(data);
    }
}

static bool isTemplateHTML(const std::string &templatePath)
{
    return templatePath.size() > 4 &&
           (templatePath.compare(templatePath.size() - 5, 5, ".html") == 0 ||
            templatePath.compare(templatePath.size() - 5, 5, ".tmpl") == 0 ||
            templatePath.compare(templatePath.size() - 4, 4, ".tpl") == 0);
}

void HttpContext::HTML(int httpRetCode, const std::string &templatePath,
                       const std::unordered_map<std::string, std::string> &args)
{
    if (!isTemplateHTML(templatePath))
    {
        LOGERR(templatePath, "not a template (*.html, *.tmpl, *.tpl)");
        Status(400);
        Abort(400, "Not a template file");
    }
    if (m_ptrRsp)
    {
        CHECK_HAS_SET_RSPCONTENT;
        m_ptrRsp->m_httpRspContentType =
            HttpResponse::PathSuffix2FileType(".html");
        m_ptrRsp->m_iHttpRetCode = httpRetCode;
        m_ptrRsp->m_httpRspStaticResourcePath = templatePath;
        m_ptrRsp->m_mapTemplateArgs = args;
        m_ptrRsp->m_bResourceIsTemplate = true;
    }
}

void HttpContext::HTML(int httpRetCode, const std::string &templatePath,
                       const google::protobuf::Message &args)
{
    if (!isTemplateHTML(templatePath))
    {
        LOGERR(templatePath, "not a template (*.html, *.tmpl, *.tpl)");
        Status(400);
        Abort(400, "Not a template file");
    }
    if (m_ptrRsp)
    {
        CHECK_HAS_SET_RSPCONTENT;
        m_ptrRsp->m_httpRspContentType =
            HttpResponse::PathSuffix2FileType(".html");
        m_ptrRsp->m_iHttpRetCode = httpRetCode;
        m_ptrRsp->m_httpRspStaticResourcePath = templatePath;
        m_ptrRsp->m_bResourceIsTemplate = true;

        const google::protobuf::Descriptor *descriptor = args.GetDescriptor();
        const google::protobuf::Message *prototype =
            google::protobuf::MessageFactory::generated_factory()->GetPrototype(
                descriptor);

        if (m_ptrRsp->m_protoTemplateArgs != nullptr)
        {
            delete m_ptrRsp->m_protoTemplateArgs;
        }
        m_ptrRsp->m_protoTemplateArgs = prototype->New();
        m_ptrRsp->m_protoTemplateArgs->CopyFrom(args);
    }
}

void HttpContext::HTML(int httpRetCode, const std::string &htmlPath)
{
    if (m_ptrRsp)
    {
        CHECK_HAS_SET_RSPCONTENT;
        m_ptrRsp->m_httpRspContentType =
            HttpResponse::PathSuffix2FileType(".html");
        m_ptrRsp->m_iHttpRetCode = httpRetCode;
        m_ptrRsp->m_httpRspStaticResourcePath = htmlPath;
    }
}

void HttpContext::File(const std::string &filepath)
{
    if (m_ptrRsp)
    {
        CHECK_HAS_SET_RSPCONTENT;
        m_ptrRsp->m_httpRspStaticResourcePath = filepath;
    }
}

void HttpContext::Status(int httpRetCode)
{
    if (m_ptrRsp)
    {
        m_ptrRsp->m_iHttpRetCode = httpRetCode;
    }
}

void HttpContext::GenErrorPage(int httpRetCode)
{
    if (m_ptrRsp)
    {
        m_bHasSetRspContent = true;
        m_ptrRsp->ShouldGenErrorPage(httpRetCode);
    }
}

void HttpContext::Redirect(int httpRetCode, const std::string &location)
{
    if (m_ptrRsp)
    {
        CHECK_HAS_SET_RSPCONTENT;
        m_ptrRsp->m_iHttpRetCode = httpRetCode;
        SetHeader("Location", location);
    }
}

void HttpContext::SetHeader(const std::string &key, const std::string &value)
{
    if (m_ptrRsp)
    {
        m_ptrRsp->AddHeader(key, value);
    }
}

std::string HttpContext::Param(const std::string &name) const
{
    auto it = m_mapParams.find(name);
    if (it != m_mapParams.end())
    {
        return it->second;
    }
    return "";
}

std::string HttpContext::Query(const std::string &key) const
{
    auto it = m_mapQueryParams.find(key);
    if (it != m_mapQueryParams.end())
    {
        return it->second;
    }
    return "";
}

std::string HttpContext::DefaultQuery(const std::string &key,
                                      const std::string &default_value) const
{
    auto it = m_mapQueryParams.find(key);
    if (it != m_mapQueryParams.end())
    {
        return it->second;
    }
    return default_value;
}

const std::unordered_map<std::string, std::string> &
HttpContext::QueryAll() const
{
    return m_mapQueryParams;
}

std::string HttpContext::GetHeader(const std::string &key) const
{
    if (m_ptrReq == nullptr) return "";
    for (const auto &[name, value] : m_ptrReq->m_mapHeader)
    {
        if (name.size() != key.size()) continue;
        bool equal = true;
        for (size_t i = 0; i < name.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(name[i])) !=
                std::tolower(static_cast<unsigned char>(key[i])))
            {
                equal = false;
                break;
            }
        }
        if (equal) return value;
    }
    return "";
}

std::string HttpContext::GetCookie(const std::string& name) const
{
    if (name.empty()) return "";

    const std::string header = GetHeader("Cookie");
    std::string value;
    bool found = false;
    size_t begin = 0;
    while (begin < header.size())
    {
        size_t end = header.find(';', begin);
        if (end == std::string::npos) end = header.size();

        std::string_view item(header.data() + begin, end - begin);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
            item.remove_prefix(1);
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
            item.remove_suffix(1);

        const size_t separator = item.find('=');
        if (separator != std::string_view::npos &&
            item.substr(0, separator) == name)
        {
            if (found) return "";
            value.assign(item.substr(separator + 1));
            found = true;
        }
        begin = end + 1;
    }
    return value;
}

std::string HttpContext::GetRawData()
{
    std::string res;
    if (m_ptrReq == nullptr)
        return res;
    if (m_ptrReq->m_state == HttpRequest::eParsingFinish)
    {
        res = std::string(m_ptrReq->m_buffer.CurReadPos(),
                          m_ptrReq->m_buffer.CurReadPos() +
                              m_ptrReq->m_iHttpRequsetContentLength);
        m_ptrReq->m_buffer.Retrieve(m_ptrReq->m_iHttpRequsetContentLength);
    }
    return res;
}

std::string_view HttpContext::PeekRawData()
{
    std::string_view res;
    if (m_ptrReq == nullptr)
        return res;
    if (m_ptrReq->m_state == HttpRequest::eParsingFinish)
    {
        res = std::string_view(m_ptrReq->m_buffer.CurReadPos(),
                               m_ptrReq->m_iHttpRequsetContentLength);
    }
    return res;
}

bool HttpContext::ShouldBindJSON(google::protobuf::Message &message)
{
    if (m_ptrReq == nullptr) return false;
    const std::string_view body = PeekRawData();
    if (body.empty())
    {
        LOGERR("empty body");
        return false;
    }
    if (!JsonToMessage(body, message))
    {
        LOGERR("parse error", NR(body));
        return false;
    }
    m_ptrReq->m_buffer.Retrieve(m_ptrReq->m_iHttpRequsetContentLength);
    return true;
}

void HttpContext::BindJSON(google::protobuf::Message &message)
{
    if (!ShouldBindJSON(message))
    {
        Status(400);
        Abort(400, "BindJSON");
    }
}

bool decode_component(std::string_view value, std::string &result)
{
    result.clear();
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '+')
        {
            result.push_back(' ');
        }
        else if (value[i] == '%')
        {
            if (i + 2 >= value.size()) return false;
            const int high = uco::CharToHex(value[i + 1]);
            const int low = uco::CharToHex(value[i + 2]);
            if (high < 0 || low < 0) return false;
            result.push_back(static_cast<char>((high << 4) | low));
            i += 2;
        }
        else
        {
            result.push_back(value[i]);
        }
    }
    return true;
}

static bool parse_urlencoded(std::string_view body,
    std::unordered_map<std::string, std::string> &fields)
{
    fields.clear();
    size_t begin = 0;
    while (begin < body.size())
    {
        size_t end = body.find('&', begin);
        if (end == std::string_view::npos) end = body.size();
        const std::string_view item = body.substr(begin, end - begin);
        const size_t equal = item.find('=');
        if (equal == std::string_view::npos) return false;

        std::string key;
        std::string value;
        if (!decode_component(item.substr(0, equal), key) ||
            !decode_component(item.substr(equal + 1), value) || key.empty() ||
            !fields.emplace(std::move(key), std::move(value)).second)
            return false;
        begin = end + 1;
    }
    return !fields.empty();
}

bool HttpContext::ShouldBindForm(
    std::unordered_map<std::string, std::string> &fields, size_t max_body_size)
{
    if (m_ptrReq == nullptr) return false;

    const std::string_view body = PeekRawData();
    if (body.empty())
    {
        LOGERR("empty body");
        return false;
    }
    else if (body.size() > max_body_size)
    {
        LOGERR(body.size(), max_body_size);
        return false;
    }
    if (!parse_urlencoded(body, fields))
    {
        LOGERR("parse error", NR(body));
        return false;
    }
    m_ptrReq->m_buffer.Retrieve(m_ptrReq->m_iHttpRequsetContentLength);
    return true;
}

void HttpContext::BindForm(std::unordered_map<std::string, std::string> &fields,
                           size_t max_body_size)
{
    if (!ShouldBindForm(fields, max_body_size))
    {
        Status(400);
        Abort(400, "BindForm");
    }
}

uco::task<void> HttpContext::Next()
{
    ++m_iCurHandleIndex;
    while (m_iCurHandleIndex < m_handles.size())
    {
        co_await m_handles[m_iCurHandleIndex](this);
        ++m_iCurHandleIndex;
    }
    co_return;
}

void HttpContext::Abort(int httpRetCode, const std::string &msg)
{
    throw HttpException(httpRetCode, msg);
}

HttpServer::HttpServerInstance::~HttpServerInstance()
{
    if (m_iListenSock > 0)
    {
        close(m_iListenSock);
    }
}

void HttpServer::HttpServerInstance::Init(HttpServer *manager, int port)
{
    this->m_manager = manager;
    struct sockaddr_in serv_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    m_iListenSock = socket(AF_INET, SOCK_STREAM, 0);
    const int val = 1;
    setsockopt(m_iListenSock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    setsockopt(m_iListenSock, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val));
    setsockopt(m_iListenSock, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = INADDR_ANY;

    // bind and listen
    if (bind(m_iListenSock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        SYSFTL("bind:", strerror(errno));
    }
    if (listen(m_iListenSock, BACKLOG) < 0)
    {
        SYSFTL("listen:", strerror(errno));
    }
    SYSMSG("listening on port:", port);
}

void HttpServer::HttpServerInstance::Run()
{
    go run();
    go peek_exit();
}

int HttpServer::HttpServerInstance::GetRecvTimeout() const
{
    if (m_manager == nullptr)
    {
        return 10;
    }
    return m_manager->m_iRecvTimeOut;
}

int HttpServer::HttpServerInstance::GetSendTimeout() const
{
    if (m_manager == nullptr)
    {
        return 10;
    }
    return m_manager->m_iSendTimeOut;
}

int HttpServer::HttpServerInstance::GetKeepAliveTimeout() const
{
    if (m_manager == nullptr)
    {
        return 60;
    }
    return m_manager->m_iKeepAliveTime;
}

int HttpServer::HttpServerInstance::GetKeepAliveCount() const
{
    if (m_manager == nullptr)
    {
        return 100;
    }
    return m_manager->m_iKeepAliveCount;
}

std::string HttpServer::HttpServerInstance::GetResourceDir() const
{
    if (m_manager == nullptr)
    {
        return "";
    }
    return m_manager->m_sResourceDir;
}

std::string HttpServer::HttpServerInstance::GetErrorPagePath(int code) const
{
    if (m_manager == nullptr)
    {
        return "";
    }
    auto it = m_manager->m_mapErrorPagePath.find(code);
    if (it != m_manager->m_mapErrorPagePath.end())
    {
        return it->second;
    }
    return "";
}

std::string HttpServer::HttpServerInstance::GetErrorTemplatePath() const
{
    if (m_manager == nullptr)
    {
        return "";
    }
    return m_manager->m_sErrorTemplatePath;
}

std::string
HttpServer::HttpServerInstance::GetForward(const std::string &path) const
{
    if (m_manager == nullptr)
    {
        return "";
    }
    auto it = m_manager->m_fwdDict.find(path);
    if (it != m_manager->m_fwdDict.end())
    {
        return it->second;
    }
    else
    {
        return "";
    }
}

std::pair<std::vector<HttpServer::HandleFunc>,
          std::unordered_map<std::string, std::string>>
HttpServer::HttpServerInstance::GetHandles(HttpMethod method,
                                           const std::string &path) const
{
    if (m_manager == nullptr)
        return {};
    return m_manager->find_handles(method, path);
}

uco::task<void> HttpServer::HttpServerInstance::run()
{
    while (m_iRunning)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int conn_fd = co_await uaccept(
            m_iListenSock, (struct sockaddr *)&client_addr, &client_len);
        if (conn_fd > 0)
        {
            // 注意这里异步启动协程，client_addr 一定要值传递，不能引用传递
            // 虽然这里引用传递也没问题，因为ServeHttpClient要执行到 conn.Read 才会被切出
            // 这之前 addr 已经被复制到 conn 了.
            // 但是这里是因为 ServeHttpClient 的实现特殊才可以.
            // go 启动协程，都需要传值，否则需要手动保证生命周期（例如用锁，信号量等）.
            go ServeHttpClient(conn_fd, client_addr);
        }
        else if (conn_fd == -ECANCELED)
        {
            break;
        }
        else
        {
            SYSERR("accept:", strerror(errno));
        }
    }
}

uco::task<void>
HttpServer::HttpServerInstance::ServeHttpClient(int fd, sockaddr_in addr)
{
    HttpConnection conn(fd, addr, this);
    m_setClientFds.insert(fd);
    while (1)
    {
        if (!m_setClientFds.contains(fd)) break;
        bool bRet = false;
        bRet = co_await conn.Read();
        if (!bRet)
            break;
        bRet = co_await conn.Process();
        if (!bRet)
            continue;
        bRet = co_await conn.Write();
        if (!bRet)
            break;
    }
    co_await conn.CloseConnection();
    m_setClientFds.erase(fd);
}

uco::task<void> HttpServer::HttpServerInstance::peek_exit()
{
    co_await m_manager->exit_sema.wait();
    m_iRunning = false;

    co_await ucancel(m_iListenSock);
    while (!m_setClientFds.empty())
    {
        SYSMSG("cancel", m_setClientFds.size(), "clients");
        auto clients = m_setClientFds;
        m_setClientFds.clear();
        for (int fd : clients)
        {
            co_await ucancel(fd);
        }
    }
    co_return;
}

HttpServer::~HttpServer()
{
    if (m_iSignalFd > 0)
    {
        close(m_iSignalFd);
    }
    m_vecWorkers.clear();
    // 这里打印日志依赖全局变量和TLS，但这些数据是基础数据类型，因此此时仍然可用.
    SYSMSG("Server Exit...");
}

void HttpServer::Init(int port, int num_threads, int keepalivecnt,
                      int keepalivesec, int recvtimeoutsec, int sendtimeoutsec,
                      const std::string &resourceDir)
{
    this->m_iPort = port;
    this->m_iNumThreads = num_threads;
    this->m_sResourceDir = resourceDir;
    this->m_iKeepAliveCount = keepalivecnt;
    this->m_iKeepAliveTime = keepalivesec;
    this->m_iRecvTimeOut = recvtimeoutsec;
    this->m_iSendTimeOut = sendtimeoutsec;
    m_vecWorkers.resize(num_threads);
    setup_signalfd();
}

void HttpServer::InitMySqLPool()
{
    usql::upool::config config;
    config.host = "127.0.0.1";
    config.user = "root";
    config.pass = "123456";
    config.db = "webserver";
    config.max_size = 16;
    usql::upool::GetInstance().Init(config);
}
void HttpServer::InitRedisPool()
{
    uredis::upool::config config;
    config.max_size = 16;
    uredis::upool::GetInstance().Init(config);
}

void HttpServer::Run()
{
    InitMySqLPool();
    InitRedisPool();

    for (int i = 1; i < m_iNumThreads; i++)
    {
        m_vecThreads.emplace_back(
            [this, i]
            {
                this->m_vecWorkers[i].Init(this, m_iPort);
                this->m_vecWorkers[i].Run();
            });
        m_vecThreads.back().daemonize();
    }

    SYSMSG("threads size:", m_vecThreads.size() + 1);

    m_vecWorkers[0].Init(this, m_iPort);
    m_vecWorkers[0].Run();
    go peek_exit();
}

void HttpServer::Forward(const std::string &src, const std::string &dst)
{
    m_fwdDict.emplace(src, dst);
}

static uco::task<void> default_handle(HttpContext *context)
{
    context->File(context->GetRequestUrl());
    co_return;
}

void HttpServer::Static(const std::string &path)
{
    m_trees[eGet].insert(path, default_handle);
}

void HttpServer::ErrorTemplate(const std::string &error_html_template)
{
    m_sErrorTemplatePath = error_html_template;
}

void HttpServer::ErrorPage(int errHttpCode, const std::string &error_html_path)
{
    m_mapErrorPagePath.emplace(errHttpCode, error_html_path);
}

std::pair<std::vector<HttpServer::HandleFunc>,
          std::unordered_map<std::string, std::string>>
HttpServer::find_handles(HttpMethod method, const std::string &path)
{
    std::vector<HandleFunc> handles;
    std::unordered_map<std::string, std::string> params;
    auto it = m_trees.find(method);
    if (it != m_trees.end())
    {
        auto &&trie = it->second;
        trie.match(path, handles, params);
    }
    return {handles, params};
}

void HttpServer::setup_signalfd()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGPIPE);
    sigprocmask(SIG_BLOCK, &mask, nullptr);

    m_iSignalFd = signalfd(-1, &mask, 0);
    if (m_iSignalFd == -1)
    {
        SYSERR("signalfd:", strerror(errno));
        exit(1);
    }
}

uco::task<void> HttpServer::peek_exit()
{
    struct signalfd_siginfo siginfo {};
    while (1)
    {
        ssize_t s = co_await uread(m_iSignalFd, &siginfo, sizeof(siginfo));
        if (s != sizeof(siginfo))
        {
            SYSERR("read:", strerror(errno));
            continue;
        }
        if (siginfo.ssi_signo == SIGINT)
        {
            SYSMSG("Caught SIGINT signal");
            for (int i = 0; i < m_iNumThreads; i++)
                exit_sema.signal();
            break;
        }
        else if (siginfo.ssi_signo == SIGTERM)
        {
            SYSMSG("Caught SIGTERM signal");
            for (int i = 0; i < m_iNumThreads; i++)
                exit_sema.signal();
            break;
        }
        else if (siginfo.ssi_signo == SIGPIPE)
        {
            LOGDBG("ignore SIGPIPE");
        }
        LOGDBG("xread ret %zd", s);
    }

    usql::upool::GetInstance().Close();
    uredis::upool::GetInstance().Close();
    co_return;
}

const std::unordered_map<std::string, HttpMethod> HttpMethodStr2EnumMap = {
    {"GET", HttpMethod::eGet},       {"POST", HttpMethod::ePost},
    {"PUT", HttpMethod::ePut},       {"PATCH", HttpMethod::ePatch},
    {"HEAD", HttpMethod::eHead},     {"OPTIONS", HttpMethod::eOptions},
    {"DELETE", HttpMethod::eDelete}, {"CONNECT", HttpMethod::eConnect},
    {"TRACE", HttpMethod::eTrace},
};

HttpMethod HttpServer::HttpMethodStr2Enum(const std::string &method)
{
    std::string str = method;
    std::transform(str.begin(), str.end(), str.begin(), ::toupper);
    auto it = HttpMethodStr2EnumMap.find(str);
    if (it != HttpMethodStr2EnumMap.end())
    {
        return it->second;
    }
    return HttpMethod::eGet;
}

///////////////////////
// Prefix tree.
///////////////////////
using namespace std;
static vector<string> splitPath(const string &path)
{
    vector<string> res;
    stringstream ss(path);
    string segment;
    while (getline(ss, segment, '/'))
    {
        if (!segment.empty())
        {
            res.push_back(segment);
        }
    }
    return res;
}

HttpServer::TrieNode::~TrieNode()
{
    for (auto &p : children)
        delete p.second;
    if (paramChild)
        delete paramChild;
    if (wildcardChild)
        delete wildcardChild;
}

HttpServer::PrefixTree::PrefixTree() { root = new TrieNode(); }
HttpServer::PrefixTree::~PrefixTree() { delete root; }

HttpServer::TrieNode *HttpServer::PrefixTree::__insert(const std::string &path)
{
    vector<string> parts = splitPath(path);
    TrieNode *cur = root;
    for (size_t i = 0; i < parts.size(); i++)
    {
        string &segment = parts[i];
        if (!segment.empty() && segment[0] == ':')
        {
            if (!cur->paramChild)
            {
                cur->paramChild = new TrieNode();
                cur->paramChild->paramName = segment.substr(1);
            }
            cur = cur->paramChild;
        }
        else if (!segment.empty() && segment[0] == '*')
        {
            if (!cur->wildcardChild)
            {
                cur->wildcardChild = new TrieNode();
                cur->wildcardChild->wildcardName = segment.substr(1);
            }
            cur = cur->wildcardChild;
            break;
        }
        else
        {
            if (cur->children.count(segment) == 0)
            {
                cur->children[segment] = new TrieNode();
            }
            cur = cur->children[segment];
        }
    }
    cur->isEnd = true;
    return cur;
}

bool HttpServer::PrefixTree::match(const string &path,
                                   std::vector<HandleFunc> &handles,
                                   unordered_map<string, string> &params) const
{
    handles.clear();
    vector<string> parts = splitPath(path);
    return dfsMatch(root, parts, 0, handles, params);
}

bool HttpServer::PrefixTree::dfsMatch(
    TrieNode *node, const vector<string> &parts, size_t idx,
    std::vector<HandleFunc> &handles,
    unordered_map<string, string> &params) const
{
    if (!node)
        return false;

    if (idx == parts.size())
    {
        if (node->isEnd)
        {
            handles = node->handles;
            return true;
        }
        // 通配符节点匹配空路径
        if (node->wildcardChild && node->wildcardChild->isEnd)
        {
            params[node->wildcardChild->wildcardName] = "";
            handles = node->wildcardChild->handles;
            return true;
        }
        return false;
    }

    string segment = parts[idx];

    if (node->children.count(segment))
    {
        if (dfsMatch(node->children[segment], parts, idx + 1, handles, params))
        {
            return true;
        }
    }

    if (node->paramChild)
    {
        params[node->paramChild->paramName] = segment;
        if (dfsMatch(node->paramChild, parts, idx + 1, handles, params))
        {
            return true;
        }
        params.erase(node->paramChild->paramName); // 回溯
    }

    if (node->wildcardChild && node->wildcardChild->isEnd)
    {
        string rest;
        for (size_t i = idx; i < parts.size(); i++)
        {
            if (i > idx)
                rest += "/";
            rest += parts[i];
        }
        params[node->wildcardChild->wildcardName] = rest;
        handles = node->wildcardChild->handles;
        return true;
    }

    return false;
}
