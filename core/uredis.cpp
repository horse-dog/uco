#define _UCO_THREAD_ENV_IMPL
#include "core/uredis.h"
#include "core/ulog.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <random>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace uredis
{

// ==================== 连接 ====================

struct uconnection
{
    int fd = -1;
    bool broken = false; ///< 传输层出错/超时后置位, 流已失步, 不可复用.
    std::string rbuf;    ///< 读缓冲 (单次命令内使用).
};

// ==================== 内部实现 ====================

namespace __inner__
{

/// 总超时控制 (参考 usql::await_call):
/// 记录起点与总量, 每次收发前换算剩余 ts.
struct deadline
{
    bool has = false;
    int64_t total_ns = 0;
    std::chrono::steady_clock::time_point begin;

    void init(uco_time_t ts)
    {
        has = (ts.tv_sec != 0 || ts.tv_nsec != 0);
        total_ns = (int64_t)ts.tv_sec * 1'000'000'000 + ts.tv_nsec;
        begin = std::chrono::steady_clock::now();
    }

    /// 换算剩余 ts; 已超时返回 false. 无超时则 out = {0, 0}.
    bool remain(uco_time_t &out)
    {
        out = {0, 0};
        if (!has)
        {
            return true;
        }
        using namespace std::chrono;
        int64_t left = total_ns -
                       duration_cast<nanoseconds>(steady_clock::now() - begin)
                           .count();
        if (left <= 0)
        {
            return false;
        }
        out.tv_sec = (decltype(out.tv_sec))(left / 1'000'000'000);
        out.tv_nsec = (decltype(out.tv_nsec))(left % 1'000'000'000);
        return true;
    }
};

/// RESP2 请求序列化: *N\r\n$len\r\narg\r\n...
static std::string serialize(const std::vector<std::string> &args)
{
    std::string out;
    out.reserve(32);
    out += '*';
    out += std::to_string(args.size());
    out += "\r\n";
    for (const auto &arg : args)
    {
        out += '$';
        out += std::to_string(arg.size());
        out += "\r\n";
        out += arg;
        out += "\r\n";
    }
    return out;
}

/// RESP2 递归下降解析器.
/// parse_one 返回: 0 成功, 1 数据不足 (需继续收), -1 协议错误.
struct parser
{
    const std::string &buf;
    size_t pos = 0;

    parser(const std::string &b) : buf(b) {}

    /// 从 pos 起找 CRLF 行; 成功后 pos 越过 CRLF, 数据不足返回 false.
    bool find_line(size_t &begin, size_t &end)
    {
        size_t p = buf.find("\r\n", pos);
        if (p == std::string::npos)
        {
            return false;
        }
        begin = pos;
        end = p;
        pos = p + 2;
        return true;
    }

    static bool parse_int(const std::string &s, int64_t &v)
    {
        if (s.empty())
        {
            return false;
        }
        size_t i = (s[0] == '-') ? 1 : 0;
        if (i >= s.size())
        {
            return false;
        }
        v = 0;
        for (; i < s.size(); i++)
        {
            if (s[i] < '0' || s[i] > '9')
            {
                return false;
            }
            v = v * 10 + (s[i] - '0');
        }
        if (s[0] == '-')
        {
            v = -v;
        }
        return true;
    }

    int parse_one(Reply &out)
    {
        if (pos >= buf.size())
        {
            return 1;
        }
        char t = buf[pos];
        pos++; // 消费类型字符.

        size_t b = 0;
        size_t e = 0;
        switch (t)
        {
        case '+': // 简单字符串.
        case '-': // 错误.
        case ':': // 整数.
        {
            if (!find_line(b, e))
            {
                return 1;
            }
            if (t == ':')
            {
                int64_t v = 0;
                if (!parse_int(std::string(buf, b, e - b), v))
                {
                    return -1;
                }
                out.type = Reply::INTEGER;
                out.integer = v;
                return 0;
            }
            out.type = Reply::STATUS;
            out.str.assign(buf, b, e - b);
            if (t == '-')
            { // 服务端错误回复, 连接仍健康.
                out.ret_code = kServerError;
                out.err_msg = out.str;
            }
            return 0;
        }
        case '$': // bulk string.
        {
            if (!find_line(b, e))
            {
                return 1;
            }
            int64_t len = 0;
            if (!parse_int(std::string(buf, b, e - b), len))
            {
                return -1;
            }
            if (len < 0)
            { // $-1: 空 bulk.
                out.type = Reply::NIL;
                return 0;
            }
            if ((int64_t)(buf.size() - pos) < len + 2)
            {
                return 1; // 数据未收全.
            }
            out.type = Reply::STRING;
            out.str.assign(buf, pos, (size_t)len);
            pos += (size_t)len + 2; // 跳过数据与 CRLF.
            return 0;
        }
        case '*': // 数组.
        {
            if (!find_line(b, e))
            {
                return 1;
            }
            int64_t cnt = 0;
            if (!parse_int(std::string(buf, b, e - b), cnt))
            {
                return -1;
            }
            if (cnt < 0)
            { // *-1: 空数组.
                out.type = Reply::NIL;
                return 0;
            }
            out.type = Reply::ARRAY;
            out.elements.resize((size_t)cnt);
            for (int64_t i = 0; i < cnt; i++)
            {
                int r = parse_one(out.elements[(size_t)i]);
                if (r != 0)
                {
                    return r;
                }
            }
            return 0;
        }
        default:
            return -1;
        }
    }
};

/// 发送全部数据直至写完.
/// 返回 0 完成; 负值为 errno (含 -ETIME 超时).
static uco::task<int> send_all(uconnection *c, const std::string &data,
                               deadline &dl)
{
    size_t off = 0;
    while (off < data.size())
    {
        uco_time_t ts = {0, 0};
        if (!dl.remain(ts))
        {
            SYSWRN("uredis: operation timeout");
            co_return -ETIME;
        }
        ssize_t n = co_await usend(c->fd, data.data() + off,
                                   data.size() - off, MSG_NOSIGNAL, ts);
        if (n < 0)
        {
            if (n == -EAGAIN || n == -EINTR)
            { // uio 内部已重试, 此为防御.
                continue;
            }
            co_return (int)n;
        }
        if (n == 0)
        {
            co_return -ECONNRESET;
        }
        off += (size_t)n;
    }
    co_return 0;
}

/// 读取一个完整回复 (不足则继续收).
/// 返回 0 完成; 负值为 errno (含 -ETIME 超时, -EPROTO 协议错误).
static uco::task<int> read_reply(uconnection *c, Reply &out, deadline &dl)
{
    c->rbuf.clear();
    char tmp[4096];

    while (true)
    {
        { // 尝试从已有缓冲解析.
            parser p(c->rbuf);
            Reply r;
            int st = p.parse_one(r);
            if (st == 0)
            {
                c->rbuf.erase(0, p.pos);
                out = std::move(r);
                co_return 0;
            }
            if (st < 0)
            {
                SYSERR("uredis: protocol error");
                co_return -EPROTO;
            }
        }

        uco_time_t ts = {0, 0};
        if (!dl.remain(ts))
        {
            SYSWRN("uredis: operation timeout");
            co_return -ETIME;
        }
        ssize_t n = co_await urecv(c->fd, tmp, sizeof(tmp), 0, ts);
        if (n < 0)
        {
            co_return (int)n;
        }
        if (n == 0)
        { // 对端关闭.
            co_return -ECONNRESET;
        }
        c->rbuf.append(tmp, (size_t)n);
    }
}

} // namespace __inner__

/// 填充错误信息 (r 为负的 errno).
static void fill_error(RedisError &e, int r)
{
    if (r == -ETIME)
    {
        e.ret_code = kTimeout;
        e.err_msg = "uredis: operation timeout";
        return;
    }
    e.ret_code = r; // 传输层 errno 取负 (见 UredisError).
    e.err_msg = std::string("uredis: ") + strerror(-r);
}

bool RedisError::Retryable() const
{
    // 可重试: 超时 / 传输层错误 (连接断, 重连即可);
    // 不可重试: 服务端错误 (重试同错) / 参数非法.
    return ret_code == kTimeout ||
           (ret_code < 0 && ret_code != kBadArgument &&
            ret_code != kServerError);
}

// ==================== 连接 ====================

uco::task<uconnection *> uconnect(const char *host, unsigned int port,
                                  const char *pass, int db, uco_time_t ts)
{
    uconnection *c = new uconnection();

    // 1. TCP 建连.
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0)
    {
        SYSERR("uredis: socket failed:", strerror(errno));
        delete c;
        co_return nullptr;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (host == nullptr ||
        inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        SYSERR("uredis: invalid host (IPv4 dotted only):",
               host ? host : "(null)");
        uclose(c);
        co_return nullptr;
    }

    const int nodelay = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    int r = co_await ::uconnect(c->fd, (const struct sockaddr *)&addr,
                                sizeof(addr), ts);
    if (r != 0)
    {
        SYSERR("uredis: connect failed:", strerror(-r));
        uclose(c);
        co_return nullptr;
    }

    // 2. AUTH (可选).
    if (pass != nullptr && pass[0] != '\0')
    {
        std::vector<std::string> args;
        args.reserve(2);
        args.emplace_back("AUTH");
        args.emplace_back(pass);
        Reply rep = co_await ucommand(c, std::move(args), ts);
        if (rep.ret_code != 0)
        {
            SYSERR("uredis: auth failed:", rep.err_msg);
            uclose(c);
            co_return nullptr;
        }
    }

    // 3. SELECT db (可选).
    if (db != 0)
    {
        std::vector<std::string> args;
        args.reserve(2);
        args.emplace_back("SELECT");
        args.emplace_back(std::to_string(db));
        Reply rep = co_await ucommand(c, std::move(args), ts);
        if (rep.ret_code != 0)
        {
            SYSERR("uredis: select db failed:", rep.err_msg);
            uclose(c);
            co_return nullptr;
        }
    }

    co_return c;
}

void uclose(uconnection *c)
{
    if (c == nullptr)
    {
        return;
    }
    if (c->fd >= 0)
    {
        ::close(c->fd);
        c->fd = -1;
    }
    delete c;
}

// ==================== 命令 ====================

uco::task<Reply> ucommand(uconnection *c, std::vector<std::string> args,
                          uco_time_t ts)
{
    Reply ret;
    if (c == nullptr || c->fd < 0 || c->broken || args.empty())
    {
        ret.ret_code = kBadArgument;
        ret.err_msg = "uredis: invalid connection or empty args";
        co_return ret;
    }

    __inner__::deadline dl;
    dl.init(ts);

    // 1. 序列化下发.
    int r = co_await __inner__::send_all(c, __inner__::serialize(args), dl);
    if (r != 0)
    {
        c->broken = true;
        fill_error(ret, r);
        co_return ret;
    }

    // 2. 读取回复.
    r = co_await __inner__::read_reply(c, ret, dl);
    if (r != 0)
    {
        c->broken = true;
        fill_error(ret, r);
        co_return ret;
    }

    co_return ret;
}

uco::task<Reply> uset(uconnection *c, const std::string &key,
                      const std::string &value, uco_time_t ts)
{
    std::vector<std::string> args;
    args.reserve(3);
    args.emplace_back("SET");
    args.emplace_back(key);
    args.emplace_back(value);
    co_return co_await ucommand(c, std::move(args), ts);
}

uco::task<Reply> uget(uconnection *c, const std::string &key, uco_time_t ts)
{
    std::vector<std::string> args;
    args.reserve(2);
    args.emplace_back("GET");
    args.emplace_back(key);
    co_return co_await ucommand(c, std::move(args), ts);
}

uco::task<Reply> udel(uconnection *c, std::vector<std::string> keys,
                      uco_time_t ts)
{
    std::vector<std::string> args;
    args.reserve(keys.size() + 1);
    args.emplace_back("DEL");
    for (auto &k : keys)
    {
        args.emplace_back(std::move(k));
    }
    co_return co_await ucommand(c, std::move(args), ts);
}

uco::task<Reply> uexists(uconnection *c, std::vector<std::string> keys,
                         uco_time_t ts)
{
    std::vector<std::string> args;
    args.reserve(keys.size() + 1);
    args.emplace_back("EXISTS");
    for (auto &k : keys)
    {
        args.emplace_back(std::move(k));
    }
    co_return co_await ucommand(c, std::move(args), ts);
}

uco::task<Reply> uincr(uconnection *c, const std::string &key, uco_time_t ts)
{
    std::vector<std::string> args;
    args.reserve(2);
    args.emplace_back("INCR");
    args.emplace_back(key);
    co_return co_await ucommand(c, std::move(args), ts);
}

uco::task<Reply> uexpire(uconnection *c, const std::string &key,
                         int64_t seconds, uco_time_t ts)
{
    std::vector<std::string> args;
    args.reserve(3);
    args.emplace_back("EXPIRE");
    args.emplace_back(key);
    args.emplace_back(std::to_string(seconds));
    co_return co_await ucommand(c, std::move(args), ts);
}

uco::task<Reply> uping(uconnection *c, uco_time_t ts)
{
    std::vector<std::string> args;
    args.reserve(1);
    args.emplace_back("PING");
    co_return co_await ucommand(c, std::move(args), ts);
}

// ==================== 连接池 ====================

void upool::Init(const config &cfg)
{
    st_ = std::make_shared<state>(cfg, cfg.max_size ? cfg.max_size : 1);
    if (st_->cfg.max_size == 0)
    {
        st_->cfg.max_size = 1;
    }
    go reaper(st_);
}

upool::upool(const config &cfg)
{
    Init(cfg);
}

upool::~upool() { Close(); }

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
            co_return;
        }

        uconnection *victim = nullptr;
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
            uclose(victim); // 锁外关闭, 不得阻塞临界区.
        }
    }
}

