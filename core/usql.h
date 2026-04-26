/**
  * 懒得写了，ai 生成一份伪代码🤡.
  * 懒得写了，ai 生成一份伪代码🤡.
  * 1. uring 监听读写：
      - io_uring_prep_poll_add(sqe, fd, POLLIN);          // 监听可读
      - io_uring_prep_poll_add(sqe, fd, POLLOUT);         // 监听可写
      - io_uring_prep_poll_add(sqe, fd, POLLIN | POLLOUT); // 同时监听 

  * 2. MYSQL 异步接口参考：https://dev.mysqlserver.cn/doc/c-api/8.4/en/c-api-asynchronous-interface-usage.html#c-api-asynchronous-interface-example
  * 3. MYSQL 连接管理思路：
      - 连接池化（资源控制，并发限制）
      - 连接池动态维护：空闲连接超时释放.
      - 进程退出时注意释放.
  * 4. poll_add 一定要有超时控制.
  */
// #pragma once
// #include <mysql/mysql.h>

// #define AWAIT_IO(call) \
//     while (true) { \
//         auto _status = call; \
//         if (_status == NET_ASYNC_COMPLETE) break; \
//         if (_status == NET_ASYNC_ERROR) { /* 错误处理 */ return; } \
//         co_await wait_fd(fd, POLLIN | POLLOUT, timeout); \
//     }

// #define AWAIT_IN(call) \
//     while (true) { \
//         auto _status = call; \
//         if (_status == NET_ASYNC_COMPLETE) break; \
//         if (_status == NET_ASYNC_ERROR) { /* 错误处理 */ return; } \
//         co_await wait_fd(fd, POLLIN, timeout); \
//     }

// #define AWAIT_OUT(call) \
//     while (true) { \
//         auto _status = call; \
//         if (_status == NET_ASYNC_COMPLETE) break; \
//         if (_status == NET_ASYNC_ERROR) { /* 错误处理 */ return; } \
//         co_await wait_fd(fd, POLLOUT, timeout); \
//     }

// int get_mysql_fd(MYSQL* conn)
// {
//     return (int)conn->net.fd;
// }

// uco::task<MYSQL*> mysql_init_async(const char* host, const char* user,
//                         const char* pass, const char* db,
//                         unsigned int port)
// {
//     MYSQL* mysql = mysql_init(nullptr);
//     mysql_options(mysql, MYSQL_OPT_NONBLOCK, nullptr);

//     AWAIT_IO( mysql_real_connect_nonblocking(
//         mysql, host, user, pass, db, port, nullptr, 0
//     ));

//     co_return mysql;
// }
// // 用法: MYSQL* mysql = co_await mysql_init_async(host, user, pass, db, port);

// uco::task<InsertResult> async_insert(MYSQL* mysql, const char* sql, size_t len)
// {
//     // 发请求 + 收 OK 包（含 insert_id 和 affected_rows）
//     AWAIT_IO(mysql_real_query_nonblocking(mysql, sql, len));

//     InsertResult result;
//     result.insert_id   = mysql_insert_id(mysql);
//     result.affected_rows = mysql_affected_rows(mysql);
//     co_return result;
// }

// uco::task<uint64_t> async_update(MYSQL* mysql, const char* sql, size_t len)
// {
//     AWAIT_IO( mysql_real_query_nonblocking(mysql, sql, len) );
//     co_return mysql_affected_rows(mysql);
// }

// uco::task<uint64_t> async_delete(MYSQL* mysql, const char* sql, size_t len)
// {
//     AWAIT_IO( mysql_real_query_nonblocking(mysql, sql, len) );
//     co_return mysql_affected_rows(mysql);
// }

// uco::task<SelectResult> async_select(MYSQL* mysql, const char* sql, size_t len)
// {
//     // 步骤1：发送请求 + 收服务端确认包
//     AWAIT_IO( mysql_real_query_nonblocking(mysql, sql, len) );

//     // 步骤2：拉取结果集到本地内存（纯收数据）
//     MYSQL_RES* result = nullptr;
//     AWAIT_IN( mysql_store_result_nonblocking(mysql, &result) );

//     // 步骤3：遍历（纯内存操作，不需要 await）
//     SelectResult ret;

//     ret.num_fields = mysql_num_fields(result);
//     ret.num_rows   = mysql_num_rows(result);

//     MYSQL_FIELD* fields = mysql_fetch_fields(result);
//     for (unsigned int i = 0; i < ret.num_fields; i++) {
//         ret.column_names.push_back(fields[i].name);
//     }

//     MYSQL_ROW row;
//     while ((row = mysql_fetch_row(result))) {
//         unsigned long* lengths = mysql_fetch_lengths(result);
//         std::vector<std::string> row_data(ret.num_fields);
//         for (unsigned int i = 0; i < ret.num_fields; i++) {
//             row_data[i] = std::string(row[i], lengths[i]);
//         }
//         ret.rows.push_back(std::move(row_data));
//     }

