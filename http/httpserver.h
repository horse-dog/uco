#pragma once

#include <any>
#include <memory>
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

#include "core/uco.h"
#include "core/uconfig.h"
#include "core/usync.h"

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
    /** @brief 路由处理函数，可为普通函数/lambda/成员函数绑定. */
    using HandleFunc = std::function<uco::task<void>(HttpContext *)>;
    /**
     * @brief 构造并配置 HTTP 服务器。
     *
     * 构造过程会创建 worker 配置并初始化退出信号处理，因此应在日志系统
     * 初始化完成后、创建工作线程前调用。
     *
     * @param port 监听端口。
     * @param num_threads HTTP worker 线程数，必须大于 0。
     * @param keepalivecnt 单个连接允许处理的最大请求数；0 表示不限制，
     *        内部按 std::int32_t 最大值处理；必须大于等于 0。
     * @param keepalivesec keep-alive 状态下等待下一次请求的超时时间，单位秒。
     * @param recvtimeoutsec 首次读取请求的超时时间，单位秒。
     * @param sendtimeoutsec 发送响应的超时时间，单位秒。
     * @param resourceDir 静态资源目录。
     */
    HttpServer(int port, int num_threads = 4, int keepalivecnt = 100,
               int keepalivesec = 60, int recvtimeoutsec = 10,
               int sendtimeoutsec = 10,
               const std::string &resourceDir = "res");

    /**
     * @brief 从 YAML 配置的指定节点构造 HTTP 服务器。
     * @param config 配置读取器。
     * @param prefix HTTP 配置节点前缀，默认读取 http.*。
     */
    explicit HttpServer(const uco::YamlConfig &config,
                        const std::string &prefix = "http");
    ~HttpServer();
    uco::task<void> Run();

    /** @brief 路径别名: 将 src 重写为 dst 后再路由匹配. 优先于动态路由; 重复注册后者覆盖. */
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
        HttpServerInstance()
        {
            m_mtxClientRoutineCount = new uco::umutex();
            m_condClientRoutineCount = new uco::ucond();
        }
        ~HttpServerInstance();

        void Init(HttpServer *manager, int port);
        uco::task<void> Run();

      private:
        friend class HttpServer;
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
        int m_iClientRoutineCount = 0;
        uco::umutex* m_mtxClientRoutineCount = 0;
        uco::ucond* m_condClientRoutineCount = 0;
        HttpServer *m_manager = 0;
        std::unordered_set<int> m_setClientFds;
    };

  private:
    std::pair<std::vector<HandleFunc>,
              std::unordered_map<std::string, std::string>>
    find_handles(HttpMethod method, const std::string &path);
    // 配置实体化 (由构造函数调用): 存参数 + 建 worker + signalfd.
    void Init(int port, int num_threads, int keepalivecnt, int keepalivesec,
              int recvtimeoutsec, int sendtimeoutsec,
              const std::string &resourceDir);
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

/**
 * @brief Abort() 抛出的控制流异常: 中止 handler 链, 由 HttpConnection::Process 捕获处理.
 * @note Abort() 之后的代码不会执行, 无需再 co_return.
 */
class HttpException : public std::exception
{
  public:
    HttpException(const std::string &msg)
        : m_sMessage(msg)
    {
    }

    const char *what() const noexcept override
    {
        return m_sMessage.c_str();
    }

    const std::string &message() const noexcept { return m_sMessage; }

  private:
    std::string m_sMessage;
};

class HttpContext
{

  public:
    /**
     * @brief Store 返回的 RAII 守卫: 析构时清除对应槽位.
     * @note  值匹配才清 (键值版 erase 节点 / 单槽版置空), 已被覆盖则不动;
     *       不得比 ctx 活得久.
     */
    class [[nodiscard]] StoreGuard
    {
      public:
        StoreGuard(StoreGuard &&o) noexcept
            : m_ctx(o.m_ctx), m_key(std::move(o.m_key)), m_val(o.m_val)
        {
            o.m_ctx = nullptr;
        }

        ~StoreGuard() { Release(); }

        StoreGuard(const StoreGuard &) = delete;
        StoreGuard &operator=(const StoreGuard &) = delete;

      private:
        friend class HttpContext; // 仅可由 Store 构造.

        StoreGuard(HttpContext *ctx, std::string key, void *val)
            : m_ctx(ctx), m_key(std::move(key)), m_val(val)
        {
        }

        void Release()
        {
            if (m_ctx == nullptr)
            {
                return;
            }
            if (m_key.empty()) // 单槽版
            {
                if (m_ctx->m_user_defined_data == m_val)
                {
                    m_ctx->m_user_defined_data = nullptr;
                }
            }
            else // 键值版: erase 节点, 不留 nullptr 垃圾.
            {
                auto it = m_ctx->m_mapUserData.find(m_key);
                if (it != m_ctx->m_mapUserData.end() && it->second == m_val)
                {
                    m_ctx->m_mapUserData.erase(it);
                }
            }
            m_ctx = nullptr;
        }

        HttpContext *m_ctx = nullptr;
        std::string m_key;    // 空 = 单槽版.
        void *m_val = nullptr;
    };

    HttpContext(class HttpRequest *ptrReq, class HttpResponse *ptrRsp,
                std::vector<HttpServer::HandleFunc> &handles,
                std::unordered_map<std::string, std::string> &params,
                std::unordered_map<std::string, std::string> &queryParams,
                const std::string &clientIP = "");