uco::task<uconnection *> upool::Acquire()
{
    auto st = st_; // 协程帧持有状态所有权, 池析构也能安全完成.

    co_await st->permits.wait();

    uconnection *c = nullptr;

    { // 复用空闲.
        std::lock_guard<std::mutex> guard(st->mtx);
        if (st->closed)
        {
            st->permits.signal();
            co_return nullptr;
        }
        if (!st->idle.empty())
        {
            c = st->idle.front();
            st->idle.pop_front();
            co_return c;
        }
    }

    // 新建连接 (不持锁).
    c = co_await uconnect(st->cfg.host.c_str(), st->cfg.port,
                          st->cfg.pass.c_str(), st->cfg.db, st->cfg.ts);
    if (c == nullptr)
    {
        st->permits.signal();
        co_return nullptr;
    }

    { // 建连期间池可能已被关闭.
        std::lock_guard<std::mutex> guard(st->mtx);
        if (!st->closed)
        {
            st->all.insert(c);
        }
        else
        {
            uclose(c);
            st->permits.signal();
            co_return nullptr;
        }
    }
    co_return c;
}

void upool::Release(uconnection *c)
{
    if (c == nullptr)
    {
        return;
    }

    // 自动判定死活:
    //   broken: 传输层出错/超时, 流已失步 -> 销毁;
    //   其余 (服务端错误或无错误): 连接仍然健康 -> 回收复用.
    // 注: 超时未置位 broken 的情况不存在 (超时必置位),
    //     由 ucommand 统一标记, 惰性自愈.
    bool recycle = false;

    {
        std::lock_guard<std::mutex> guard(st_->mtx);
        if (!c->broken && !st_->closed)
        {
            st_->idle.emplace_back(c);
            recycle = true;
        }
        else
        {
            st_->all.erase(c);
        }
    }
    st_->permits.signal();

    if (!recycle)
    {
        uclose(c); // 锁外关闭, 不得阻塞临界区.
    }
}

