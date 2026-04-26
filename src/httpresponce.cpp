#include "httpresponce.h"
#include "httprender.h"
#include "ubuffer.h"
#include "uio.h"
#include "ulog.h"
#include "usync.h"
#include <google/protobuf/message.h>
#include <memory>
#include <string>

std::unordered_map<int, std::string> httpRetCode2StatusString = {
    // 1xx Informational
    {100, "Continue"},
    {101, "Switching Protocols"},
    {102, "Processing"},
    {103, "Early Hints"},

    // 2xx Success
    {200, "OK"},
    {201, "Created"},
    {202, "Accepted"},
    {203, "Non-Authoritative Information"},
    {204, "No Content"},
    {205, "Reset Content"},
    {206, "Partial Content"},
    {207, "Multi-Status"},
    {208, "Already Reported"},
    {226, "IM Used"},

    // 3xx Redirection
    {300, "Multiple Choices"},
    {301, "Moved Permanently"},
    {302, "Found"},
    {303, "See Other"},
    {304, "Not Modified"},
    {305, "Use Proxy"},
    {307, "Temporary Redirect"},
    {308, "Permanent Redirect"},

    // 4xx Client Errors
    {400, "Bad Request"},
    {401, "Unauthorized"},
    {402, "Payment Required"},
    {403, "Forbidden"},
    {404, "Not Found"},
    {405, "Method Not Allowed"},
    {406, "Not Acceptable"},
    {407, "Proxy Authentication Required"},
    {408, "Request Timeout"},
    {409, "Conflict"},
    {410, "Gone"},
    {411, "Length Required"},
    {412, "Precondition Failed"},
    {413, "Payload Too Large"},
    {414, "URI Too Long"},
    {415, "Unsupported Media Type"},
    {416, "Range Not Satisfiable"},
    {417, "Expectation Failed"},
    {418, "I'm a teapot"}, // RFC 2324 April Fools' joke ☕
    {421, "Misdirected Request"},
    {422, "Unprocessable Entity"},
    {423, "Locked"},
    {424, "Failed Dependency"},
    {425, "Too Early"},
    {426, "Upgrade Required"},
    {428, "Precondition Required"},
    {429, "Too Many Requests"},
    {431, "Request Header Fields Too Large"},
    {451, "Unavailable For Legal Reasons"},

    // 5xx Server Errors
    {500, "Internal Server Error"},
    {501, "Not Implemented"},
    {502, "Bad Gateway"},
    {503, "Service Unavailable"},
    {504, "Gateway Timeout"},
    {505, "HTTP Version Not Supported"},
    {506, "Variant Also Negotiates"},
    {507, "Insufficient Storage"},
    {508, "Loop Detected"},
    {510, "Not Extended"},
    {511, "Network Authentication Required"},
};

