#define _UCO_THREAD_ENV_IMPL
#include "usql.h"
#include "ulog.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/reflection.h>

#include <mysql/errmsg.h> // CR_SERVER_LOST (客户端错误码 2013)

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <functional>
#include <liburing.h>
#include <poll.h>
#include <sys/socket.h>
#include <utility>

#if USE_MYSQL_DBG
#define MYSQL_DBG SYSDBG
#else
#define MYSQL_DBG(...)
#endif

#define UCOENV uco::__inner__::thread_co_env::GetInstance()
#define WAIT_SQE_LIST                                                        \
    ((uco::__inner__::uco_linked_list *)(UCOENV.wait_sqe_list))

// 抹平 MySQL (直接返回 net_async_status) 与
// MariaDB (末尾出参返回状态) 的非阻塞接口签名差异.
#if defined(MARIADB_VERSION_ID) || defined(MARIADB_BASE_VERSION)
#define USQL_NB_CALL(fn, ...)                                                  \
    ({                                                                         \
        net_async_status __usql_nb_st = NET_ASYNC_COMPLETE;                    \
        (fn)(__VA_ARGS__, &__usql_nb_st) != 0 ? NET_ASYNC_ERROR                \
                                              : __usql_nb_st;                  \
    })
#else
#define USQL_NB_CALL(fn, ...) ((fn)(__VA_ARGS__))
#endif

namespace usql
{

// ==================== 内部实现 ====================

/// 连接对应的 fd.
static int get_mysql_fd(MYSQL *mysql) { return (int)mysql->net.fd; }

/// 距 begin 的毫秒数.
static int64_t elapsed_ms(const std::chrono::steady_clock::time_point &begin)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - begin)
        .count();
}

/// 填充错误信息并集中记录日志 (r 为 await_call 的返回值).
static void fill_error(SqlError &e, MYSQL *mysql, int r, const char *op,
                       int64_t elapsed)
{
    if (r == -ETIME)
    {
        e.ret_code = kTimeout;
        e.err_msg = "usql: operation timeout";
        // 超时后流已失步 (服务端响应可能迟到, 错配下一条命令),
        // 连接不可复用; mysql_errno() 读的就是 net.last_errno, 直接
        // 写入客户端错误码, Release 现有的 ">= 2000 销毁" 判定自动
        // 生效, 无需额外机制 (风格同 get_mysql_fd 直访 net.fd).
        mysql->net.last_errno = CR_SERVER_LOST;
        snprintf(mysql->net.last_error, sizeof(mysql->net.last_error),
                 "usql: operation timeout, stream desynced");
        SYSWRN("usql:", op, "timeout, elapsed_ms:", elapsed);
        return;
    }
    e.ret_code = -(int)mysql_errno(mysql); // MySQL 原码取负 (见 UsqlError).
    e.err_msg = mysql_error(mysql);
    e.sqlstate = mysql_sqlstate(mysql);
    SYSERR("usql:", op, "failed, errno:", e.ret_code, "msg:", e.err_msg,
           "sqlstate:", e.sqlstate, "elapsed_ms:", elapsed);
}

bool SqlError::Retryable() const
{
    return ret_code == kTimeout ||
           (ret_code <= -2000 && ret_code > -3000) || // MySQL 客户端连接类.
           ret_code == kLockDeadlock || ret_code == kLockWaitTimeout;
}