void upool::Close()
{
    std::vector<uconnection *> victims;
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
    SYSMSG("uredis: pool close, conns:", victims.size());
    for (auto c : victims)
    {
        uclose(c); // 锁外关闭, 不得阻塞临界区.
    }
}

// ==================== 分布式锁 ====================

namespace __inner__
{

/// 生成持有者唯一令牌 (48 字符 hex: 时间 + 线程 + 随机 + 序列).
static std::string gen_token()
{
    static std::atomic<uint64_t> seq{0};
    const uint64_t v[3] = {
        (uint64_t)std::chrono::steady_clock::now().time_since_epoch()
            .count(),
        ((uint64_t)gettid() << 32) ^ (uint64_t)(uintptr_t)&seq,
        ((uint64_t)std::random_device{}() << 32) ^
            (seq.fetch_add(1) + 0x9e3779b97f4a7c15ULL),
    };
    char buf[49];
    snprintf(buf, sizeof(buf), "%016llx%016llx%016llx",
             (unsigned long long)v[0], (unsigned long long)v[1],
             (unsigned long long)v[2]);
    return buf;
}

} // namespace __inner__

/// 释放脚本: 仅当锁仍为自己持有时删除 (原子, 防误删他人锁).
static const char *ULOCK_RELEASE_LUA =
    "if redis.call('get', KEYS[1]) == ARGV[1] then "
    "return redis.call('del', KEYS[1]) "
    "else return 0 end";