    // e.g.: /example?foo=bar -> /example
    std::string GetRequestUrl() const;

    /// 客户端 IP (TCP peer, 由连接层填入; 直连部署不可伪造,
    /// 反向代理后为代理 IP).
    const std::string &ClientIP() const;

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

    /** @brief 设置 Cookie 的 SameSite 策略, 作用于后续 SetCookie. */
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

    /**
     * @brief 键值版用户数据: 可存多份, 请求链内传递.
     * @note  值为裸指针, 生命周期归设置方; 同 key 重复 Store 覆盖;
     *       返回守卫析构时清除该 key (值不匹配则不动).
     */
    [[nodiscard]] StoreGuard Store(const std::string &key, void *value);

    /** @brief 取键值版用户数据; 未设置返回 nullptr. */
    void *Load(const std::string &key) const;

    /**
     * @brief 快速用户数据: 单槽存取, 免键值版 map 查找.
     * @note  生命周期归设置方; 仅可 Store 一次 (重复 Store fail-fast);
     *       Load 的 _Tp 须与 Store 一致, 未 Store 即 Load 亦 fail-fast;
     *       返回守卫析构时清空单槽 (值不匹配则不动).
     */
    template <class _Tp>
    [[nodiscard]] StoreGuard Store(_Tp& object)
    {
        if (m_user_defined_data != nullptr)
        {
            LOGFTL("HttpContext::Store: user data already set");
        }
        m_user_defined_data = std::addressof(object);
        return StoreGuard(this, std::string(), m_user_defined_data);
    }

    /** 
     * @brief 取快速用户数据; _Tp 须与 Store 一致, 未 Store 则 fail-fast. 
     */
    template <class _Tp>
    _Tp& Load()
    {
        if (m_user_defined_data == nullptr)
        {
            LOGFTL("HttpContext::Load: user data not set");
        }
        return *static_cast<_Tp *>(m_user_defined_data);
    }

    /**
     * @brief any 版用户数据: 值语义 (拷贝/移动入 map), 同 key 重复 Set 覆盖.
     * @note  字面量按原类型存, 如 Set(k, "x") 存 const char*.
     */
    template <class _Tp>
    void Set(const std::string &key, _Tp &&value)
    {
        m_mapAnyUserData.insert_or_assign(key, std::forward<_Tp>(value));
    }

    /** @brief 取 any 版用户数据; 未设置或 _Tp 与 Set 不符 fail-fast. */
    template <class _Tp>
    const _Tp &Get(const std::string &key) const
    {
        auto it = m_mapAnyUserData.find(key);
        if (it == m_mapAnyUserData.end())
        {
            LOGFTLF("HttpContext::Get: any key not set: %s", key.c_str());
        }
        try
        {
            return std::any_cast<const _Tp &>(it->second);
        }
        catch (const std::bad_any_cast &)
        {
            LOGFTLF("HttpContext::Get: any type mismatch: %s", key.c_str());
        }
        return *static_cast<const _Tp *>(nullptr); // 不可达, LOGFTL 已 fatal.
    }

    // 设置状态码.
    void Status(int httpRetCode);

    // 生成错误页面
    void GenErrorPage(int httpRetCode);

    // 中止 handler 链.
    void Abort(const std::string &msg = "");

    // 是否在 CGI 打印请求信息日志.
    bool GenLog() const { return m_bLog; }

    // 关闭 CGI 打印请求信息日志.
    void DisableLog() { m_bLog = false; }

  private:
    class HttpRequest *m_ptrReq = 0;
    class HttpResponse *m_ptrRsp = 0;
    void *m_user_defined_data = 0;
    size_t m_iCurHandleIndex = -1;
    bool m_bHasSetRspContent = false;
    bool m_bLog = true; // 获取静态资源等操作无需日志, 以免刷屏.
    std::vector<HttpServer::HandleFunc> m_handles;
    std::unordered_map<std::string, std::string> m_mapParams;
    std::unordered_map<std::string, std::string> m_mapQueryParams;
    std::string m_sClientIP; ///< TCP peer IP (限流等中间件的 key).
    std::unordered_map<std::string, void *> m_mapUserData;
    std::unordered_map<std::string, std::any> m_mapAnyUserData;
    SameSite m_eSameSite = eSameSiteDefault;
};

#define MakeHandler(obj, method)                                              \
    [ptr = &obj, m = &std::decay_t<decltype(obj)>::method]                    \
        (HttpContext *ctx) -> uco::task<void>                                 \
    {                                                                         \
        co_return co_await [](auto *p, auto mm, HttpContext *c)               \
            -> uco::task<void>                                                \
        {                                                                     \
            if constexpr (std::is_same_v<decltype((p->*mm)(c)),               \
                                         uco::task<void>>)                    \
            {                                                                 \
                co_return co_await (p->*mm)(c);                               \
            }                                                                 \
            else                                                              \
            {                                                                 \
                co_return co_await (p->*mm)(c)(c);                            \
            }                                                                 \
        }(ptr, m, ctx);                                                       \
    }

// #define MakeHandler(obj, method)                                              \
//     [ptr = &obj, m = &std::decay_t<decltype(obj)>::method]                    \
//         (HttpContext *ctx) -> uco::task<void>                                 \
//     {                                                                         \
//         co_return co_await (ptr->*m)(ctx);                                    \
//     }