/// io_uring poll_add 监听 fd 事件.
/// 超时控制: IOSQE_IO_LINK + link_timeout, 归一化为 -ETIME;
/// sqe 获取失败时挂入 wait_sqe_list 由调度器重试 (参考 uio.cpp).
static uco::task<int> wait_fd(int fd, int events, uco_time_t ts)
{
    struct awaitable
    {
        int fd = -1;
        int events = 0;
        uco_time_t ts = {0, 0};
        uco::task<int>::promise_type *p = 0;

        auto await_ready() const noexcept { return false; }

        auto await_suspend(uco::task<int>::coro_handle h) noexcept
        {
            MYSQL_DBG("await_suspend: poll_add, fd:", fd,
                      ", events:", events, ", handle:", h.address());
            auto &&prom = h.promise();
            this->p = &prom;
            auto sqe = UCOENV.get_sqe();
            if (sqe == nullptr)
            {
                this->p->set_value(-EAGAIN);
                WAIT_SQE_LIST->push(
                    (uco::task<void>::promise_type *)(&(h.promise())));
                return true;
            }
            io_uring_prep_poll_add(sqe, fd, events);
            sqe->user_data = (uint64_t)(h.address());
            if (ts.tv_sec != 0 || ts.tv_nsec != 0)
            {
                sqe->flags |= IOSQE_IO_LINK;
                auto sqe1 = UCOENV.get_sqe();
                if (sqe1 == nullptr)
                {   // 主 sqe 置为 nop, 挂起重试.
                    sqe->user_data = 0;
                    io_uring_prep_nop(sqe);
                    this->p->set_value(-EAGAIN);
                    WAIT_SQE_LIST->push(
                        (uco::task<void>::promise_type *)(&(h.promise())));
                    return true;
                }
                io_uring_prep_link_timeout(sqe1, &ts, 0);
                sqe1->user_data = 0;
            }
            return true;
        }

        auto await_resume() noexcept
        {
            MYSQL_DBG("await_resume: poll_add, res:", p->value());
            return (int)(p->value());
        }
    };

    int res = 0;
    do
    {
        res = co_await awaitable{fd, events, ts};
    } while (res == -EAGAIN || res == -EINTR);

    if (res == -ECANCELED)
    { // link_timeout 触发, 归一化为超时.
        res = -ETIME;
    }
    co_return res;
}

/// 推断等待方向. MySQL 的 *_nonblocking 接口返回 NET_ASYNC_NOT_READY 时
/// 不携带读写方向信息 (无 MariaDB _start/_cont API 的 MYSQL_WAIT_READ/
/// WRITE 对应物, 公开 NET 结构亦无 async 状态), 不能无脑 POLLIN | POLLOUT:
/// TCP socket 几乎恒可写, level-triggered poll 会因 POLLOUT 立即返回,
/// await_call 循环空转 (表现为刷屏 wait 日志).
/// 推断依据: 调用返回 NOT_READY 时, 若库阻塞在写, 其内部 write 必然刚
/// 遭遇 EAGAIN (发送缓冲满), socket 不可写; 若 socket 仍可写, 则库必然
/// 阻塞在读. 故以 0 超时 poll 探测 POLLOUT 决定等待方向.
static int infer_wait_events(int fd)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) != 1)
    { // 探测异常, 保守退回双向等待.
        return POLLIN | POLLOUT;
    }
    return (pfd.revents & POLLOUT) ? POLLIN : POLLOUT;
}

