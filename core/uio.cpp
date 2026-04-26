#define _UCO_THREAD_ENV_IMPL
#include "uio.h"
#include "uco.h"
#include "ulog.h"
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <liburing.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

static uco_time_t advance_ts(uco_time_t ts, int64_t ns)
{
    int64_t sec = ns / 1'000'000'000LL;
    int64_t nsec = ns % 1'000'000'000LL;

    ts.tv_sec -= sec;
    ts.tv_nsec -= nsec;

    if (ts.tv_nsec < 0)
    {
        ts.tv_sec -= 1;
        ts.tv_nsec += 1'000'000'000LL;
    }

    if (ts.tv_sec < 0)
    {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    }
    return ts;
}

#define UCOENV uco::__inner__::thread_co_env::GetInstance()
#define WAIT_SQE_LIST                                                          \
    ((uco::__inner__::uco_linked_list *)(UCOENV.wait_sqe_list))

#define CHECK_SQE(sqe)                                                         \
    if (sqe == nullptr)                                                        \
    {                                                                          \
        this->p->set_value(-EAGAIN);                                           \
        WAIT_SQE_LIST->push(                                                   \
            (uco::task<void>::promise_type *)(&(h.promise())));                \
        return true;                                                           \
    }

#define CHECK_SQE2(sqe, sqe_pre)                                               \
    if (sqe == nullptr)                                                        \
    {                                                                          \
        this->p->set_value(-EAGAIN);                                           \
        WAIT_SQE_LIST->push(                                                   \
            (uco::task<void>::promise_type *)(&(h.promise())));                \
        sqe_pre->user_data = 0;                                                \
        io_uring_prep_nop(sqe_pre);                                            \
        return true;                                                           \
    }

