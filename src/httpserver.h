#pragma once

#include <netinet/in.h>
#include <string>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <google/protobuf/message.h>

#include "uco.h"
#include "usync.h"

#define BACKLOG 1024

enum HttpMethod
{
    eGet,
    ePost,
    ePut,
    ePatch,
    eHead,
    eOptions,
    eDelete,
    eConnect,
    eTrace,
};

class HttpContext;

class HttpServer
{
  public:
    using HandleFunc = uco::task<void> (*)(HttpContext *);
    ~HttpServer();
    void Init(int port, int num_threads = 4, int keepalivecnt = 100,
              int keepalivesec = 60, int recvtimeoutsec = 10,
              int sendtimeoutsec = 10,
              const std::string &resourceDir = "../res");
    void Run();

    void Forward(const std::string &src, const std::string &dst);

    void Static(const std::string &path);

    template <typename... Fn>
        requires(std::same_as<Fn, HandleFunc> && ...)
    void GET(const std::string &path, Fn... handles)
    {
        m_trees[eGet].insert(path, handles...);
    }

    template <typename... Fn>
        requires(std::same_as<Fn, HandleFunc> && ...)
    void HEAD(const std::string &path, Fn... handles)
    {
        m_trees[eHead].insert(path, handles...);
    }

    template <typename... Fn>
        requires(std::same_as<Fn, HandleFunc> && ...)
    void POST(const std::string &path, Fn... handles)
    {
        m_trees[ePost].insert(path, handles...);
    }

    void ErrorTemplate(const std::string &error_html_template);
    void ErrorPage(int errHttpCode, const std::string &error_html_path);

  private:
    friend class HttpContext;
    friend class HttpConnection;
    friend class HttpServerInstance;

    struct TrieNode
    {
        std::unordered_map<std::string, TrieNode *> children;
        // 命名参数节点，最多一个
        TrieNode *paramChild = nullptr;
        std::string paramName;

        // 通配符节点，最多一个，存储参数名（不包括 '*'）
        TrieNode *wildcardChild = nullptr;
        std::string wildcardName;
        std::vector<HandleFunc> handles;

        bool isEnd = false;

        ~TrieNode();
    };

    class PrefixTree
    {
      private:
        TrieNode *root;

      public:
        PrefixTree();
        ~PrefixTree();

        template <typename... Fn>
            requires(std::same_as<Fn, HandleFunc> && ...)
        void insert(const std::string &path, Fn... handles)
        {
            auto node = __insert(path);
            (node->handles.push_back(handles), ...);
        }

        // 返回是否匹配，并且提取参数（当有命名参数或通配符时）
        bool match(const std::string &path, std::vector<HandleFunc> &handles,
                   std::unordered_map<std::string, std::string> &params) const;

      private:
        TrieNode *__insert(const std::string &path);

        bool
        dfsMatch(TrieNode *node, const std::vector<std::string> &parts,
                 size_t idx, std::vector<HandleFunc> &handles,
                 std::unordered_map<std::string, std::string> &params) const;
    };

    class HttpServerInstance
    {
      public:
        HttpServerInstance() = default;
        ~HttpServerInstance();

        void Init(HttpServer *manager, int port);
        void Run();

      private:
        friend class HttpConnection;
        int GetRecvTimeout() const;
        int GetSendTimeout() const;
        int GetKeepAliveTimeout() const;
        int GetKeepAliveCount() const;
        std::string GetResourceDir() const;
        std::string GetErrorPagePath(int code) const;
        std::string GetErrorTemplatePath() const;
        std::string GetForward(const std::string &path) const;
        std::pair<std::vector<HandleFunc>,
                  std::unordered_map<std::string, std::string>>
        GetHandles(HttpMethod method, const std::string &path) const;
        uco::task<void> ServeHttpClient(int fd, sockaddr_in addr);

        uco::task<void> run();
        uco::task<void> peek_exit();

