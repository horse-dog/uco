#pragma once

/**
 * @file usql.h
 * @brief 基于 uco (io_uring + C++20 协程) 的 MySQL 异步基本 API.
 *
 * @note MySQL 异步接口文档:
 *       https://dev.mysql.com/doc/c-api/8.4/en/c-api-asynchronous-interface-usage.html
 * @note 所有接口默认超时 10s, ts = {0, 0} 表示不超时 (慎用);
 *       超时时结果中 ok = false 且 timeout = true.
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

/// 操作错误信息.
struct SqlError
{
    bool ok = true;           ///< 是否成功.
    bool timeout = false;     ///< 是否超时失败.
    unsigned int err_no = 0;  ///< 错误码.
    std::string err_msg;      ///< 错误描述.
    std::string sqlstate;     ///< SQLSTATE.
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

/// 关闭连接 (发送 quit 包并释放 fd).
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
 * @brief 异步 SELECT, 结果集绑定到 pb 对象.
 * @param out 单条形态: 绑定首行 (先清空, 多余行忽略, 空结果集不修改).
 * @return 是否成功; 列与字段须同名且类型兼容, 违者 false.
 */
uco::task<bool> uselect(MYSQL *mysql, std::string sql,
                        google::protobuf::Message *out,
                        uco_time_t ts = {10, 0});

/**
 * @brief 异步 SELECT, 结果集逐行填入 repeated 字段.
 * @param out 如 resp.mutable_users(), 原有内容先清空.
 * @return 是否成功, 语义同单条形态.
 * @note 必须为模板: RepeatedPtrField 私有继承内部基类且无运行时
 *       类型信息, 元素类型只能编译期获知.
 */
template <typename T>
uco::task<bool> uselect(MYSQL *mysql, std::string sql,
                        google::protobuf::RepeatedPtrField<T> *out,
                        uco_time_t ts = {10, 0})
{
    static_assert(std::is_base_of_v<google::protobuf::Message, T>,
                  "T must be a protobuf message type");
    out->Clear();
    T proto;
    SelectResult res = co_await uselect(mysql, std::move(sql), ts);
    if (!res.ok || !res.check_alignment(proto.GetDescriptor()))
    {
        co_return false;
    }
    for (size_t r = 0; r < res.rows.size(); r++)
    {
        if (!res.bind_row(r, *out->Add()))
        {
            co_return false;
        }
    }
    co_return true;
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

    static upool &GetInstance();
    static upool &GetInstance(const config &cfg);

    upool(const upool &) = delete;
    upool &operator=(const upool &) = delete;
    upool(upool &&) = delete;
    upool &operator=(upool &&) = delete;

    /**
     * @brief 获取连接: 优先复用空闲, 否则新建; 达到 max_size 则等待.
     * @return 连接句柄; 池已关闭或建连失败返回 nullptr.
     */
    uco::task<MYSQL *> acquire();

    /// 归还连接; 死活自动判定: 连接级错误 (>= 2000) 销毁,
    /// 服务端错误 (语法等) 或无错误则回收复用.
    void release(MYSQL *mysql);

    /// 关闭池并释放全部连接 (含借出中的, 进程退出时调用).
    void close();

  private:
    static upool &instance(const config *cfg);
    explicit upool(const config &cfg);
    ~upool();

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
        uco::utimer waker;           ///< 可取消定时器, 供 reaper 睡眠.
    };

    /// 后台缩容协程: 每 reap_interval_ms 关闭一个 idle, 保底 min_idle.
    /// 睡在可取消定时器上, close() 立即唤醒 (无需轮询, 不拖垮调度器收尾).
    /// static: 不依赖池对象生命周期, 仅通过 st 访问状态.
    static uco::task<void> reaper(std::shared_ptr<state> st);

    std::shared_ptr<state> st_;
};

} // namespace usql