/// 续期脚本: 仅当锁仍为自己持有时续期.
static const char *ULOCK_RENEW_LUA =
    "if redis.call('get', KEYS[1]) == ARGV[1] then "
    "return redis.call('pexpire', KEYS[1], ARGV[2]) "
    "else return 0 end";

ulock::ulock(const config &cfg) : st_(std::make_shared<state>())
{
    st_->cfg = cfg;
    if (st_->cfg.ttl_ms == 0)
    {
        SYSWRN("uredis: ulock ttl_ms == 0, fallback to 10s");
        st_->cfg.ttl_ms = 10'000;
    }
    if (st_->cfg.renew_interval_ms != 0 &&
        st_->cfg.renew_interval_ms >= st_->cfg.ttl_ms)
    { // 续期间隔必须小于 TTL, 否则锁可能在续期前过期.
        st_->cfg.renew_interval_ms = st_->cfg.ttl_ms / 3;
        SYSWRN("uredis: ulock renew_interval_ms >= ttl_ms, clamp to ttl/3");
    }
    if (st_->cfg.renew_interval_ms != 0)
    {
        st_->watchdog_armed = true;
        st_->waker = std::make_unique<uco::usleeper>(); // 看门狗的睡眠定时器.
        go watchdog(st_);
    }
}

