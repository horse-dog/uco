#pragma once

/**
 * @file usql.h
 * @brief 基于 uco (io_uring + C++20 协程) 的 MySQL 异步基本 API.
 *
 * @note MySQL 异步接口文档:
 *       https://dev.mysql.com/doc/c-api/8.4/en/c-api-asynchronous-interface-usage.html
 * @note 所有接口默认超时 10s, ts = {0, 0} 表示不超时 (慎用);
 *       超时时结果中 ret_code = UsqlError::kTimeout.
 * @note 官方限制: 不支持 LOAD DATA / LOAD XML 与协议压缩;
 *       异步操作完成前 sql 内存不得释放.
 * @note 防 SQL 注入请使用 uescape().
 */

#include "uio.h"
#include "usync.h"

#include <mysql/mysql.h>
#include <google/protobuf/message.h>
#include <google/protobuf/repeated_ptr_field.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

namespace usql
{

enum UsqlError
{
    // ---- usql 框架自身 ----
    kBadArgument = -200, ///< 参数非法/列与 proto 不对齐/绑定失败 (见 err_msg).
    kTimeout     = -201, ///< 操作超时 (应用层 deadline 到期).

    // ---- MySQL 服务端 (mysqld_error.h, 值为原码取负) ----
    kAccessDenied        = -1045, ///< ER_ACCESS_DENIED_ERROR 账号/密码/权限拒绝.
    kUnknownDatabase     = -1049, ///< ER_BAD_DB_ERROR 库不存在.
    kTableExists         = -1050, ///< ER_TABLE_EXISTS_ERROR 表已存在.
    kBadField            = -1054, ///< ER_BAD_FIELD_ERROR 未知列.
    kDuplicateEntry      = -1062, ///< ER_DUP_ENTRY 唯一键冲突.
    kParseError          = -1064, ///< ER_PARSE_ERROR SQL 语法错误.
    kNoSuchTable         = -1146, ///< ER_NO_SUCH_TABLE 表不存在.
    kLockWaitTimeout     = -1205, ///< ER_LOCK_WAIT_TIMEOUT 锁等待超时.
    kLockDeadlock        = -1213, ///< ER_LOCK_DEADLOCK 死锁 (可重试).
    kTruncatedWrongValue = -1292, ///< ER_TRUNCATED_WRONG_VALUE 值格式错/被截断.
    kDataTooLong         = -1406, ///< ER_DATA_TOO_LONG 数据超列长.
    kRowIsReferenced     = -1451, ///< ER_ROW_IS_REFERENCED_2 外键约束, 行被引用.
    kForeignKeyNoParent  = -1452, ///< ER_NO_REFERENCED_ROW_2 外键约束, 父行不存在.

    // ---- MySQL 客户端 (errmsg.h, 值为原码取负) ----
    kConnHostError = -2003, ///< CR_CONN_HOST_ERROR 连不上服务器.
    kUnknownHost   = -2005, ///< CR_UNKNOWN_HOST 主机名解析失败.
    kServerGone    = -2006, ///< CR_SERVER_GONE_ERROR 服务器已断开.
    kOutOfMemory   = -2008, ///< CR_OUT_OF_MEMORY 客户端内存耗尽.
    kServerLost    = -2013, ///< CR_SERVER_LOST 查询期间连接丢失.
};

/// 操作错误信息.
struct SqlError
{
    int ret_code = 0;         ///< 0 成功; !0 失败.
    std::string err_msg;      ///< 错误描述.
    std::string sqlstate;     ///< SQLSTATE.

    /**
     * @brief 是否瞬态错误 (重试有成功可能).
     *        可重试: 超时 / MySQL 客户端连接类 (原码 2xxx) / 死锁 /
     *                锁等待超时;
     *        不可重试: 服务端逻辑错误 (语法/表结构等, 属开发期问题)
     *                及 usql 框架错误.
     */
    bool Retryable() const;
};

/// INSERT 结果.
struct InsertResult : SqlError
{
    uint64_t insert_id = 0;     ///< 自增主键.
    uint64_t affected_rows = 0; ///< 影响行数.
};

/// UPDATE / DELETE 结果.
struct AffectedResult : SqlError
{
    uint64_t affected_rows = 0; ///< 影响行数.
};

/// SELECT 结果 (NULL 列以空串表示).
struct SelectResult : SqlError
{
    unsigned int num_fields = 0;             ///< 列数.
    uint64_t num_rows = 0;                   ///< 行数.
    std::vector<std::string> column_names;   ///< 列名.
    std::vector<enum_field_types> column_types; ///< 列类型.
    std::vector<unsigned int> column_flags;  ///< 列标志 (UNSIGNED_FLAG 等).
    std::vector<std::vector<std::string>> rows; ///< 行数据.