/// 推进一次 mysql *_nonblocking 调用直至完成.
/// op 为日志标识 ("connect" / "select.query" 等).
/// 返回 0 完成; -ETIME 超时; -EIO mysql 报错.
static uco::task<int> await_call(MYSQL *mysql, const char *op,
                                 std::function<net_async_status()> call,
                                 uco_time_t ts)
{
    using namespace std::chrono;

    const bool has_deadline = (ts.tv_sec != 0 || ts.tv_nsec != 0);
    const int64_t total_ns =
        (int64_t)ts.tv_sec * 1'000'000'000 + ts.tv_nsec;
    const auto begin = steady_clock::now();

    while (true)
    {
        net_async_status status = call();
        if (status == NET_ASYNC_COMPLETE ||
            status == NET_ASYNC_COMPLETE_NO_MORE_RESULTS)
        {
            MYSQL_DBG("usql:", op, "done, fd:", get_mysql_fd(mysql),
                      "elapsed_ms:",
                   duration_cast<milliseconds>(steady_clock::now() - begin)
                       .count());
            co_return 0;
        }
        if (status == NET_ASYNC_ERROR)
        {
            SYSERR("usql:", op, "async error, fd:", get_mysql_fd(mysql),
                   "errno:", mysql_errno(mysql),
                   "msg:", mysql_error(mysql));
            co_return -EIO;
        }

        uco_time_t ts_wait = ts;
        if (has_deadline)
        {
            int64_t remain =
                total_ns -
                duration_cast<nanoseconds>(steady_clock::now() - begin)
                    .count();
            if (remain <= 0)
            {
                co_return -ETIME;
            }
            ts_wait.tv_sec =
                (decltype(ts_wait.tv_sec))(remain / 1'000'000'000);
            ts_wait.tv_nsec =
                (decltype(ts_wait.tv_nsec))(remain % 1'000'000'000);
        }

        int fd = get_mysql_fd(mysql);
        if (fd <= 0)
        { // 尚无有效 fd (如 DNS 解析阶段), 短暂休眠后重试.
            co_await uco_nanosleep(0, 1'000'000);
            continue;
        }

        int events = infer_wait_events(fd);
        LOGDBG("usql:", op, "wait, fd:", fd, "events:", events);
        int r = co_await wait_fd(fd, events, ts_wait);
        if (r < 0)
        {
            if (r != -ETIME)
            { // poll 本身失败 (非超时): 此刻 mysql_errno 多半还是 0,
              // 单独记录 io_uring 侧返回值以便定位.
                SYSERR("usql:", op, "wait fd failed, fd:", fd, "res:", r,
                       "errno:", mysql_errno(mysql),
                       "msg:", mysql_error(mysql));
            }
            co_return r;
        }
    }
}

// ==================== 连接 ====================

uco::task<MYSQL *> uconnect(const char *host, const char *user,
                             const char *pass, const char *db,
                             unsigned int port, uco_time_t ts, bool use_ssl)
{
    MYSQL *mysql = mysql_init(nullptr);
    if (mysql == nullptr)
    {
        SYSERR("usql: mysql_init failed");
        co_return nullptr;
    }

    // TLS 默认关: libmysqlclient 默认 ssl-mode=PREFERRED, 每连接完整
    // TLS 握手 (CA 加载 + 证书验证 + 密钥计算) 在少核机器上可达百 ms
    // 且并发连接会排队放大; 本地/内网无加密必要.
    enum mysql_ssl_mode ssl_mode =
        use_ssl ? SSL_MODE_REQUIRED : SSL_MODE_DISABLED;
    mysql_options(mysql, MYSQL_OPT_SSL_MODE, &ssl_mode);

    LOGMSG("usql: connect, host:", host, "port:", port, "user:", user,
           "db:", db);
    const auto begin = std::chrono::steady_clock::now();
    int r = co_await await_call(
        mysql,
        "connect",
        [&] {
            return USQL_NB_CALL(mysql_real_connect_nonblocking, mysql, host,
                                user, pass, db, port, nullptr, 0);
        },
        ts);
    if (r != 0)
    {
        SYSERR("usql: connect failed, ret:", r, "host:", host,
               "port:", port, "errno:", mysql_errno(mysql),
               "msg:", mysql_error(mysql),
               "elapsed_ms:", elapsed_ms(begin));
        mysql_close(mysql);
        co_return nullptr;
    }
    LOGMSG("usql: connect ok, fd:", get_mysql_fd(mysql),
           "server:", mysql_get_server_info(mysql),
           "elapsed_ms:", elapsed_ms(begin));
    co_return mysql;
}

void uclose(MYSQL *mysql)
{
    if (mysql == nullptr)
    {
        return;
    }
    const int fd = get_mysql_fd(mysql);
    MYSQL_DBG("usql: close, fd:", fd);
    if (fd > 0)
    {
        // 兜底: 发送缓冲仅剩少量空间的极端场景下, QUIT 部分写的
        // 补发循环至多阻塞 1ms.
        struct timeval tv = {0, 1000}; // 1ms.
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        // 探测发送缓冲: 可写则 mysql_close 内的 COM_QUIT 能立即发出,
        // 优雅收尾(服务器不计 Aborted_clients); 不可写(缓冲满/对端
        // 假死)则 shutdown 暴力关闭, mysql_close 退化为本地操作,
        // 不阻塞调度线程.
        struct pollfd pfd = {fd, POLLOUT, 0};
        if (poll(&pfd, 1, 0) != 1 || !(pfd.revents & POLLOUT))
        {
            LOGWRN("usql: send buffer full on close, fd:", fd,
                   "peer likely dead, force close");
            shutdown(fd, SHUT_RDWR);
        }
    }
    mysql_close(mysql);
}

std::string uescape(MYSQL *mysql, const std::string &s)
{
    if (mysql == nullptr)
    {
        // 无法安全转义: 返回空串 (宁可数据被拒, 不可漏转义致注入).
        SYSERR("usql: uescape: mysql is nullptr");
        return {};
    }
    std::string out(s.size() * 2 + 1, '\0');
    unsigned long n = mysql_real_escape_string(mysql, out.data(), s.c_str(),
                                               (unsigned long)s.size());
    out.resize(n);
    return out;
}

// ==================== 增删改查 ====================

uco::task<InsertResult> uinsert(MYSQL *mysql, std::string sql,
                                 uco_time_t ts)
{
    InsertResult result;
    if (mysql == nullptr)
    {
        result.ret_code = kBadArgument;
        result.err_msg = "usql: insert: mysql is nullptr";
        co_return result;
    }
    const auto begin = std::chrono::steady_clock::now();
    LOGMSG("usql: insert, fd:", get_mysql_fd(mysql));
    int r = co_await await_call(
        mysql,
        "insert",
        [&] {
            return USQL_NB_CALL(mysql_real_query_nonblocking, mysql,
                                sql.c_str(), (unsigned long)sql.size());
        },
        ts);
    if (r != 0)
    {
        fill_error(result, mysql, r, "insert", elapsed_ms(begin));
        co_return result;
    }
    result.insert_id = mysql_insert_id(mysql);
    result.affected_rows = mysql_affected_rows(mysql);
    LOGMSG("usql: insert ok, fd:", get_mysql_fd(mysql),
           "insert_id:", result.insert_id,
           "affected_rows:", result.affected_rows,
           "elapsed_ms:", elapsed_ms(begin));
    co_return result;
}

uco::task<AffectedResult> uupdate(MYSQL *mysql, std::string sql,
                                  uco_time_t ts)
{
    AffectedResult result;
    if (mysql == nullptr)
    {
        result.ret_code = kBadArgument;
        result.err_msg = "usql: update: mysql is nullptr";
        co_return result;
    }
    const auto begin = std::chrono::steady_clock::now();
    LOGMSG("usql: update, fd:", get_mysql_fd(mysql));
    int r = co_await await_call(
        mysql,
        "update",
        [&] {
            return USQL_NB_CALL(mysql_real_query_nonblocking, mysql,
                                sql.c_str(), (unsigned long)sql.size());
        },
        ts);
    if (r != 0)
    {
        fill_error(result, mysql, r, "update", elapsed_ms(begin));
        co_return result;
    }
    result.affected_rows = mysql_affected_rows(mysql);
    LOGMSG("usql: update ok, fd:", get_mysql_fd(mysql),
           "affected_rows:", result.affected_rows,
           "elapsed_ms:", elapsed_ms(begin));
    co_return result;
}

uco::task<AffectedResult> udelete(MYSQL *mysql, std::string sql,
                                  uco_time_t ts)
{
    AffectedResult result;
    if (mysql == nullptr)
    {
        result.ret_code = kBadArgument;
        result.err_msg = "usql: delete: mysql is nullptr";
        co_return result;
    }
    const auto begin = std::chrono::steady_clock::now();
    LOGMSG("usql: delete, fd:", get_mysql_fd(mysql));
    int r = co_await await_call(
        mysql,
        "delete",
        [&] {
            return USQL_NB_CALL(mysql_real_query_nonblocking, mysql,
                                sql.c_str(), (unsigned long)sql.size());
        },
        ts);
    if (r != 0)
    {
        fill_error(result, mysql, r, "delete", elapsed_ms(begin));
        co_return result;
    }
    result.affected_rows = mysql_affected_rows(mysql);
    LOGMSG("usql: delete ok, fd:", get_mysql_fd(mysql),
           "affected_rows:", result.affected_rows,
           "elapsed_ms:", elapsed_ms(begin));
    co_return result;
}

uco::task<SelectResult> uselect(MYSQL *mysql, std::string sql,
                                uco_time_t ts)
{
    SelectResult ret;
    if (mysql == nullptr)
    {
        ret.ret_code = kBadArgument;
        ret.err_msg = "usql: select: mysql is nullptr";
        co_return ret;
    }
    const auto begin = std::chrono::steady_clock::now();
    LOGDBG("usql: select, fd:", get_mysql_fd(mysql));

    // 1. 发送请求.
    int r = co_await await_call(
        mysql,
        "select.query",
        [&] {
            return USQL_NB_CALL(mysql_real_query_nonblocking, mysql,
                                sql.c_str(), (unsigned long)sql.size());
        },
        ts);
    if (r != 0)
    {
        fill_error(ret, mysql, r, "select.query", elapsed_ms(begin));
        co_return ret;
    }

    if (mysql_field_count(mysql) == 0)
    {
        ret.ret_code = kBadArgument;
        ret.err_msg = "usql: no result set (not a SELECT?)";
        SYSERR("usql: select no result set, fd:", get_mysql_fd(mysql));
        co_return ret;
    }

    // 2. 拉取结果集到本地内存.
    MYSQL_RES *result = nullptr;
    r = co_await await_call(
        mysql,
        "select.store",
        [&] {
            return USQL_NB_CALL(mysql_store_result_nonblocking, mysql,
                                &result);
        },
        ts);
    if (r != 0 || result == nullptr)
    {
        fill_error(ret, mysql, (r != 0) ? r : -EIO, "select.store",
                   elapsed_ms(begin));
        co_return ret;
    }

    // 3. 纯内存遍历.
    ret.num_fields = mysql_num_fields(result);
    ret.num_rows = mysql_num_rows(result);

    MYSQL_FIELD *fields = mysql_fetch_fields(result);
    for (unsigned int i = 0; i < ret.num_fields; i++)
    {
        ret.column_names.emplace_back(fields[i].name);
        ret.column_types.push_back(fields[i].type);
        ret.column_flags.push_back(fields[i].flags);
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result)) != nullptr)
    {
        unsigned long *lengths = mysql_fetch_lengths(result);
        std::vector<std::string> row_data(ret.num_fields);
        for (unsigned int i = 0; i < ret.num_fields; i++)
        {
            if (row[i] != nullptr)
            {
                row_data[i].assign(row[i], lengths[i]);
            }
        }
        ret.rows.push_back(std::move(row_data));
    }

    mysql_free_result(result);
    LOGMSG("usql: select ok, fd:", get_mysql_fd(mysql),
           "rows:", ret.num_rows, "fields:", ret.num_fields,
           "elapsed_ms:", elapsed_ms(begin));
    co_return ret;
}