ulock::~ulock() { Close(); }

/// 后台看门狗: 每 renew_interval_ms 续期一次.
/// 睡在可取消定时器上, close() 经 waker->wake() 立即唤醒 (微秒级),
/// 无需轮询 stopped 标志, 不拖垮进程收尾.
/// 帧内持有 state 的 shared_ptr, 锁析构后仍可安全访问并自行退出;
/// 锁停止时由看门狗负责关闭连接 (此刻无进行中的命令).
uco::task<void> ulock::watchdog(std::shared_ptr<state> st)
{
    using namespace std::chrono;
    const auto interval = milliseconds(st->cfg.renew_interval_ms);
    while (true)
    {
        co_await st->waker->sleep_for(interval);
        if (st->stopped)
        {
            uconnection *victim = nullptr;
            {
                std::lock_guard<std::mutex> guard(st->token_mtx);
                victim = st->c;
                st->c = nullptr;
            }
            uclose(victim);
            co_return;
        }

        {
            std::lock_guard<std::mutex> guard(st->token_mtx);
            if (st->token.empty())
            {
                continue; // 未持有.
            }
        }

        if (!co_await do_renew(st))
        {
            SYSWRN("uredis: lock renew failed or lost, key:", st->cfg.key);
        }
    }
}

/// 续期实现 (持 cmd_mtx, 与看门狗/用户命令串行).
uco::task<bool> ulock::do_renew(std::shared_ptr<state> st)
{
    auto guard = co_await uco::ulock_guard(st->cmd_mtx);
    if (st->stopped)
    {
        co_return false;
    }

    std::string token;
    {
        std::lock_guard<std::mutex> g(st->token_mtx);
        token = st->token;
    }
    if (token.empty())
    {
        co_return false; // 未持有.
    }

    if (st->c == nullptr || st->c->broken)
    { // 连接不可用: 标记未持有, 服务端靠 TTL 兜底.
        std::lock_guard<std::mutex> g(st->token_mtx);
        st->token.clear();
        co_return false;
    }

    std::vector<std::string> args;
    args.reserve(6);
    args.emplace_back("EVAL");
    args.emplace_back(ULOCK_RENEW_LUA);
    args.emplace_back("1");
    args.emplace_back(st->cfg.key);
    args.emplace_back(token);
    args.emplace_back(std::to_string(st->cfg.ttl_ms));
    Reply r = co_await ucommand(st->c, std::move(args), st->cfg.ts);
    bool ok = r.ret_code == 0 && r.type == Reply::INTEGER && r.integer == 1;
    if (!ok)
    { // 锁已丢失 (过期被他人抢走) 或命令失败: 标记未持有.
        std::lock_guard<std::mutex> g(st->token_mtx);
        st->token.clear();
    }
    co_return ok;
}