    /// 严格校验列与 proto 描述符对齐 (列名 + 类型), 失败记 SYSERR.
    bool check_alignment(const google::protobuf::Descriptor *desc) const;

    /// 严格绑定一行到 msg (须先通过 check_alignment), 失败记 SYSERR.
    bool bind_row(size_t row_idx, google::protobuf::Message &msg) const;
};

/// 事务中单条语句的结果.
struct StmtResult
{
    uint64_t affected_rows = 0; ///< 影响行数.
    uint64_t insert_id = 0;     ///< 自增主键.
    unsigned int warnings = 0;  ///< 告警数.
    bool ok = true;             ///< 是否成功.
};

/// 事务整体结果.
struct TxnResult : SqlError
{
    bool committed = false;                ///< 是否成功提交.
    std::vector<StmtResult> stmt_results;  ///< 每条语句的结果.
};

// ==================== 连接 ====================

/**
 * @brief 建立数据库连接.
 * @param use_ssl 是否启用 TLS. 默认关闭: 本地/内网免 TLS 握手开销
 *                (实测 VM 上每连接可省 ~百 ms); 跨不可信网络置 true.
 * @return 连接句柄, 失败返回 nullptr.
 */
uco::task<MYSQL *> uconnect(const char *host, const char *user,
                                    const char *pass, const char *db,
                                    unsigned int port,
                                    uco_time_t ts = {10, 0},
                                    bool use_ssl = false);

/// 关闭连接.
void uclose(MYSQL *mysql);

/// 转义字符串, 拼 SQL 防注入用.
std::string uescape(MYSQL *mysql, const std::string &s);

// ==================== 增删改查 ====================

/**
 * @brief 异步 INSERT.
 * @return 自增主键与影响行数.
 */
uco::task<InsertResult> uinsert(MYSQL *mysql, std::string sql,
                                uco_time_t ts = {10, 0});

/**
 * @brief 异步 UPDATE.
 * @return 影响行数.
 */
uco::task<AffectedResult> uupdate(MYSQL *mysql, std::string sql,
                                  uco_time_t ts = {10, 0});

/**
 * @brief 异步 DELETE.
 * @return 影响行数.
 */
uco::task<AffectedResult> udelete(MYSQL *mysql, std::string sql,
                                  uco_time_t ts = {10, 0});

/**
 * @brief 异步 SELECT, 结果集全量拉取到内存.
 * @return 列名与行数据.
 */
uco::task<SelectResult> uselect(MYSQL *mysql, std::string sql,
                                uco_time_t ts = {10, 0});

/**
 * @brief 异步 SELECT, 结果集绑定到 pb 对象 (单条形态).
 * @param out 无行时不修改; 有行时先清空再绑定首行 (多余行忽略).
 * @return res.ok=false: 执行/对齐/绑定失败 (err_msg 有详情);
 *         res.ok=true && num_rows==0: 无行 (out 未修改);
 *         res.ok=true && num_rows>=1: 首行已绑定到 out
 *         (rows 已释放, 数据只在 out, 勿再取 rows).
 * @note 列与字段须同名且类型兼容, 违者 ok=false.
 */
uco::task<SelectResult> uselect(MYSQL *mysql, std::string sql,
                                google::protobuf::Message *out,
                                uco_time_t ts = {10, 0});

/**
 * @brief 异步 SELECT, 结果集逐行填入 repeated 字段.
 * @param out 如 resp.mutable_users(), 原有内容先清空; 失败时亦清空.
 * @return res.ok=false: 执行/对齐/绑定失败 (err_msg 有详情);
 *         res.ok=true: 全部行已填入 out, 行数即 out->size()
 *         (rows 已释放, 数据只在 out, 勿再取 rows).
 */
template <typename T>
uco::task<SelectResult> uselect(MYSQL *mysql, std::string sql,
                                google::protobuf::RepeatedPtrField<T> *out,
                                uco_time_t ts = {10, 0})
{
    static_assert(std::is_base_of_v<google::protobuf::Message, T>,
                  "T must be a protobuf message type");
    // 参数校验先于任何 IO (空 out 解引用 Clear 会直接崩溃).
    if (out == nullptr)
    {
        SelectResult res;
        res.ret_code = kBadArgument;
        res.err_msg = "usql: uselect: out is nullptr";
        co_return res;
    }
    out->Clear();
    T proto;
    SelectResult res = co_await uselect(mysql, std::move(sql), ts);
    if (res.ret_code != 0)
    {
        co_return res; // 执行失败, 原样带错误信息.
    }
    if (!res.check_alignment(proto.GetDescriptor()))
    {
        // 对齐失败详情已由 check_alignment 记 SYSERR.
        res.ret_code = kBadArgument;
        res.err_msg = "usql: uselect: columns not aligned with proto fields";
        co_return res;
    }
    for (size_t r = 0; r < res.rows.size(); r++)
    {
        if (!res.bind_row(r, *out->Add()))
        {
            // 绑定失败详情已由 bind_row 记 SYSERR; 回滚半填充状态.
            out->Clear();
            res.ret_code = kBadArgument;
            res.err_msg = "usql: uselect: bind row failed";
            co_return res;
        }
    }
    // 数据已全部转入 out, 释放原始行 (num_rows 保留).
    res.rows.clear();
    res.rows.shrink_to_fit();
    co_return res;
}

// ==================== 事务 ====================

/**
 * @brief 异步事务: BEGIN -> 逐条执行 -> COMMIT, 任一步失败自动 ROLLBACK.
 * @note GCC 13 已知 bug: 花括号初始化内含函数调用的写法会触发编译器 ICE,
 *       如 co_await utransaction(m, {"...", make_str()});
 *       请先构造 vector 再传入 (GCC 14+ 已修复).
 */
uco::task<TxnResult> utransaction(
    MYSQL *mysql, std::vector<std::string> sqls, uco_time_t ts = {10, 0});

// ==================== 连接池 ====================

/// 连接池: 复用连接, max_size 限制并发, 后台协程周期缩容 idle.
class upool
{
  public:
    /// 配置.
    struct config
    {
        std::string host = "127.0.0.1"; ///< 主机.
        std::string user;               ///< 用户名.
        std::string pass;               ///< 密码.
        std::string db;                 ///< 数据库.
        unsigned int port = 3306;       ///< 端口.
        size_t max_size = 16;           ///< 最大连接数 (并发限制).
        size_t min_idle = 1;            ///< idle 保底数, 低于等于此值不再关闭.
        uint64_t reap_interval_ms = 60000; ///< 缩容间隔 (ms), 每次关闭一个 idle.
        bool use_ssl = false;           ///< TLS (本地/内网建议关, 省握手开销).
        uco_time_t ts = {10, 0};        ///< 建连超时.
    };