std::unordered_map<std::string, std::string> pathSuffix2FileType = {
    {".html", "text/html"},
    {".htm", "text/html"},
    {".xml", "text/xml"},
    {".xhtml", "application/xhtml+xml"},
    {".txt", "text/plain"},
    {".rtf", "application/rtf"},
    {".pdf", "application/pdf"},
    {".doc", "application/msword"},
    {".docx",
     "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".xls", "application/vnd.ms-excel"},
    {".xlsx",
     "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {".ppt", "application/vnd.ms-powerpoint"},
    {".pptx", "application/"
              "vnd.openxmlformats-officedocument.presentationml.presentation"},
    {".png", "image/png"},
    {".gif", "image/gif"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".bmp", "image/bmp"},
    {".svg", "image/svg+xml"},
    {".ico", "image/x-icon"},
    {".webp", "image/webp"},
    {".mp3", "audio/mpeg"},
    {".wav", "audio/wav"},
    {".ogg", "audio/ogg"},
    {".oga", "audio/ogg"},
    {".mid", "audio/midi"},
    {".midi", "audio/midi"},
    {".amr", "audio/amr"},
    {".mp4", "video/mp4"},
    {".mpeg", "video/mpeg"},
    {".mpg", "video/mpeg"},
    {".mov", "video/quicktime"},
    {".avi", "video/x-msvideo"},
    {".wmv", "video/x-ms-wmv"},
    {".flv", "video/x-flv"},
    {".webm", "video/webm"},
    {".ogv", "video/ogg"},
    {".zip", "application/zip"},
    {".tar", "application/x-tar"},
    {".gz", "application/gzip"},
    {".rar", "application/vnd.rar"},
    {".7z", "application/x-7z-compressed"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".json", "application/json"},
    {".csv", "text/csv"},
    {".tsv", "text/tab-separated-values"},
    {".md", "text/markdown"},
    {".yaml", "application/x-yaml"},
    {".yml", "application/x-yaml"},
    {".exe", "application/octet-stream"},
    {".bin", "application/octet-stream"},
    {".dll", "application/octet-stream"},
    {".deb", "application/vnd.debian.binary-package"},
    {".iso", "application/x-iso9660-image"},
    {".epub", "application/epub+zip"},
};

void HttpResponse::Reset()
{
    m_iHttpRetCode = 0;
    m_pCache.reset();
    m_iPartialFrom = 0;
    m_iPartialTo = 0;
    m_bShouldGenErrorPage = false;
    m_bResourceIsTemplate = false;
    m_httpRspContentType = pathSuffix2FileType[".html"];
    m_vecMoreHeader.clear();
    m_mapTemplateArgs.clear();
    if (m_protoTemplateArgs != nullptr)
    {
        delete m_protoTemplateArgs;
        m_protoTemplateArgs = nullptr;
    }
    m_httpRspStaticResourcePath.clear();
    m_httpRspHeaderBuffer.Reset();
    m_httpRspContentBuffer.Reset();
}

HttpResponse::~HttpResponse()
{
    if (m_protoTemplateArgs != nullptr)
    {
        delete m_protoTemplateArgs;
        m_protoTemplateArgs = nullptr;
    }
}

std::string HttpResponse::GetFileTypeByPath(const std::string &path)
{
    auto index = path.find_last_of('.');
    if (index == std::string::npos)
        return pathSuffix2FileType[".txt"];

    std::string suffix = path.substr(index);
    auto it = pathSuffix2FileType.find(suffix);
    if (it != pathSuffix2FileType.end())
        return it->second;
    else
        return pathSuffix2FileType[".txt"];
}

void HttpResponse::GenHttpHeader()
{
    std::string status = httpRetCode2StatusString[m_iHttpRetCode];
    m_httpRspHeaderBuffer.Append("HTTP/1.1 ");
    m_httpRspHeaderBuffer.Append(std::to_string(m_iHttpRetCode));
    m_httpRspHeaderBuffer.Append(" ");
    m_httpRspHeaderBuffer.Append(status);
    m_httpRspHeaderBuffer.Append("\r\nConnection: ");
    if (m_bKeepAlive == true)
    {
        m_httpRspHeaderBuffer.Append("keep-alive\r\n");
        std::string keepmsg =
            "keep-alive: max=" + std::to_string(m_iKeepAliveCount) +
            ", timeout=" + std::to_string(m_iKeepAliveTime) + "\r\n";
        m_httpRspHeaderBuffer.Append(keepmsg);
    }
    else
    {
        m_httpRspHeaderBuffer.Append("close\r\n");
    }
    for (auto &&[key, value] : m_vecMoreHeader)
    {
        m_httpRspHeaderBuffer.Append(key);
        m_httpRspHeaderBuffer.Append(": ");
        m_httpRspHeaderBuffer.Append(value);
        m_httpRspHeaderBuffer.Append("\r\n");
    }
    m_httpRspHeaderBuffer.Append("Content-type: " + m_httpRspContentType +
                                 "\r\n");

    size_t len = 0;
    // Only one of file and body is allowed.
    if (m_pCache)
    {
        if (m_iPartialFrom != 0 || m_iPartialTo != 0)
        {
            len += (m_iPartialTo - m_iPartialFrom) + 1;
        }
        else
        {
            len += m_pCache->second;
        }
    }
    else
    {
        len += m_httpRspContentBuffer.ReadableBytes();
    }
    m_httpRspHeaderBuffer.Append("Content-length: " + std::to_string(len) +
                                 "\r\n\r\n");
}

uco::task<void> HttpResponse::
GenErrorPage(int httpRetCode, const std::string &fullpath)
{
    m_httpRspStaticResourcePath.clear();
    m_pCache.reset();
    m_httpRspContentType = pathSuffix2FileType[".html"];

    auto cache =
        co_await StaticResourcePool::GetInstance().GetResource(fullpath);
    if (cache->first == -1)
    {
        SYSWRN("html template", fullpath, "not valid");
        GenErrorPageDefault(httpRetCode);
        co_return;
    }
    std::string buffer;
    buffer.resize(cache->second);
    int ret =
        co_await uread(cache->first, buffer.data(), buffer.size(), 0, {10, 0});
    if (ret < 0)
    {
        GenErrorPageDefault(httpRetCode);
        co_return;
    }

    using namespace kainjow::mustache;
    mustache tmpl(buffer);

    data d;
    d.set("code", std::to_string(httpRetCode));
    d.set("message", httpRetCode2StatusString[httpRetCode]);
    m_httpRspContentBuffer.Reset();
    m_httpRspContentBuffer.Append(tmpl.render(d));
}

static kainjow::mustache::data
protoToData(const google::protobuf::Message &proto_msg)
{
    kainjow::mustache::data result;
    const google::protobuf::Reflection *reflection = proto_msg.GetReflection();
    const google::protobuf::Descriptor *descriptor = proto_msg.GetDescriptor();

    // 遍历 Protobuf 消息的所有字段
    for (int i = 0; i < descriptor->field_count(); i++)
    {
        const google::protobuf::FieldDescriptor *field = descriptor->field(i);
        const std::string &field_name = field->name();

        // 处理简单类型字段
        if (field->type() == google::protobuf::FieldDescriptor::TYPE_STRING)
        {
            result.set(field_name, reflection->GetString(proto_msg, field));
        }
        else if (field->type() == google::protobuf::FieldDescriptor::TYPE_INT32)
        {
            result.set(field_name,
                       std::to_string(reflection->GetInt32(proto_msg, field)));
        }
        else if (field->type() == google::protobuf::FieldDescriptor::TYPE_INT64)
        {
            result.set(field_name,
                       std::to_string(reflection->GetInt64(proto_msg, field)));
        }
        else if (field->type() ==
                 google::protobuf::FieldDescriptor::TYPE_UINT32)
        {
            result.set(field_name,
                       std::to_string(reflection->GetUInt32(proto_msg, field)));
        }
        else if (field->type() ==
                 google::protobuf::FieldDescriptor::TYPE_UINT64)
        {
            result.set(field_name,
                       std::to_string(reflection->GetUInt64(proto_msg, field)));
        }
        else if (field->type() == google::protobuf::FieldDescriptor::TYPE_FLOAT)
        {
            result.set(field_name,
                       std::to_string(reflection->GetFloat(proto_msg, field)));
        }
        else if (field->type() ==
                 google::protobuf::FieldDescriptor::TYPE_DOUBLE)
        {
            result.set(field_name,
                       std::to_string(reflection->GetDouble(proto_msg, field)));
        }
        else if (field->type() == google::protobuf::FieldDescriptor::TYPE_BOOL)
        {
            result.set(field_name, reflection->GetBool(proto_msg, field)
                                       ? "true"
                                       : "false");
        }

        else if (field->type() ==
                 google::protobuf::FieldDescriptor::TYPE_MESSAGE)
        {
            if (field->is_repeated())
            {
                kainjow::mustache::data list_data(
                    kainjow::mustache::data::type::list);
                for (int j = 0; j < reflection->FieldSize(proto_msg, field);
                     ++j)
                {
                    const google::protobuf::Message &repeated_msg =
                        reflection->GetRepeatedMessage(proto_msg, field, j);
                    list_data.push_back(protoToData(repeated_msg));
                }
                result.set(field_name, list_data);
            }
            else
            {
                const google::protobuf::Message &nested_msg =
                    reflection->GetMessage(proto_msg, field);
                result.set(field_name, protoToData(nested_msg));
            }
        }
    }

    return result;
}

uco::task<bool> HttpResponse::GenHtmlTemplate(int fd, int size)
{
    std::string buffer;
    buffer.resize(size);
    int ret = co_await uread(fd, buffer.data(), buffer.size(), 0, {10, 0});
    if (ret < 0)
    {
        co_return false;
    }

    kainjow::mustache::mustache tmpl(buffer);
    kainjow::mustache::data d;
    if (m_protoTemplateArgs != nullptr)
    {
        d = protoToData(*m_protoTemplateArgs);
    }
    else
    {
        for (auto &&[k, v] : m_mapTemplateArgs)
        {
            d.set(k, v);
        }
    }
    m_httpRspContentBuffer.Reset();
    m_httpRspContentBuffer.Append(tmpl.render(d));
    co_return true;
}

void HttpResponse::GenErrorPageDefault(int httpRetCode)
{
    m_httpRspStaticResourcePath.clear();
    m_pCache.reset();
    m_iHttpRetCode = httpRetCode;
    std::string body;
    std::string stat = httpRetCode2StatusString[m_iHttpRetCode];
    body.reserve(1024);
    body += "<html><title>Error</title>";
    body += "<body bgcolor=\"ffffff\">";
    body += std::to_string(m_iHttpRetCode) + " - " + stat;
    body += "<hr><em>TinyWebServer</em></body></html>";
    m_httpRspContentBuffer.Reset();
    m_httpRspContentBuffer.Append(body);
    m_httpRspContentType = pathSuffix2FileType[".html"];
}

std::string HttpResponse::PathSuffix2FileType(const std::string &suffix)
{
    auto it = pathSuffix2FileType.find(suffix);
    if (it != pathSuffix2FileType.end())
    {
        return it->second;
    }
    return pathSuffix2FileType[".txt"];
}

void HttpResponse::AddHeader(const std::string &key, const std::string &value)
{
    m_vecMoreHeader.emplace_back(key, value);
}

StaticResourcePool::FileCacheI::~FileCacheI()
{
    if (first > 0)
    {
        LOGDBG("close fd: %d", first);
        go uclose(first);
    }
    first = -1;
}

StaticResourcePool::StaticResourcePool() : m_mapResource(512) {}

StaticResourcePool::~StaticResourcePool() {}

uco::task<StaticResourcePool::FileCache>
StaticResourcePool::GetResource(const std::string &path)
{
    FileCache result;
    co_await m_mtx.lock();
    auto it = m_mapResource.get(path);
    if (it->first != -1)
    {
        result = it;
        m_mtx.unlock();
        co_return result;
    }

    auto call_it = m_mapInFlight.find(path);
    if (call_it != m_mapInFlight.end())
    {
        auto call = call_it->second;
        m_mtx.unlock();

        LOGDBG("mapInFlight:", path.c_str());

        auto lock = co_await unique_ulock(call->mu);
        co_await call->cv.wait(lock, [&call] { return call->done; });
        result = call->result;
        co_return result;
    }

    auto call = std::make_shared<Call>();
    m_mapInFlight[path] = call;
    m_mtx.unlock();

    result = co_await do_open(path);

    if (result->first != -1)
    {
        co_await m_mtx.lock();
        auto exist_cache = m_mapResource.put(path, result);
        if (exist_cache->first != -1)
        {
            // Another coroutine beat us to it, close our fd.
            LOGERR("duplicate open: %s", path.c_str());
            result = exist_cache;
        }
        m_mtx.unlock();
    }

    co_await call->mu.lock();
    call->result = result;
    call->done = true;
    call->cv.notify_all();
    call->mu.unlock();

    co_await m_mtx.lock();
    m_mapInFlight.erase(path);
    m_mtx.unlock();

    co_return result;
}

uco::task<StaticResourcePool::FileCache>
StaticResourcePool::do_open(const std::string &path)
{
    FileCache cache = std::make_shared<StaticResourcePool::FileCacheI>();

    // Open not in do while bacause fd not need to close.
    int fd = co_await uopen(path.c_str(), O_RDONLY);
    if (fd < 0)
    {
        cache->first = -1;
        cache->second = 404;
        co_return cache;
    }

    struct statx file_stat;
    file_stat.stx_mode = 0;
    file_stat.stx_size = 0;
    do
    {
        int ret = co_await ufstat(fd, &file_stat);
        if (ret < 0 || S_ISDIR(file_stat.stx_mode))
        {
            cache->first = -1;
            cache->second = 404;
            break;
        }

        if (!(file_stat.stx_mode & S_IROTH))
        {
            cache->first = -1;
            cache->second = 403;
            break;
        }
        cache->first = fd;
        cache->second = file_stat.stx_size;
        co_return cache;
    } while (0);

    co_await uclose(fd);
    co_return cache;
}
