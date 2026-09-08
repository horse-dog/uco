#pragma once

/**
 * @file user.h
 * @brief user 表数据访问 (dao 层).
 *
 * 表结构 (见 webserver 建表约定):
 *   vid      BIGINT UNSIGNED  主键, 业务算法生成, 非自增;
 *   username VARCHAR(64)      唯一键;
 *   password VARCHAR(255)     密文 (bcrypt 等, 生成与校验在 service 层).
 *
 * 模型约定:
 *   - dao 出入参直接使用 pb 的 User (表镜像, 含 password 密文),
 *     仅限 service 层内部使用;
 *   - 对外响应一律构造 UserInfo (不含 password), 密文回显的防线
 *     在响应建模, 不在 dao;
 *   - 绑定走 usql::uselect (三态语义: 未命中与系统错误分离).
 *
 * @note dao 只做存取: 不做密码加密/校验 (service 职责),
 *       不做登录态判断 (session 职责);
 * @note 防注入: 字符串参数一律 usql::uescape 后拼接, 整数直接拼接;
 * @note 多态取舍: 暂不抽 virtual 接口 —— 当前调用方是 service 层
 *       GetInstance 直调, 无第二实现与 mock 消费者; 待第一个单测
 *       需要假 dao 时再抽 IUserDao + 指针注入, 改动面可控;
 * @note 单例风格与 usql::upool / SessionStore 一致; dao 自身无配置
 *       (MySQL 池由 HttpServer::InitMySqLPool 初始化), 无需 Init.
 */

#include "uco.h"
#include "user.pb.h"

#include <cstdint>
#include <string>

namespace webserver
{
namespace dao
{

/**
 * @brief user 表访问器.
 */
class UserDao
{
  public:
    static UserDao &GetInstance();

    UserDao(const UserDao &) = delete;
    UserDao &operator=(const UserDao &) = delete;
    UserDao(UserDao &&) = delete;
    UserDao &operator=(UserDao &&) = delete;

    /**
     * @brief 注册: 插入用户.
     * @param vid 用户ID (业务算法生成, 避开 0).
     * @param username 用户名.
     * @param password_hash 密码密文 (bcrypt).
     * @return 0 成功;
     *         1 用户名已被占用;
     *        -1 系统错误 (不可重试, 已记日志);
     *        -2 系统错误 (可重试: 超时/连接类/死锁, 是否重试由业务决策).
     */
    uco::task<int> InsertUser(uint64_t vid, const std::string &username,
                              const std::string &password_hash);

    /**
     * @brief 登录: 按用户名取用户 (含密文, 供 service 做密码校验).
     * @param username 用户名.
     * @param out 出参, 成功时填充行数据.
     * @return 0 成功 (out 已填充);
     *         1 无此用户;
     *        -1 系统错误 (不可重试, 已记日志);
     *        -2 系统错误 (可重试: 超时/连接类/死锁).
     */
    uco::task<int> QueryByUsername(const std::string &username, User &out);

    /**
     * @brief 鉴权后取用户信息 / 按 vid 查询.
     * @param vid 用户ID.
     * @param out 出参, 成功时填充行数据.
     * @return 0 成功 (out 已填充);
     *         1 无此用户;
     *        -1 系统错误 (不可重试, 已记日志);
     *        -2 系统错误 (可重试: 超时/连接类/死锁).
     */
    uco::task<int> QueryByVid(uint64_t vid, User &out);

    /**
     * @brief 修改密码.
     * @param vid 用户ID.
     * @param password_hash 新密码密文 (bcrypt).
     * @return 0 成功;
     *         1 无此用户;
     *        -1 系统错误 (不可重试, 已记日志);
     *        -2 系统错误 (可重试: 超时/连接类/死锁).
     * @note bcrypt 输出带随机盐, 同一明文两次哈希必不同,
     *       故 affected_rows == 0 可安全视为"无此用户";
     *       (若某日换无盐哈希, 此语义会误判"新旧相同", 届时需重审.)
     */
    uco::task<int> UpdatePassword(uint64_t vid,
                                  const std::string &password_hash);

  private:
    UserDao() = default;
    ~UserDao() = default;
};

} // namespace dao
} // namespace webserver