uco::task<bool> ulock::TryAcquire()
{
    auto guard = co_await uco::ulock_guard(st_->cmd_mtx);
    if (st_->stopped)
    {
        co_return false;
    }
    if (st_->cfg.key.empty())
    {
        SYSERR("uredis: ulock key is empty");
        co_return false;
    }

    { // 已持有则直接成功 (幂等).
        std::lock_guard<std::mutex> g(st_->token_mtx);
        if (!st_->token.empty())
        {
            co_return true;
        }
    }

    // 确保连接可用 (断线自动重建).
    if (st_->c == nullptr || st_->c->broken)
    {
        uconnection *dead = st_->c;
        st_->c = nullptr;
        if (dead != nullptr)
        {
            uclose(dead);
        }
        st_->c = co_await uconnect(st_->cfg.host.c_str(), st_->cfg.port,
                                   st_->cfg.pass.c_str(), st_->cfg.db,
                                   st_->cfg.ts);
        if (st_->c == nullptr)
        {
            co_return false;
        }
    }

    // SET key token NX PX ttl (原子加锁).
    std::string token = __inner__::gen_token();
    std::vector<std::string> args;
    args.reserve(6);
    args.emplace_back("SET");
    args.emplace_back(st_->cfg.key);
    args.emplace_back(token);
    args.emplace_back("NX");
    args.emplace_back("PX");
    args.emplace_back(std::to_string(st_->cfg.ttl_ms));
    Reply r = co_await ucommand(st_->c, std::move(args), st_->cfg.ts);
    if (r.ret_code != 0 || r.type != Reply::STATUS || r.str != "OK")
    { // NIL: 被他人持有; 其余: 命令失败.
        co_return false;
    }

    {
        std::lock_guard<std::mutex> g(st_->token_mtx);
        st_->token = std::move(token);
    }
    co_return true;
}

uco::task<bool> ulock::Acquire(uco_time_t ts)
{
    if (st_->cfg.retry_interval_ms == 0)
    {
        co_return co_await TryAcquire();
    }

    __inner__::deadline dl;
    dl.init(ts);

    while (true)
    {
        if (co_await TryAcquire())
        {
            co_return true;
        }

        uco_time_t remain = {0, 0};
        if (!dl.remain(remain))
        {
            co_return false;
        }
        uint64_t wait_ms = st_->cfg.retry_interval_ms;
        if (dl.has)
        {
            uint64_t remain_ms =
                (uint64_t)remain.tv_sec * 1000 + remain.tv_nsec / 1'000'000;
            if (remain_ms == 0)
            {
                co_return false;
            }
            if (wait_ms > remain_ms)
            {
                wait_ms = remain_ms;
            }
        }
        co_await uco_nanosleep(0, wait_ms * 1'000'000);
    }
}

uco::task<bool> ulock::Release()
{
    auto guard = co_await uco::ulock_guard(st_->cmd_mtx);
    if (st_->stopped)
    {
        co_return false;
    }

    std::string token;
    {
        std::lock_guard<std::mutex> g(st_->token_mtx);
        token = st_->token;
        st_->token.clear(); // 先表达释放意图, 命令失败由 TTL 兜底.
    }
    if (token.empty())
    {
        co_return false; // 本就未持有.
    }

    if (st_->c == nullptr || st_->c->broken)
    {
        co_return false; // 无法通知服务端, 靠 TTL 过期.
    }

    // EVAL: 仅当锁仍为自己持有时删除.
    std::vector<std::string> args;
    args.reserve(5);
    args.emplace_back("EVAL");
    args.emplace_back(ULOCK_RELEASE_LUA);
    args.emplace_back("1");
    args.emplace_back(st_->cfg.key);
    args.emplace_back(token);
    Reply r = co_await ucommand(st_->c, std::move(args), st_->cfg.ts);
    co_return r.ret_code == 0 && r.type == Reply::INTEGER && r.integer == 1;
}

uco::task<bool> ulock::ReNew() { co_return co_await do_renew(st_); }

bool ulock::Owns() const
{
    std::lock_guard<std::mutex> guard(st_->token_mtx);
    return !st_->token.empty();
}

void ulock::Close()
{
    st_->stopped = true;
    {
        std::lock_guard<std::mutex> guard(st_->token_mtx);
        st_->token.clear();
    }
    if (!st_->watchdog_armed)
    { // 无看门狗: 此刻无并发使用者, 直接断开.
        uconnection *victim = st_->c;
        st_->c = nullptr;
        if (victim != nullptr)
        {
            uclose(victim);
        }
    }
    else
    { // 有看门狗: 立即唤醒, 由其感知 stopped 后关闭连接并退出.
        if (st_->waker)
        {
            st_->waker->wake();
        }
    }
}

} // namespace uredis