        int m_iPort = 0;
        int m_iListenSock = -1;
        int m_iRunning = 1;
        HttpServer *m_manager = 0;
        std::unordered_set<int> m_setClientFds;
    };

  private:
    std::pair<std::vector<HandleFunc>,
              std::unordered_map<std::string, std::string>>
    find_handles(HttpMethod method, const std::string &path);
    void setup_signalfd();
    uco::task<void> peek_exit();
    static HttpMethod HttpMethodStr2Enum(const std::string &method);

    int m_iPort = 0;
    int m_iNumThreads = 0;
    uco::usema exit_sema;
    int m_iSignalFd = -1; // Signal handling for exit
    int m_iKeepAliveCount = 100;
    int m_iKeepAliveTime = 60; // seconds.
    int m_iRecvTimeOut = 10;   // seconds.
    int m_iSendTimeOut = 10;   // seconds.
    std::string m_sResourceDir;
    std::string m_sErrorTemplatePath;
    std::vector<HttpServerInstance> m_vecWorkers;
    std::vector<uco::uthread> m_vecThreads;
    std::unordered_map<std::string, std::string> m_fwdDict;
    std::unordered_map<HttpMethod, PrefixTree> m_trees;
    std::unordered_map<int, std::string> m_mapErrorPagePath;
};

class HttpContext
{

  public:
    HttpContext(class HttpRequest *ptrReq, class HttpResponse *ptrRsp,
                std::vector<HttpServer::HandleFunc> &handles,
                std::unordered_map<std::string, std::string> &params,
                std::unordered_map<std::string, std::string> &queryParams);

    // e.g.: /example?foo=bar -> /example
    std::string GetRequestUrl() const;

    /***
     * 对于一个请求链，以下函数只能被最多调用一次
     */
    void Json(int httpRetCode, const google::protobuf::Message &message);

    void String(int httpRetCode, const std::string &msg);

    void Data(int httpRetCode, const std::string &contentType,
              const std::string &data);

    void HTML(int httpRetCode, const std::string &templatePath,
              const std::unordered_map<std::string, std::string> &args);

    void HTML(int httpRetCode, const std::string &templatePath,
              const google::protobuf::Message &args);

    // not a template.
    void HTML(int httpRetCode, const std::string &htmlPath);

    void File(const std::string &filepath);

    // Just set code, no body.
    void Status(int httpRetCode);

    // Just set code, no body.
    void AbortWithStatus(int httpRetCode);

    // set code and corresponding html body.
    // this func will clear file and previous
    // response even if they are set.
    void AbortWithStatusHtml(int httpRetCode);

    void Redirect(int httpRetCode, const std::string &location);

    void Header(const std::string &key, const std::string &value);

    std::string Param(const std::string &name) const;

    std::string Query(const std::string &key) const;

    std::string DefaultQuery(const std::string &key,
                             const std::string &default_value) const;

    const std::unordered_map<std::string, std::string> &QueryAll() const;

    std::string GetRawData();

    std::string_view PeekRawData();

    bool ShouldBindJSON(google::protobuf::Message &message);

#define BindJSON(ctx, message)                                                 \
    do                                                                         \
    {                                                                          \
        if (!ctx->ShouldBindJSON(message))                                     \
        {                                                                      \
            ctx->AbortWithStatusHtml(400);                                     \
            co_return;                                                         \
        }                                                                      \
    } while (0)

    uco::task<void> Next();

    void Abort();

#define CTXNEXT(context) co_await context->Next()
#define CTXABORT(context)                                                      \
    do                                                                         \
    {                                                                          \
        context->Abort();                                                      \
        co_return;                                                             \
    } while (0)

  private:
    class HttpRequest *m_ptrReq = 0;
    class HttpResponse *m_ptrRsp = 0;
    size_t m_iCurHandleIndex = -1;
    bool m_bHasSetRspContent = false;
    std::vector<HttpServer::HandleFunc> m_handles;
    std::unordered_map<std::string, std::string> m_mapParams;
    std::unordered_map<std::string, std::string> m_mapQueryParams;
};