/// 单元格字符串写入 pb 字段 (反射), 失败抛异常由调用方捕获.
static void set_pb_field(google::protobuf::Message &msg,
                         const google::protobuf::FieldDescriptor *field,
                         const std::string &value)
{
    using namespace google::protobuf;
    const Reflection *refl = msg.GetReflection();
    switch (field->cpp_type())
    {
    case FieldDescriptor::CPPTYPE_INT32:
        refl->SetInt32(&msg, field, (int32_t)std::stoll(value));
        break;
    case FieldDescriptor::CPPTYPE_INT64:
        refl->SetInt64(&msg, field, std::stoll(value));
        break;
    case FieldDescriptor::CPPTYPE_UINT32:
        refl->SetUInt32(&msg, field, (uint32_t)std::stoull(value));
        break;
    case FieldDescriptor::CPPTYPE_UINT64:
        refl->SetUInt64(&msg, field, std::stoull(value));
        break;
    case FieldDescriptor::CPPTYPE_FLOAT:
        refl->SetFloat(&msg, field, std::stof(value));
        break;
    case FieldDescriptor::CPPTYPE_DOUBLE:
        refl->SetDouble(&msg, field, std::stod(value));
        break;
    case FieldDescriptor::CPPTYPE_BOOL:
        refl->SetBool(&msg, field, value == "1" || value == "true" ||
                                       value == "t");
        break;
    case FieldDescriptor::CPPTYPE_ENUM:
    { // 先按名字 (MySQL enum 列返回名字), 再按数值.
        const EnumDescriptor *ed = field->enum_type();
        const EnumValueDescriptor *ev = ed->FindValueByName(value);
        if (ev == nullptr)
        {
            ev = ed->FindValueByNumber((int)std::stoll(value));
        }
        if (ev == nullptr)
        {
            throw std::runtime_error("no such enum value: " + value);
        }
        refl->SetEnum(&msg, field, ev);
        break;
    }
    case FieldDescriptor::CPPTYPE_STRING:
        refl->SetString(&msg, field, value);
        break;
    default:
        throw std::runtime_error("unsupported cpp_type");
    }
}