    upool(const config &cfg);
   ~upool();
    upool(const upool &) = delete;
    upool &operator=(const upool &) = delete;
    upool(upool &&) = delete;
    upool &operator=(upool &&) = delete;

    /**
     * @brief 获取连接: 优先复用空闲, 否则新建; 达到 max_size 则等待.
     * @return 连接句柄; 池已关闭或建连失败返回 nullptr.
     */
    uco::task<MYSQL *> Acquire();

    /// 归还连接; 死活自动判定: 连接级错误 (>= 2000) 销毁,
    /// 服务端错误 (语法等) 或无错误则回收复用.
    void Release(MYSQL *mysql);

    /// 关闭池并释放全部连接 (含借出中的, 进程退出时调用).
    void Close();

  private:
    /// 初始化状态并启动 reaper. 仅由构造函数调用, 不可重复执行.
    void Init(const config &cfg);

    /// 内部状态: shared_ptr 共享所有权 (reaper 协程与进行中的 acquire 各持
    /// 一份), 保证池析构后协程仍能安全访问状态并自行退出.
    struct state
    {
        state(const config &c, size_t permits_cnt)
            : cfg(c), permits(permits_cnt)
        {
        }

        config cfg;
        std::atomic<bool> closed{false};
        std::mutex mtx;              ///< 保护 idle/all (临界区内无 co_await).
        uco::usema permits;          ///< 槽位信号量, 容量 = max_size.
        std::deque<MYSQL *> idle;    ///< 空闲连接 (队首最老).
        std::set<MYSQL *> all;       ///< 全部存活连接 (含借出中).
        uco::usleeper waker;           ///< 可取消定时器, 供 reaper 睡眠.
    };

    /// 后台缩容协程: 每 reap_interval_ms 关闭一个 idle, 保底 min_idle.
    /// 睡在可取消定时器上, close() 立即唤醒 (无需轮询, 不拖垮调度器收尾).
    /// static: 不依赖池对象生命周期, 仅通过 st 访问状态.
    static uco::task<void> reaper(std::shared_ptr<state> st);

    std::shared_ptr<state> st_;
};

class UsqlGuard
{
public:
    UsqlGuard(upool &pool, MYSQL *conn) : pool_(pool), conn_(conn) {}
    ~UsqlGuard() { pool_.Release(conn_); }
private:
    upool &pool_;
    MYSQL *conn_ = 0;
};

} // namespace usql