#define UR_FUNC_PRE_COMM0(retType, op, ...)                                    \
    struct awaitable                                                           \
    {                                                                          \
        auto await_ready() const noexcept                                      \
        {                                                                      \
            SYSDBG("await_ready ? false");                                     \
            return false;                                                      \
        }                                                                      \
        auto await_suspend(uco::task<retType>::coro_handle h) noexcept         \
        {                                                                      \
            SYSDBG("await_suspend:", #op, ',', "handle:", h.address());        \
            auto &&p = h.promise();                                            \
            this->p = &p;                                                      \
            auto sqe = UCOENV.get_sqe();                                       \
            CHECK_SQE(sqe);                                                    \
            io_uring_prep_##op(sqe, __VA_ARGS__);                              \
            sqe->user_data = (uint64_t)(h.address());                          \
            if (ts.tv_sec != 0 || ts.tv_nsec != 0)                             \
            {                                                                  \
                sqe->flags |= IOSQE_IO_LINK;                                   \
                auto sqe1 = UCOENV.get_sqe();                                  \
                CHECK_SQE2(sqe1, sqe);                                         \
                io_uring_prep_link_timeout(sqe1, &ts, 0);                      \
                sqe1->user_data = 0;                                           \
            }                                                                  \
            return true;                                                       \
        }                                                                      \
        auto await_resume() noexcept                                           \
        {                                                                      \
            SYSDBG("await_resume:", #op);                                      \
            return p->value();                                                 \
        }

#define UR_FUNC_PRE_COMM1(retType, op, ...)                                    \
    struct awaitable                                                           \
    {                                                                          \
        auto await_ready() const noexcept                                      \
        {                                                                      \
            SYSDBG("await_ready ? false");                                     \
            return false;                                                      \
        }                                                                      \
        auto await_suspend(uco::task<retType>::coro_handle h) noexcept         \
        {                                                                      \
            SYSDBG("await_suspend:", #op, ',', "handle:", h.address());        \
            auto &&p = h.promise();                                            \
            this->p = &p;                                                      \
            auto sqe = UCOENV.get_sqe();                                       \
            CHECK_SQE(sqe);                                                    \
            io_uring_prep_##op(sqe, __VA_ARGS__, 0);                           \
            sqe->user_data = (uint64_t)(h.address());                          \
            if (ts.tv_sec != 0 || ts.tv_nsec != 0)                             \
            {                                                                  \
                sqe->flags |= IOSQE_IO_LINK;                                   \
                auto sqe1 = UCOENV.get_sqe();                                  \
                CHECK_SQE2(sqe1, sqe);                                         \
                io_uring_prep_link_timeout(sqe1, &ts, 0);                      \
                sqe1->user_data = 0;                                           \
            }                                                                  \
            return true;                                                       \
        }                                                                      \
        auto await_resume() noexcept                                           \
        {                                                                      \
            SYSDBG("await_resume:", #op);                                      \
            return p->value();                                                 \
        }

#define UR_FUNC_POST_COMM0(retType, ...)                                       \
    uco_time_t ts{};                                                           \
    uco::task<retType>::promise_type *p = 0;                                   \
    }                                                                          \
    ;                                                                          \
    uint64_t res = 0;                                                          \
    bool first = true;                                                         \
    using namespace std::chrono;                                               \
    steady_clock::time_point tpoint;                                           \
    do                                                                         \
    {                                                                          \
        if (ts.tv_sec != 0 || ts.tv_nsec != 0)                                 \
        {                                                                      \
            if (first == false)                                                \
            {                                                                  \
                auto now = steady_clock::now();                                \
                auto dr = duration_cast<nanoseconds>(now - tpoint).count();    \
                ts = advance_ts(ts, dr);                                       \
                if (ts.tv_sec == 0 && ts.tv_nsec == 0)                         \
                {                                                              \
                    SYSERR("timeout");                                         \
                    res = -ETIME;                                              \
                    break;                                                     \
                }                                                              \
            }                                                                  \
            first = false;                                                     \
            tpoint = steady_clock::now();                                      \
        }                                                                      \
        res = co_await awaitable(__VA_ARGS__, ts);                             \
    } while (res == -EAGAIN || res == -EINTR);                                 \
    co_return static_cast<retType>(res);

#define UR_FUNC_POST_COMM1(retType, ...)                                       \
    uco_time_t ts{};                                                           \
    uco::task<retType>::promise_type *p = 0;                                   \
    }                                                                          \
    ;                                                                          \
    uint64_t res = 0;                                                          \
    bool first = true;                                                         \
    using namespace std::chrono;                                               \
    steady_clock::time_point tpoint;                                           \
    do                                                                         \
    {                                                                          \
        if (ts.tv_sec != 0 || ts.tv_nsec != 0)                                 \
        {                                                                      \
            if (first == false)                                                \
            {                                                                  \
                auto now = steady_clock::now();                                \
                auto dr = duration_cast<nanoseconds>(now - tpoint).count();    \
                ts = advance_ts(ts, dr);                                       \
                if (ts.tv_sec == 0 && ts.tv_nsec == 0)                         \
                {                                                              \
                    SYSERR("timeout");                                         \
                    res = -ETIME;                                              \
                    break;                                                     \
                }                                                              \
            }                                                                  \
            first = false;                                                     \
            tpoint = steady_clock::now();                                      \
        }                                                                      \
        res = co_await awaitable(__VA_ARGS__, ts);                             \
    } while (res == -EAGAIN || res == -EINTR);                                 \
    co_return;

#define UR_FUNC_POST_COMM2(retType, ...)                                       \
    uco_time_t ts{};                                                           \
    uco::task<retType>::promise_type *p = 0;                                   \
    }                                                                          \
    ;                                                                          \
    uint64_t res = 0;                                                          \
    bool first = true;                                                         \
    using namespace std::chrono;                                               \
    steady_clock::time_point tpoint;                                           \
    do                                                                         \
    {                                                                          \
        if (ts.tv_sec != 0 || ts.tv_nsec != 0)                                 \
        {                                                                      \
            if (first == false)                                                \
            {                                                                  \
                auto now = steady_clock::now();                                \
                auto dr = duration_cast<nanoseconds>(now - tpoint).count();    \
                ts = advance_ts(ts, dr);                                       \
                if (ts.tv_sec == 0 && ts.tv_nsec == 0)                         \
                {                                                              \
                    SYSWRN("timeout");                                         \
                    res = -ETIME;                                              \
                    break;                                                     \
                }                                                              \
            }                                                                  \
            first = false;                                                     \
            tpoint = steady_clock::now();                                      \
        }                                                                      \
        res = co_await awaitable(ts);                                          \
    } while (res == -EAGAIN || res == -EINTR);                                 \
    co_return;

uco::task<void> ucancel(int fd, uco_time_t ts)
{
    UR_FUNC_PRE_COMM1(void, cancel_fd, fd);
    int fd = 0;
    UR_FUNC_POST_COMM1(void, fd);
    co_return;
}

uco::task<ssize_t> urecv(int fd, void *buf, size_t n, int flags, uco_time_t ts)
{
    UR_FUNC_PRE_COMM0(ssize_t, recv, fd, buf, n, flags);
    int fd = 0;
    int flags = 0;
    void *buf = 0;
    size_t n = 0;
    UR_FUNC_POST_COMM0(ssize_t, fd, flags, buf, n);
}

uco::task<ssize_t> uread(int fd, void *buf, size_t nbytes, uint64_t offset,
                         uco_time_t ts)
{
    // 1. submit read to io_uring.
    // 2. await_suspend.
    // 3. io_uring complete, resume coroutine.
    UR_FUNC_PRE_COMM0(ssize_t, read, fd, buf, nbytes, offset);
    int fd = 0;
    void *buf = 0;
    size_t nbytes = 0;
    uint64_t offset = 0;
    UR_FUNC_POST_COMM0(ssize_t, fd, buf, nbytes, offset);
}

uco::task<ssize_t> ureadv(int fd, const struct iovec *iovec, int count,
                          uint64_t offset, uco_time_t ts)
{
    UR_FUNC_PRE_COMM0(ssize_t, readv, fd, iovec, count, offset);
    int fd = 0;
    int count = 0;
    uint64_t offset = 0;
    const struct iovec *iovec = 0;
    UR_FUNC_POST_COMM0(ssize_t, fd, count, offset, iovec);
}

uco::task<ssize_t> usend(int fd, const void *buf, size_t n, int flags,
                         uco_time_t ts)
{
    UR_FUNC_PRE_COMM0(ssize_t, send, fd, buf, n, flags);
    int fd = 0;
    int flags = 0;
    const void *buf = 0;
    size_t n = 0;
    UR_FUNC_POST_COMM0(ssize_t, fd, flags, buf, n);
}

uco::task<ssize_t> uwrite(int fd, const void *buf, size_t nbytes,
                          uint64_t offset, uco_time_t ts)
{
    UR_FUNC_PRE_COMM0(ssize_t, write, fd, buf, nbytes, offset);
    int fd = 0;
    const void *buf = 0;
    size_t nbytes = 0;
    uint64_t offset = 0;
    UR_FUNC_POST_COMM0(ssize_t, fd, buf, nbytes, offset);
}

uco::task<ssize_t> uwritev(int fd, const struct iovec *iovec, int count,
                           uint64_t offset, uco_time_t ts)
{
    UR_FUNC_PRE_COMM0(ssize_t, writev, fd, iovec, count, offset);
    int fd = 0;
    int count = 0;
    const struct iovec *iovec = 0;
    uint64_t offset = 0;
    UR_FUNC_POST_COMM0(ssize_t, fd, count, iovec, offset);
}

uco::task<ssize_t> urecvmsg(int fd, struct msghdr *message, int flags,
                            uco_time_t ts)
{
    UR_FUNC_PRE_COMM0(ssize_t, recvmsg, fd, message, flags);
    int fd = 0;
    int flags = 0;
    struct msghdr *message = 0;
    UR_FUNC_POST_COMM0(ssize_t, fd, flags, message);
}

uco::task<ssize_t> usendmsg(int fd, const struct msghdr *message, int flags,
                            uco_time_t ts)
{
    UR_FUNC_PRE_COMM0(ssize_t, sendmsg, fd, message, flags);
    int fd = 0;
    int flags = 0;
    const struct msghdr *message = 0;
    UR_FUNC_POST_COMM0(ssize_t, fd, flags, message);
}

uco::task<int> uconnect(int fd, const struct sockaddr *addr, socklen_t len,
                        uco_time_t ts)
{
    UR_FUNC_PRE_COMM0(int, connect, fd, addr, len);
    int fd = 0;
    const struct sockaddr *addr = 0;
    socklen_t len = 0;
    UR_FUNC_POST_COMM0(int, fd, addr, len);
}

uco::task<int> uaccept(int sockfd, struct sockaddr *addr, socklen_t *addrlen,
                       int flags, uco_time_t ts)
{
    UR_FUNC_PRE_COMM0(int, accept, sockfd, addr, addrlen, flags);
    int sockfd = 0;
    int flags = 0;
    struct sockaddr *addr = 0;
    socklen_t *addrlen = 0;
    UR_FUNC_POST_COMM0(int, sockfd, flags, addr, addrlen);
}

uco::task<int> uopen(const char *pathname, int flags, mode_t mode,
                     uco_time_t ts)
{
    const int dfd = AT_FDCWD;
    UR_FUNC_PRE_COMM0(int, openat, dfd, pathname, flags, mode);
    int dfd = 0;
    int flags = 0;
    const char *pathname = 0;
    mode_t mode = 0;
    UR_FUNC_POST_COMM0(int, dfd, flags, pathname, mode);
}

uco::task<int> uclose(int fd, uco_time_t ts)
{
    UR_FUNC_PRE_COMM0(int, close, fd);
    int fd = 0;
    UR_FUNC_POST_COMM0(int, fd);
}

uco::task<int> ustatx(int dirfd, const char *path, int flags, unsigned int mask,
                      struct statx *buf, uco_time_t ts)
{
    UR_FUNC_PRE_COMM0(int, statx, dirfd, path, flags, mask, buf);
    int dirfd = 0;
    int flags = 0;
    unsigned int mask = 0;
    const char *path = 0;
    struct statx *buf = 0;
    UR_FUNC_POST_COMM0(int, dirfd, flags, mask, path, buf);
}

uco::task<int> ustat(const char *file, struct statx *buf)
{
    int ret = co_await ustatx(AT_FDCWD, file, 0, STATX_BASIC_STATS, buf);
    co_return ret;
}

uco::task<int> ufstat(int fd, struct statx *buf)
{
    int ret = co_await ustatx(fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, buf);
    co_return ret;
}

static uco::task<void> __unanosleep(uco_time_t ts)
{ // 🤡
    using namespace uco::__inner__;
    struct awaitable
    {
        auto await_ready() const noexcept
        {
            SYSDBG("await_ready ? false");
            return false;
        }
        auto await_suspend(uco::task<void>::coro_handle h) noexcept
        {
            SYSDBG("await_suspend, handle:", h.address());
            auto &&p = h.promise();
            this->p = &p;
            if (ts.tv_sec < 0 || (ts.tv_sec == 0 && ts.tv_nsec <= 0))
                return false;
            auto sqe = UCOENV.get_sqe();
            CHECK_SQE(sqe);
            io_uring_prep_timeout(sqe, &ts, 0, 0);
            sqe->user_data = (uint64_t)(h.address());
            return true;
        }
        auto await_resume() noexcept
        {
            SYSDBG("await_resume");
            return p->value();
        }
        UR_FUNC_POST_COMM2(void);
}

uco::task<void> uco_nanosleep(uint64_t sec, uint64_t nsec)
{
    co_await __unanosleep({(decltype(uco_time_t{}.tv_sec))sec,
                           (decltype(uco_time_t{}.tv_nsec))nsec});
    co_return;
}

uco::task<ssize_t> usendfile(int out_fd, int in_fd, off_t *offset, size_t count,
                             uco_time_t ts)
{
    using namespace uco::__inner__;
    struct awaitable
    {
        auto await_ready() const noexcept
        {
            SYSDBG("await_ready ? false");
            return false;
        }
        auto await_suspend(uco::task<ssize_t>::coro_handle h) noexcept
        {
            SYSDBG("await_suspend, handle:", h.address());
            auto &&p = h.promise();
            this->p = &p;
            auto sqe = thread_co_env::GetInstance().get_sqe();
            CHECK_SQE(sqe);
            io_uring_prep_splice(sqe, in_fd, cur, pipe1, -1, count, 0);
            sqe->user_data = (uint64_t)(h.address());
            sqe->opcode = IORING_OP_SPLICE;
            if (ts.tv_sec != 0 || ts.tv_nsec != 0)
            {
                sqe->flags |= IOSQE_IO_LINK;
                auto sqe1 = thread_co_env::GetInstance().get_sqe();
                CHECK_SQE2(sqe1, sqe);
                io_uring_prep_link_timeout(sqe1, &ts, 0);
                sqe1->user_data = 0;
            }
            return true;
        }
        auto await_resume() noexcept
        {
            SYSDBG("await_resume");
            return p->value();
        }

        int in_fd = 0;
        int pipe1 = 0;
        off_t cur = 0;
        size_t count = 0;
        uco_time_t ts = {0, 0};
        uco::task<ssize_t>::promise_type *p = 0;
    };

    struct awaitable1
    {
        auto await_ready() const noexcept
        {
            SYSDBG("await_ready ? false");
            return false;
        }
        auto await_suspend(uco::task<ssize_t>::coro_handle h) noexcept
        {
            SYSDBG("await_suspend, handle:", h.address());
            auto &&p = h.promise();
            this->p = &p;
            auto sqe = thread_co_env::GetInstance().get_sqe();
            CHECK_SQE(sqe);
            io_uring_prep_splice(sqe, pipe0, -1, out_fd, -1, nbytes, 0);
            sqe->user_data = (uint64_t)(h.address());
            sqe->opcode = IORING_OP_SPLICE;
            if (ts.tv_sec != 0 || ts.tv_nsec != 0)
            {
                sqe->flags |= IOSQE_IO_LINK;
                auto sqe1 = thread_co_env::GetInstance().get_sqe();
                CHECK_SQE2(sqe1, sqe);
                io_uring_prep_link_timeout(sqe1, &ts, 0);
                sqe1->user_data = 0;
            }
            return true;
        }
        auto await_resume() noexcept
        {
            SYSDBG("await_resume");
            return p->value();
        }

        int out_fd = 0;
        int pipe0 = 0;
        ssize_t nbytes = 0;
        uco_time_t ts = {0, 0};
        uco::task<ssize_t>::promise_type *p = 0;
    };

    if (count == 0)
        co_return 0;

    ssize_t res = 0;
    int pipefd[2];
    res = pipe(pipefd);
    if (res != 0)
    {
        SYSERR("pipe:", strerror(errno));
        co_return -errno;
    }

    if (count >= 1024 * 256)
    {
        res = fcntl(pipefd[0], F_SETPIPE_SZ, 1024 * 256);
        // fcntl error is ok, we can continue.
    }

    auto remain = count;
    ssize_t cur = 0;
    off_t off = *offset;

    bool first = true;
    using namespace std::chrono;
    steady_clock::time_point tpoint;

    while (remain > 0)
    {
        ssize_t m = 0;
        do
        {
            if (ts.tv_sec != 0 || ts.tv_nsec != 0)
            {
                if (first == false)
                {
                    auto now = steady_clock::now();
                    auto dr = duration_cast<nanoseconds>(now - tpoint).count();
                    ts = advance_ts(ts, dr);
                    if (ts.tv_sec == 0 && ts.tv_nsec == 0)
                    { // already timeout.
                        errno = ETIME;
                        co_await uclose(pipefd[0]);
                        co_await uclose(pipefd[1]);
                        co_return (cur != 0) ? cur : -ETIME;
                    }
                }
                first = false;
                tpoint = steady_clock::now();
            }
            m = co_await awaitable{in_fd, pipefd[1], cur + off, count, ts};
        } while (m == -EAGAIN || m == -EINTR);
        if (m == 0)
        {
            co_await uclose(pipefd[0]);
            co_await uclose(pipefd[1]);
            co_return cur;
        }
        if (m < 0)
        {
            SYSERR("splice in_fd(", in_fd ,") -> pipe[1]:", strerror(-m));
            co_await uclose(pipefd[0]);
            co_await uclose(pipefd[1]);
            errno = (-m);
            co_return (cur != 0) ? cur : m;
        }

        ssize_t written = 0;
        while (written < m)
        {
            ssize_t k = 0;
            do
            {
                if (ts.tv_sec != 0 || ts.tv_nsec != 0)
                {
                    auto now = steady_clock::now();
                    auto dr = duration_cast<nanoseconds>(now - tpoint).count();
                    ts = advance_ts(ts, dr);
                    if (ts.tv_sec == 0 && ts.tv_nsec == 0)
                    { // already timeout.
                        errno = ETIME;
                        co_await uclose(pipefd[0]);
                        co_await uclose(pipefd[1]);
                        co_return (cur != 0) ? cur : -ETIME;
                    }
                    tpoint = steady_clock::now();
                }
                k = co_await awaitable1{out_fd, pipefd[0], m - written, ts};
            } while (k == -EAGAIN || k == -EINTR);
            if (k == 0)
            {
                co_await uclose(pipefd[0]);
                co_await uclose(pipefd[1]);
                co_return cur;
            }
            if (k < 0)
            {
                // EPIPE means client closed.
                if (k == -EPIPE || k == -ECONNRESET)
                {
                    SYSDBG("splice pipe[0] -> out_fd:", out_fd, strerror(-k), ',',
                            NR(cur), NR(m), NR(k));
                }
                else
                {
                    SYSERR("splice pipe[0] -> out_fd:", out_fd, strerror(-k), ',',
                            NR(cur), NR(m), NR(k));
                }
                co_await uclose(pipefd[0]);
                co_await uclose(pipefd[1]);
                errno = (-k);
                co_return (cur != 0) ? cur : k;
            }
            cur += k;
            *offset += k;
            written += k;
        }
        remain -= m;
    }

    co_await uclose(pipefd[0]);
    co_await uclose(pipefd[1]);
    co_return cur;
}


uco::task<int> ushutdown(int fd, int how, uco_time_t ts)
{
    UR_FUNC_PRE_COMM0(int, shutdown, fd, how);
    int fd = 0;
    int how = 0;
    UR_FUNC_POST_COMM0(int, fd, how);
}

uco::task<int> ufsync(int fd, uco_time_t ts)
{
    int flag = 0;
    UR_FUNC_PRE_COMM0(int, fsync, fd, flag);
    int fd = 0;
    int flag = 0;
    UR_FUNC_POST_COMM0(int, fd, flag);
}

uco::task<int> ufdatasync(int fd, uco_time_t ts)
{
    int flag = IORING_FSYNC_DATASYNC;
    UR_FUNC_PRE_COMM0(int, fsync, fd, flag);
    int fd = 0;
    int flag = 0;
    UR_FUNC_POST_COMM0(int, fd, flag);
}

uco::task<int> umkdir(const char* path, mode_t mode, uco_time_t ts)
{
    UR_FUNC_PRE_COMM0(int, mkdir, path, mode);
    const char* path = 0;
    mode_t mode = 0;
    UR_FUNC_POST_COMM0(int, path, mode);
}

uco::task<int> mkdirat(int fd, const char *path, mode_t mode, uco_time_t ts)
{
    UR_FUNC_PRE_COMM0(int, mkdirat, fd, path, mode);
    int fd = 0;
    const char* path = 0;
    mode_t mode = 0;
    UR_FUNC_POST_COMM0(int, fd, path, mode);
}