/// MySQL 列类型与 pb 字段类型是否严格兼容.
static bool field_accepts(enum_field_types t, unsigned int flags,
                          const google::protobuf::FieldDescriptor *f)
{
    using FD = google::protobuf::FieldDescriptor;
    const int cpp = f->cpp_type();
    const bool uns = (flags & UNSIGNED_FLAG) != 0;
    switch (t)
    {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_LONG: // ≤32bit 整数
        if (t == MYSQL_TYPE_TINY && cpp == FD::CPPTYPE_BOOL)
        {
            return true;
        }
        return uns ? (cpp == FD::CPPTYPE_UINT32 || cpp == FD::CPPTYPE_UINT64)
                   : (cpp == FD::CPPTYPE_INT32 || cpp == FD::CPPTYPE_INT64);
    case MYSQL_TYPE_LONGLONG: // BIGINT: 不允许窄化
        return uns ? cpp == FD::CPPTYPE_UINT64 : cpp == FD::CPPTYPE_INT64;
    case MYSQL_TYPE_FLOAT:
        return cpp == FD::CPPTYPE_FLOAT || cpp == FD::CPPTYPE_DOUBLE;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_DOUBLE:
        return cpp == FD::CPPTYPE_DOUBLE;
    case MYSQL_TYPE_ENUM:
        return cpp == FD::CPPTYPE_STRING || cpp == FD::CPPTYPE_ENUM;
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_JSON:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_GEOMETRY:
        return cpp == FD::CPPTYPE_STRING;
    default:
        return false;
    }
}

