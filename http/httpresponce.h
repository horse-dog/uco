#pragma once

#include <cstdio>
#include <cstring>
#include <google/protobuf/message.h>
#include <list>
#include <memory>
#include <string>
#include <sys/eventfd.h>
#include <unordered_map>

#include <fcntl.h>    // open
#include <sys/mman.h> // mmap, munmap
#include <sys/stat.h> // stat
#include <unistd.h>   // close

#include "core/uco.h"
#include "core/ulog.h"
#include "core/usync.h"
#include "core/ubuffer.h"

// http status code -> reason phrase (defined in httpresponce.cpp)
extern std::unordered_map<int, std::string> httpRetCode2StatusString;

class StaticResourcePool
{
  public:
    struct FileCacheI
    {
        ~FileCacheI();

        int first = -1; // fd.
        int second = 0; // file size.

        bool vaild() const { return first >= 0; }
    };

    using FileCache = std::shared_ptr<FileCacheI>;

    static StaticResourcePool &GetInstance()
    {
        static StaticResourcePool ins;
        return ins;
    }

    uco::task<FileCache> GetResource(const std::string &path);

  private:
    class LRUCache
    {
      protected:
        using iter = std::list<std::pair<std::string, FileCache>>::iterator;
        int cap = 0;
        std::unordered_map<std::string, iter> cache;
        std::list<std::pair<std::string, FileCache>> lru;

      public:
        LRUCache(int capacity) { this->cap = capacity; }

        ~LRUCache()
        {
            for (auto &&[key, cache] : lru)
            {
                close(cache->first);
                cache->first = -1;
            }
        }

        FileCache get(const std::string &key)
        {
            auto it = this->cache.find(key);
            if (it == cache.end())
                return std::make_shared<FileCacheI>();
            auto iter = it->second;
            auto result = iter->second;
            lru.splice(lru.begin(), lru, iter);
            return result;
        }

        FileCache put(const std::string &key, FileCache value)
        {
            auto it = this->cache.find(key);
            if (it == cache.end())
            {
                if (lru.size() == this->cap)
                {
                    auto back = lru.back();
                    LOGDBG("expired:", back.first, "fd:", back.second->first, "size:", back.second->second, 
                           "use_count:", back.second.use_count());
                    cache.erase(back.first);
                    lru.pop_back();
                }
                lru.emplace_front(key, value);
                cache[key] = lru.begin();
            }
            else
            {
                auto iter = it->second;
                // iter->second = value;
                lru.splice(lru.begin(), lru, iter);
                return iter->second;
            }
            return std::make_shared<FileCacheI>();
        }
    };

    StaticResourcePool();
    ~StaticResourcePool();

    uco::task<FileCache> do_open(const std::string &path);

    struct Call
    {
        uco::umutex mu;
        uco::ucond cv;
        bool done = false;
        FileCache result;
    };

    uco::umutex m_mtx;
    LRUCache m_mapResource;
    std::unordered_map<std::string, std::shared_ptr<Call>> m_mapInFlight;
};

class HttpResponse
{
  public:
    HttpResponse(int sendtimeout, int keepalivecount, int keepalivesec)
        : m_iSendTimeoutSec(sendtimeout), 
          m_iKeepAliveCount(keepalivecount), 
          m_iKeepAliveTime(keepalivesec)
    {
        Reset();
    }
    ~HttpResponse();
    static std::string GetFileTypeByPath(const std::string &path);

  private:
    void SetHttpRetCode(int code) { m_iHttpRetCode = code; }
    int GetHttpRetCode() const { return m_iHttpRetCode; }
    void SetKeepAlive(bool keep_alive) { m_bKeepAlive = keep_alive; }
    bool IsKeepAlive() const { return m_bKeepAlive; }
    void Reset();
    // 覆盖写响应体前清理 body 相关字段 (保留 headers/RetCode).
    void ResetContent();
    void GenHttpHeader();
    void ShouldGenErrorPage(int httpRetCode);
    uco::task<bool> GenHtmlTemplate(int fd, int size);
    uco::task<void> GenErrorPage(int httpRetCode, const std::string &fullpath);
    void GenErrorPageDefault(int httpRetCode);

    void AddHeader(const std::string &key, const std::string &value);

    void write(const std::string &__msg)
    {
        m_httpRspContentBuffer.Append(__msg);
    }

    static std::string PathSuffix2FileType(const std::string &suffix);

  private:
    friend class HttpContext;
    friend class HttpConnection;
    int m_iHttpRetCode = 0;
    int m_iPartialFrom = 0;
    int m_iPartialTo = 0;
    int m_iCurrentReqCount = 0;
    int m_iSendTimeoutSec = 0;
    int m_iKeepAliveCount = 0;
    int m_iKeepAliveTime = 0; // seconds.
    bool m_bKeepAlive = false;
    bool m_bShouldGenErrorPage = false;
    bool m_bResourceIsTemplate = false;
    StaticResourcePool::FileCache m_pCache;
    std::vector<std::pair<std::string, std::string>> m_vecMoreHeader;
    std::unordered_map<std::string, std::string> m_mapTemplateArgs;
    google::protobuf::Message *m_protoTemplateArgs = 0;
    std::string m_httpRspContentType;
    std::string m_httpRspStaticResourcePath;
    uco::buffer m_httpRspHeaderBuffer;
    uco::buffer m_httpRspContentBuffer;
};
