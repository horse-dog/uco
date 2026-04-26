#pragma once
#include "uco.h"
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <liburing.h>
#include <chrono>

using uco_time_t = struct __kernel_timespec;

uco::task<void> ucancel(int fd, uco_time_t ts = {0, 0});

uco::task<ssize_t> urecv(int fd, void *buf, size_t n, int flags,
                         uco_time_t ts = {0, 0});

uco::task<ssize_t> uread(int fd, void *buf, size_t nbytes, uint64_t offset = 0,
                         uco_time_t ts = {0, 0});

uco::task<ssize_t> ureadv(int fd, const struct iovec *iovec, int count,
                          uint64_t offset = 0, uco_time_t ts = {0, 0});

uco::task<ssize_t> usend(int fd, const void *buf, size_t n, int flags,
                         uco_time_t ts = {0, 0});

uco::task<ssize_t> uwrite(int fd, const void *buf, size_t nbytes,
                          uint64_t offset = 0, uco_time_t ts = {0, 0});

uco::task<ssize_t> uwritev(int fd, const struct iovec *iovec, int count,
                           uint64_t offset = 0, uco_time_t ts = {0, 0});

uco::task<ssize_t> urecvmsg(int fd, struct msghdr *message, int flags,
                            uco_time_t ts = {0, 0});

uco::task<ssize_t> usendmsg(int fd, const struct msghdr *message, int flags,
                            uco_time_t ts = {0, 0});

uco::task<int> uconnect(int fd, const struct sockaddr *addr, socklen_t len,
                        uco_time_t ts = {0, 0});

uco::task<int> uaccept(int sockfd, struct sockaddr *addr, socklen_t *addrlen,
                       int flags = 0, uco_time_t ts = {0, 0});

uco::task<int> uopen(const char *file, int oflag, mode_t mode = 0,
                     uco_time_t ts = {0, 0});

uco::task<int> ustat(int dfd, const char *path, int flags,
                     struct statx *statxbuf, uco_time_t ts = {0, 0});

uco::task<int> ustat(const char *file, struct statx *buf);

uco::task<int> ufstat(int fd, struct statx *buf);

uco::task<int> ustatx(int dirfd, const char *__restrict path, int flags,
                      unsigned int mask, struct statx *__restrict buf,
                      uco_time_t ts = {0, 0});

uco::task<ssize_t> usendfile(int out_fd, int in_fd, off_t *offset, size_t count,
                             uco_time_t ts = {0, 0});

uco::task<int> ushutdown(int fd, int how, uco_time_t ts = {0, 0});

uco::task<int> ufsync(int fd, uco_time_t ts = {0, 0});

uco::task<int> ufdatasync(int fd, uco_time_t ts = {0, 0});

uco::task<int> umkdir(const char* path, mode_t mode, uco_time_t ts = {0, 0});

uco::task<int> mkdirat(int fd, const char *path, mode_t mode, uco_time_t ts = {0, 0});

uco::task<int> uclose(int fd, uco_time_t ts = {0, 0});

uco::task<int> ufcntl(int fd, int cmd, ...);

uco::task<void> uco_nanosleep(uint64_t sec, uint64_t nsec);

template <typename Rep, typename Period>
uco::task<void> uco_sleep(const std::chrono::duration<Rep, Period> &d)
{
    using namespace std::chrono;
    auto ns = duration_cast<nanoseconds>(d);
    long long sec = ns.count() / 1'000'000'000;
    long nsec = static_cast<long>(ns.count() % 1'000'000'000);
    co_await uco_nanosleep(sec, nsec);
}