/// 严格对齐校验: 每一列必须有同名字段且类型兼容 (空结果集同样校验).
bool SelectResult::check_alignment(
    const google::protobuf::Descriptor *desc) const
{
    using FD = google::protobuf::FieldDescriptor;
    for (size_t i = 0; i < column_names.size(); i++)
    {
        const auto &name = column_names[i];
        const google::protobuf::FieldDescriptor *field =
            desc->FindFieldByName(name);
        if (field == nullptr)
        {
            SYSERR("usql: strict: column", NR(name),
                   "has no matching proto field");
            return false;
        }
        if (field->is_repeated() || field->cpp_type() == FD::CPPTYPE_MESSAGE)
        {
            SYSERR("usql: strict: field", NR(name),
                   "is repeated/message, unsupported");
            return false;
        }
        if (!field_accepts(column_types[i], column_flags[i], field))
        {
            SYSERR("usql: strict: column", NR(name),
                   "type mismatch with proto field");
            return false;
        }
    }
    return true;
}

/// 单行值绑定到 pb 对象 (对齐已由 check_alignment 校验).
bool SelectResult::bind_row(size_t row_idx,
                            google::protobuf::Message &out) const
{
    const auto &row = rows[row_idx];
    const google::protobuf::Descriptor *desc = out.GetDescriptor();
    for (size_t i = 0; i < column_names.size() && i < row.size(); i++)
    {
        const auto &name = column_names[i];
        const google::protobuf::FieldDescriptor *field =
            desc->FindFieldByName(name);
        try
        {
            set_pb_field(out, field, row[i]);
        }
        catch (const std::exception &e)
        {
            SYSERR("usql: strict: bind column", NR(name),
                   "failed:", e.what());
            return false;
        }
    }
    return true;
}

uco::task<SelectResult> uselect(MYSQL *mysql, std::string sql,
                                google::protobuf::Message *out,
                                uco_time_t ts)
{
    // 参数校验先于任何 IO (空 out 不该白跑一次查询).
    if (out == nullptr)
    {
        SelectResult ret;
        ret.ret_code = kBadArgument;
        ret.err_msg = "usql: uselect: out is nullptr";
        co_return ret;
    }
    SelectResult ret = co_await uselect(mysql, std::move(sql), ts);
    if (ret.ret_code != 0)
    {
        co_return ret; // 执行失败, 原样带错误信息.
    }

    if (!ret.check_alignment(out->GetDescriptor()))
    {
        // 对齐失败详情已由 check_alignment 记 SYSERR.
        ret.ret_code = kBadArgument;
        ret.err_msg = "usql: uselect: columns not aligned with proto fields";
        co_return ret;
    }

    if (!ret.rows.empty())
    {
        out->Clear();
        if (!ret.bind_row(0, *out))
        {
            // 绑定失败详情已由 bind_row 记 SYSERR.
            ret.ret_code = kBadArgument;
            ret.err_msg = "usql: uselect: bind row failed";
            co_return ret;
        }
        // 数据已转入 out, 释放原始行, 避免双份快照
        // (num_rows 保留, 供三态判断: 0=无行, >=1=已绑定).
        ret.rows.clear();
        ret.rows.shrink_to_fit();
    }
    co_return ret;
}