//     mysql_free_result(result);
//     co_return ret;
// }

// // 防注入接口.
// uco::task<MYSQL_STMT*> async_prepare(
//     MYSQL* mysql, const char* stmt_sql, size_t len)
// {
//     MYSQL_STMT* stmt = mysql_stmt_init(mysql);
//     if (!stmt) co_return nullptr;

//     // prepare：发送模板 + 收确认
//     AWAIT_IO( mysql_stmt_prepare_nonblocking(stmt, stmt_sql, len) );
//     co_return stmt;
// }

// // 防注入接口.
// uco::task<void> async_execute(MYSQL_STMT* stmt,
//                                MYSQL_BIND* params, unsigned int param_count,
//                                bool is_select /* 是否有结果集需要收 */)
// {
//     // 绑定参数（同步）
//     mysql_stmt_bind_param(stmt, params);

//     // 执行：发送参数值 + 收确认
//     AWAIT_IO( mysql_stmt_execute_nonblocking(stmt) );

//     // SELECT 需要额外拉取结果集
//     if (is_select) {
//         AWAIT_IN( mysql_stmt_store_result_nonblocking(stmt) );
//     }
// }

// // 防注入接口.
// // 完整使用示例：
// uco::task<SelectResult> async_prepared_select(
//     MYSQL* mysql, const char* template_sql, int min_age)
// {
//     // Prepare
//     auto stmt = co_await async_prepare(mysql, template_sql, strlen(template_sql));

//     // Bind
//     MYSQL_BIND param[1] = {};
//     param[0].buffer_type = MYSQL_TYPE_LONG;
//     param[0].buffer      = &min_age;

//     // Execute + Store（SELECT 需要 IN）
//     co_await async_execute(stmt, param, 1, true);

//     // Fetch（同步）
//     SelectResult result;
//     result.num_fields = mysql_stmt_field_count(stmt);
//     // ... mysql_stmt_fetch 循环 ...

//     mysql_stmt_close(stmt);
//     co_return result;
// }

// struct StmtResult {
//     ulonglong affected_rows = 0;
//     ulonglong insert_id     = 0;
//     unsigned int warnings   = 0;
//     bool         ok          = true;
// };

// struct TxnResult {
//     bool           committed = false;       // 是否成功提交
//     std::vector<StmtResult> stmt_results;   // 每条语句的结果
//     unsigned int   errno     = 0;            // 错误码
//     std::string    error;                    // 错误描述
//     std::string    sqlstate;                 // SQLSTATE
// };

// uco::task<TxnResult> async_transaction(MYSQL* mysql,
//                                         const std::vector<const char*>& sqls)
// {
//     TxnResult txn;

//     // BEGIN
//     auto status = NET_ASYNC_NOT_READY;
//     AWAIT_IO( status = mysql_real_query_nonblocking(mysql, "BEGIN", 5) );
//     if (status == NET_ASYNC_ERROR) {
//         txn.errno    = mysql_errno(mysql);
//         txn.error    = mysql_error(mysql);
//         txn.sqlstate = mysql_sqlstate(mysql);
//         co_return txn;
//     }

//     // 逐条执行
//     for (const auto& sql : sqls) {
//         StmtResult sr;

//         status = NET_ASYNC_NOT_READY;
//         AWAIT_IO( status = mysql_real_query_nonblocking(mysql, sql, strlen(sql)) );

//         if (status == NET_ASYNC_ERROR) {
//             sr.ok = false;
//             txn.errno    = mysql_errno(mysql);
//             txn.error    = mysql_error(mysql);
//             txn.sqlstate = mysql_sqlstate(mysql);

//             // 回滚
//             AWAIT_IO( mysql_real_query_nonblocking(mysql, "ROLLBACK", 8) );
//             txn.stmt_results.push_back(sr);
//             co_return txn;
//         }

//         // ✅ 读取每条语句的结果信息
//         sr.affected_rows = mysql_affected_rows(mysql);
//         sr.insert_id     = mysql_insert_id(mysql);
//         sr.warnings      = mysql_warning_count(mysql);

//         txn.stmt_results.push_back(std::move(sr));
//     }

//     // COMMIT
//     status = NET_ASYNC_NOT_READY;
//     AWAIT_IO( status = mysql_real_query_nonblocking(mysql, "COMMIT", 6) );

//     if (status == NET_ASYNC_ERROR) {
//         txn.errno    = mysql_errno(mysql);
//         txn.error    = mysql_error(mysql);
//         txn.sqlstate = mysql_sqlstate(mysql);

//         // commit 失败也要尝试 rollback
//         AWAIT_IO( mysql_real_query_nonblocking(mysql, "ROLLBACK", 8) );
//         co_return txn;
//     }

//     txn.committed = true;
//     co_return txn;
// }

// void async_close(MYSQL* mysql)
// {
//     // mysql_close 内部发送 quit 包 + close(fd)
//     // 同步调用即可，不需要 await
//     mysql_close(mysql);
//     // fd 此后失效
// }
