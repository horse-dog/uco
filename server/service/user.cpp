/**
 * @file user.cpp
 * @brief 用户业务逻辑实现 (UserService), 见 service/user.h.
 */

#include "server/service/user.h"

#include "core/ulog.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <openssl/rand.h>
#include <stdexcept>

namespace webserver
{
namespace service
{

UserService::UserService(dao::IUserDao &dao,
                         security::IPasswordHasher &hasher)
: m_dao(dao), m_hasher(hasher)
{
}

/// 生成业务用户ID: 48bit 注册毫秒时间戳 << 16 | 15bit 随机 | 1.
/// - 趋势递增: InnoDB 主键 B+ 树顺序插入, 索引友好;
/// - 防碰撞: 同毫秒 2^15 随机槽, 跨毫秒由时间戳区分;
/// - 可解析: vid >> 16 即注册时间 (ms), 运维友好;
/// - 非零: 最低位置 1; 48bit 毫秒可用至公元 ~8900 年.
/// 随机源故障 (环境瞬态错误) 抛异常: 由 HttpConnection 兜底为
/// 单请求 500, 勿终止进程放大故障面.
static uint64_t NewVid()
{
    uint64_t ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    uint16_t rnd = 0;
    if (RAND_bytes(reinterpret_cast<unsigned char *>(&rnd), sizeof(rnd)) != 1)
    {
        LOGERR("register: rand_bytes failed for vid");
        throw std::runtime_error("rand_bytes failed for vid");
    }
    return (ms << 16) | (rnd & 0x7fff) | 1;
}

/// 用户名字符白名单 [A-Za-z0-9_-] (见 service/user.proto RegisterReq 契约):
/// 根除同形仿冒 (西里尔 а 冒充 a) 与控制字符/空白注入面;
/// 白名单皆单字节 ASCII, 字节数即字符数.
static bool HasValidUsernameChars(const std::string &username)
{
    return std::all_of(username.begin(), username.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

/// 密码含危险字符: 控制字符 / 首尾空白
/// (黑名单式, 不做白名单 — 见 service/user.proto RegisterReq 契约).
static bool HasBadPasswordChars(const std::string &pw)
{
    if (pw.front() == ' ' || pw.back() == ' ')
    {
        return true;
    }
    return std::any_of(pw.begin(), pw.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    });
}

/// 密码包含用户名 (NIST 800-63B 检查项).
static bool ContainsUsername(const std::string &pw, const std::string &username)
{
    return pw.find(username) != std::string::npos;
}

/// 弱密码黑名单 (现代派: 黑名单式, 不强制组合规则).
/// demo 级小表; 生产替换为泄露库 top 列表 (rockyou) 或
/// HaveIBeenPwned k-anonymity API. 黑名单只放后端 (安全数据不下发前端).
static bool IsWeakPassword(const std::string &pw)
{
    static const std::array<const char *, 12> kWeakPasswords = {
        "123456",   "12345678", "123456789", "password", "qwerty",
        "111111",   "abc123",   "000000",    "admin",    "letmein",
        "iloveyou", "666666",
    };
    return std::find(kWeakPasswords.begin(), kWeakPasswords.end(), pw) !=
           kWeakPasswords.end();
}

uco::task<int> UserService::Register(const service::RegisterReq &req,
                                     service::RegisterRsp &rsp)
{
    // 1. 参数校验 (规则契约见 service/user.proto RegisterReq 注释).
    if (req.username().size() < 3 || req.username().size() > 64 ||
        !HasValidUsernameChars(req.username()))
    {
        LOGWRN("register: invalid username, username:", req.username());
        co_return 2;
    }
    if (req.password().size() < 8 || req.password().size() > 64)
    {
        LOGWRN("register: invalid password length, username:", req.username(),
               ", password len:", req.password().size());
        co_return 2;
    }
    if (HasBadPasswordChars(req.password()) ||
        ContainsUsername(req.password(), req.username()) ||
        IsWeakPassword(req.password()))
    {
        LOGWRN("register: password too weak, username:", req.username());
        co_return 3; // 密码过于简单 (注册场景明示, 无枚举顾虑).
    }

    // 2. 生成 vid (时间戳+随机, 趋势递增; 同毫秒碰撞概率 2^-15,
    //    极端情况下主键冲突会以"用户名已被占用"返回, 可重试).
    uint64_t vid = NewVid();

    // 3. bcrypt 加密 (cost 12 慢哈希, 在线程池中执行).
    std::string hash;
    int ret = co_await m_hasher.Hash(req.password(), hash);
    if (ret != 0)
    {
        LOGERR("register: bcrypt hash failed, username:", req.username(),
               ", ret:", ret);
        co_return -1;
    }

    // 4. 插入 (dao 返回码透传: 1 用户名已被占用).
    dao::InsertUserReq dreq;
    dreq.set_vid(vid);
    dreq.set_username(req.username());
    dreq.set_password_hash(hash);
    dao::InsertUserRsp drsp;
    ret = co_await m_dao.InsertUser(dreq, drsp);
    if (ret != 0)
    {
        if (ret == 1)
        {
            LOGWRN("register: username taken, username:", req.username());
            co_return 1;
        }
        LOGERR("register: dao insert failed, username:", req.username(),
               ", ret:", ret);
        co_return ret;
    }

    rsp.set_vid(vid);
    rsp.set_username(req.username());
    LOGMSG("register: ok, username:", req.username(), ", vid:", vid);
    co_return 0;
}

uco::task<int> UserService::Login(const service::LoginReq &req,
                                  service::LoginRsp &rsp)
{
    // 1. 取认证凭据 (dao 返回码透传: 1 无此用户, -1/-2 系统).
    dao::QueryByUsernameReq dreq;
    dreq.set_username(req.username());
    dao::QueryByUsernameRsp drsp;
    int ret = co_await m_dao.QueryByUsername(dreq, drsp);
    const bool user_exists = ret == 0;
    if (ret < 0)
    {
        LOGERR("login: dao query failed, username:", req.username(),
               ", ret:", ret);
        co_return ret; // -1/-2 系统错误透传.
    }

    // 2. 用户不存在也执行同 cost 的 bcrypt，降低基于耗时的账号枚举风险.
    ret = user_exists
              ? co_await m_hasher.Verify(req.password(), drsp.password_hash())
              : co_await m_hasher.VerifyDummy(req.password());
    if (ret < 0)
    {
        LOGERR("verify failed, ret:", ret);
        co_return -1;
    }
    if (!user_exists)
    {
        LOGMSG("login: no such user, username:", req.username());
        co_return 1;
    }
    if (ret == 1)
    {
        LOGWRN("login: wrong password, username:", req.username());
        co_return 2; // 密码错误.
    }

    // 3. 填充登录成功信息 (不含密文).
    rsp.set_vid(drsp.vid());
    rsp.set_username(drsp.username());
    LOGMSG("login: ok, username:", rsp.username(), ", vid:", rsp.vid());
    co_return 0;
}

} // namespace service
} // namespace webserver