// ==================== 事务 ====================

namespace __inner__
{
/// 事务内执行一条语句, 若产生结果集则顺便清空,
/// 避免后续语句报 CR_COMMANDS_OUT_OF_SYNC.
/// 注: 语句计划由 utransaction 入口统一打印, 此处不再分散打印.
static uco::task<int> txn_query(MYSQL *mysql, const std::string &sql,
                                  uco_time_t ts)
{
    int r = co_await await_call(
        mysql,
        "txn.query",
        [&] {
            return USQL_NB_CALL(mysql_real_query_nonblocking, mysql,
                                sql.c_str(), (unsigned long)sql.size());
        },
        ts);
    if (r != 0)
    {
        co_return r;
    }

    if (mysql_field_count(mysql) > 0)
    {
        MYSQL_RES *res = nullptr;
        r = co_await await_call(
            mysql,
            "txn.store",
            [&] {
                return USQL_NB_CALL(mysql_store_result_nonblocking, mysql,
                                    &res);
            },
            ts);
        if (res != nullptr)
        {
            mysql_free_result(res);
        }
    }
    co_return r;
}
} // namespace __inner__

uco::task<TxnResult> utransaction(MYSQL *mysql, std::vector<std::string> sqls,
                                    uco_time_t ts)
{
    TxnResult txn;
    if (mysql == nullptr)
    {
        txn.ret_code = kBadArgument;
        txn.err_msg = "usql: transaction: mysql is nullptr";
        co_return txn;
    }
    const auto begin = std::chrono::steady_clock::now();

    LOGMSG("usql: txn begin, fd:", get_mysql_fd(mysql),
           "stmts:", sqls.size());

    // BEGIN.
    int r = co_await __inner__::txn_query(mysql, "BEGIN", ts);
    if (r != 0)
    {
        fill_error(txn, mysql, r, "txn.begin", elapsed_ms(begin));
        co_return txn;
    }

    // 逐条执行.
    for (const auto &sql : sqls)
    {
        StmtResult sr;
        r = co_await __inner__::txn_query(mysql, sql, ts);
        if (r != 0)
        {
            sr.ok = false;
            fill_error(txn, mysql, r, "txn.stmt", elapsed_ms(begin));
            txn.stmt_results.push_back(std::move(sr));
            SYSWRN("usql: txn stmt failed, rollback");
            co_await __inner__::txn_query(mysql, "ROLLBACK", ts);
            co_return txn;
        }

        sr.affected_rows = mysql_affected_rows(mysql);
        sr.insert_id = mysql_insert_id(mysql);
        sr.warnings = mysql_warning_count(mysql);
        txn.stmt_results.push_back(std::move(sr));
    }

    // COMMIT.
    r = co_await __inner__::txn_query(mysql, "COMMIT", ts);
    if (r != 0)
    {
        fill_error(txn, mysql, r, "txn.commit", elapsed_ms(begin));
        LOGWRN("usql: txn commit failed, rollback");
        co_await __inner__::txn_query(mysql, "ROLLBACK", ts);
        co_return txn;
    }

    txn.committed = true;
    LOGMSG("usql: txn committed, stmts:", sqls.size(),
           "elapsed_ms:", elapsed_ms(begin));
    co_return txn;
}

// ==================== 连接池 ====================

upool::upool(const config &cfg)
{
    Init(cfg);
}

upool::~upool() { Close(); }

void upool::Init(const config &cfg)
{
    st_ = std::make_shared<state>(cfg, cfg.max_size ? cfg.max_size : 1);
    if (st_->cfg.max_size == 0)
    {
        st_->cfg.max_size = 1;
    }
    go reaper(st_);
}

/// 后台缩容协程: 每 reap_interval_ms 关闭一个 idle, 保底 min_idle.
/// 睡在可取消定时器上, close() 经 waker.wake() 立即唤醒 (微秒级),
/// 无需轮询 closed 标志, 不拖垮调度器收尾.
/// 帧内持有 state 的 shared_ptr, 池析构后仍可安全访问并自行退出.
uco::task<void> upool::reaper(std::shared_ptr<state> st)
{
    while (true)
    {
        co_await st->waker.sleep_for(
            std::chrono::milliseconds(st->cfg.reap_interval_ms));
        if (st->closed)
        {
            MYSQL_DBG("reaper exit");
            co_return;
        }

        MYSQL *victim = nullptr;
        {
            std::lock_guard<std::mutex> guard(st->mtx);
            if (!st->closed && st->idle.size() > st->cfg.min_idle)
            {
                victim = st->idle.front();
                st->idle.pop_front();
                st->all.erase(victim);
            }
        }
        if (victim != nullptr)
        {
            MYSQL_DBG("usql: pool reap idle, fd:", get_mysql_fd(victim));
            uclose(victim);
        }
    }
}

