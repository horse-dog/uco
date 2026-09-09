#pragma once

#include <netinet/in.h>
#include <string>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <exception>
#include <concepts>
#include <functional>
#include <map>
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
    /**
     * @brief 路由处理函数 (≈ gin 的 gin.HandlerFunc).
     *        std::function 包装: 兼容普通函数 / lambda(可捕获, ≈ gin 闭包
     *        中间件) / 成员函数绑定, 使 handler 可携带状态 (controller 等).
     * @note 将被拷贝存入路由树, 要求可拷贝;
     *       捕获对象的生命周期由注册方保证 (覆盖全部请求).
     */
    using HandleFunc = std::function<uco::task<void>(HttpContext *)>;
    ~HttpServer();
    void Init(int port, int num_threads = 4, int keepalivecnt = 100,
              int keepalivesec = 60, int recvtimeoutsec = 10,
              int sendtimeoutsec = 10,
              const std::string &resourceDir = "../res");
    uco::task<int> Run();

    /**
     * @brief 路径别名 (URL 美化): 将 src 重写为 dst 后再走路由匹配.
     *        规则按 (method, src) 二元组区分, 同一路径各 method
     *        独立注册互不影响 (如 GET / 与 POST / 各一条).
     * @note Forward 优先级高于动态路由: 同一路径同一 method 下
     *       二者并存时 Forward 生效, 勿如此配置;
     * @note 同一 (method, src) 重复注册时后者覆盖前者.
     */
    void Forward(HttpMethod method, const std::string &src,
                 const std::string &dst);

    void Static(const std::string &path);

    template <typename... Fn>
        requires(std::convertible_to<Fn, HandleFunc> && ...)
    void GET(const std::string &path, Fn... handles)
    {
        m_trees[eGet].insert(path, handles...);
    }

    template <typename... Fn>
        requires(std::convertible_to<Fn, HandleFunc> && ...)
    void HEAD(const std::string &path, Fn... handles)
    {
        m_trees[eHead].insert(path, handles...);
    }

    template <typename... Fn>
        requires(std::convertible_to<Fn, HandleFunc> && ...)
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
            requires(std::convertible_to<Fn, HandleFunc> && ...)
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
        std::string GetForward(HttpMethod method, const std::string &path) const;
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
    std::map<std::pair<HttpMethod, std::string>, std::string> m_fwdDict; ///< (method, src) → dst.
    std::unordered_map<HttpMethod, PrefixTree> m_trees;
    std::unordered_map<int, std::string> m_mapErrorPagePath;
};

// Abort() 抛出的控制流异常：中止 handler 链，
// 由 HttpConnection::Process 统一捕获处理。
// 调用 Abort() 之后的代码不会执行，无需再 co_return。
class HttpException : public std::exception
{
  public:
    HttpException(int httpRetCode, const std::string &msg)
        : m_iHttpRetCode(httpRetCode), m_sMessage(msg)
    {
    }

    const char *what() const noexcept override
    {
        return m_sMessage.empty() ? "http handler chain aborted"
                                  : m_sMessage.c_str();
    }

    int code() const noexcept { return m_iHttpRetCode; }
    const std::string &message() const noexcept { return m_sMessage; }

  private:
    int m_iHttpRetCode = 0;
    std::string m_sMessage;
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

    void Redirect(int httpRetCode, const std::string &location);

    std::string GetHeader(const std::string &key) const;

    void SetHeader(const std::string &key, const std::string &value);

    std::string Param(const std::string &name) const;

    std::string Query(const std::string &key) const;

    std::string DefaultQuery(const std::string &key,
                             const std::string &default_value) const;

    const std::unordered_map<std::string, std::string> &QueryAll() const;

    // SameSite 策略 (对齐 gin 的 c.SetSameSite):
    // 设置后作用于后续 SetCookie, 未设置 (eSameSiteDefault) 不输出该属性.
    enum SameSite
    {
        eSameSiteDefault,
        eSameSiteLax,
        eSameSiteStrict,
        eSameSiteNone, // 需配合 secure, 否则浏览器忽略.
    };

    void SetSameSite(SameSite mode) { m_eSameSite = mode; }

    std::string GetCookie(const std::string& name) const;

    void SetCookie(const std::string& name, const std::string& value,
                   int max_age = -1, const std::string& path = "/",
                   const std::string& domain = "", bool secure = false,
                   bool http_only = false);

    std::string GetRawData();

    std::string_view PeekRawData();

    bool ShouldBindJSON(google::protobuf::Message &message);

    void BindJSON(google::protobuf::Message &message);

    bool ShouldBindForm(std::unordered_map<std::string, std::string> &fields,
                        size_t max_body_size = 1024);

    void BindForm(std::unordered_map<std::string, std::string> &fields,
                  size_t max_body_size = 1024);

    uco::task<void> Next();

    // ---- 用户数据 (≈ gin 的 c.Set/Get): 请求链内传递.
    // 值为裸指针, 生命周期归设置方; 随 HttpContext 一起销毁.
    void Set(const std::string &key, void *value);
    void *Get(const std::string &key) const; ///< 未设置返回 nullptr

    // 设置状态码.
    void Status(int httpRetCode);

    // 生成错误页面
    void GenErrorPage(int httpRetCode);

    // 中止 handler 链.
    void Abort(int httpRetCode, const std::string &msg = "");

    bool GenLog() const { return m_bLog; }

    void DisableLog() { m_bLog = false; }

  private:
    class HttpRequest *m_ptrReq = 0;
    class HttpResponse *m_ptrRsp = 0;
    size_t m_iCurHandleIndex = -1;
    bool m_bHasSetRspContent = false;
    bool m_bLog = true; // 获取静态资源等操作无需日志, 以免刷屏.
    std::vector<HttpServer::HandleFunc> m_handles;
    std::unordered_map<std::string, std::string> m_mapParams;
    std::unordered_map<std::string, std::string> m_mapQueryParams;
    std::unordered_map<std::string, void *> m_mapUserData;
    SameSite m_eSameSite = eSameSiteDefault;
};

#define MakeHandler(obj, method)                                              \
    [ptr = &obj, m = &std::decay_t<decltype(obj)>::method]                    \
        (HttpContext *ctx) -> uco::task<void>                                 \
    {                                                                         \
        co_return co_await (ptr->*m)(ctx);                                    \
    }