uco::task<MYSQL *> upool::Acquire()
{
    auto st = st_; // 协程帧持有状态所有权, 池析构也能安全完成.

    co_await st->permits.wait();

    MYSQL *m = nullptr;

    { // 复用空闲.
        std::lock_guard<std::mutex> guard(st->mtx);
        if (st->closed)
        {
            st->permits.signal();
            MYSQL_DBG("usql: pool acquire: pool closed");
            co_return nullptr;
        }
        if (!st->idle.empty())
        {
            m = st->idle.front();
            st->idle.pop_front();
            MYSQL_DBG("usql: pool acquire: reuse, fd:", get_mysql_fd(m));
            co_return m;
        }
    }

    // 新建连接 (不持锁).
    MYSQL_DBG("usql: pool acquire: no idle, creating");
    m = co_await uconnect(st->cfg.host.c_str(), st->cfg.user.c_str(),
                          st->cfg.pass.c_str(), st->cfg.db.c_str(),
                          st->cfg.port, st->cfg.ts, st->cfg.use_ssl);
    if (m == nullptr)
    {
        SYSERR("usql: pool acquire: create failed");
        st->permits.signal();
        co_return nullptr;
    }

    { // 建连期间池可能已被关闭.
        std::lock_guard<std::mutex> guard(st->mtx);
        if (!st->closed)
        {
            st->all.insert(m);
        }
        else
        {
            SYSERR("usql: pool acquire: pool closed during connect");
            uclose(m);
            st->permits.signal();
            co_return nullptr;
        }
    }
    co_return m;
}

void upool::Release(MYSQL *mysql)
{
    if (mysql == nullptr)
    {
        return;
    }

    // 自动判定死活: mysql_errno 返回最后一次 API 调用的错误码 (粘滞).
    //   >= 2000: 客户端/传输层错误 (2003 连接失败, 2006 server gone,
    //            2013 传输中断...), 连接已不可用 -> 销毁.
    //   <  2000: 服务端错误 (1064 语法错误等) 或 0 (无错误),
    //            连接仍然健康 -> 回收复用.
    // 注: 超时若未触发库内报错 (errno 为 0), 连接会被回收,
    //     由下一位使用者触发连接错误后再销毁 (惰性自愈).
    bool broken = (mysql_errno(mysql) >= 2000);
    bool recycle = false;

    {
        std::lock_guard<std::mutex> guard(st_->mtx);
        if (!broken && !st_->closed)
        {
            st_->idle.emplace_back(mysql);
            recycle = true;
        }
        else
        {
            st_->all.erase(mysql);
        }
    }
    st_->permits.signal();

    if (!recycle)
    {
        if (broken)
        {
            SYSWRN("usql: pool release: destroy broken conn, fd:",
                   get_mysql_fd(mysql), "errno:", mysql_errno(mysql),
                   "msg:", mysql_error(mysql));
        }
        else
        {
            SYSERR("usql: pool release: pool closed, destroy, fd:",
                   get_mysql_fd(mysql));
        }
        uclose(mysql);
    }
    else
    {
        MYSQL_DBG("usql: pool release: recycle, fd:", get_mysql_fd(mysql));
    }
}

void upool::Close()
{
    std::vector<MYSQL *> victims;
    {
        std::lock_guard<std::mutex> guard(st_->mtx);
        if (st_->closed)
        {
            return;
        }
        st_->closed = true;

        // all 含借出中的连接.
        victims.assign(st_->all.begin(), st_->all.end());
        st_->all.clear();
        st_->idle.clear();
    }
    st_->waker.wake(); // 立即唤醒 reaper 使其感知关闭并退出.
    SYSMSG("usql: pool close, conns:", victims.size());
    for (auto mysql : victims)
    {
        uclose(mysql);
    }
}

} // namespace usql
