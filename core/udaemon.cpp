#include "udaemon.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace uco
{

/**
 * @brief 进程初始化 RAII 单例类：可选 daemonize + 可选进程单例（flock）
 *
 * 构造时完成初始化，析构时自动释放文件锁（如果有）。
 * 调用 ok() 检查初始化是否成功。
 * Meyers' SingleTon，全局唯一实例。
 */
class ProcessInit
{
  public:
    /**
     * 获取唯一实例。首次调用时执行初始化，后续调用返回同一实例。
     *
     * @param daemonize  是否以守护进程方式运行 (fork + setsid)
     * @param lock_name  文件锁名称，用于保证进程单例。
     *                   为空字符串则跳过单例检查，允许多实例运行。
     *                   非空时使用 /tmp/<lock_name>.lock 作为 flock 文件。
     */
    static ProcessInit &GetInstance(bool daemonize = false,
                                    const std::string &lock_name = "");

    // 禁止拷贝和移动
    ProcessInit(const ProcessInit &) = delete;
    ProcessInit &operator=(const ProcessInit &) = delete;
    ProcessInit(ProcessInit &&) = delete;
    ProcessInit &operator=(ProcessInit &&) = delete;

  private:
    ProcessInit(bool daemonize, const std::string &lock_name);
    ~ProcessInit();

    int lock_fd_ = -1;
};

ProcessInit &ProcessInit::GetInstance(bool daemonize,
                                      const std::string &lock_name)
{
    static ProcessInit instance(daemonize, lock_name);
    return instance;
}

ProcessInit::ProcessInit(bool daemonize, const std::string &lock_name)
{
    // 1) 可选：进程单例 (flock)
    if (!lock_name.empty())
    {
        std::string lock_path = "/tmp/" + lock_name + ".lock";
        lock_fd_ = open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
        if (lock_fd_ < 0)
        {
            printf("[udaemon] open lock file %s failed: %s\n",
                   lock_path.c_str(), strerror(errno));
            _exit(1);
        }

        if (flock(lock_fd_, LOCK_EX | LOCK_NB) < 0)
        {
            printf("[udaemon] another instance is running (lock: %s)\n",
                   lock_path.c_str());
            close(lock_fd_);
            lock_fd_ = -1;
            _exit(1);
        }

        // 写入 pid，方便排查
        char pid_buf[32];
        snprintf(pid_buf, sizeof(pid_buf), "%d\n", getpid());
        int ret = ftruncate(lock_fd_, 0);
        lseek(lock_fd_, 0, SEEK_SET);
        auto nw = write(lock_fd_, pid_buf, strlen(pid_buf));
    }

    // 2) 可选：daemonize
    if (daemonize)
    {
        if (::daemon(0, 0) < 0)
        {
            printf("[udaemon] daemon() failed: %s\n", strerror(errno));
            if (lock_fd_ >= 0)
            {
                flock(lock_fd_, LOCK_UN);
                close(lock_fd_);
                lock_fd_ = -1;
            }
            _exit(1);
        }
        printf("[udaemon] process daemonized\n");
    }
}

ProcessInit::~ProcessInit()
{
    if (lock_fd_ >= 0)
    {
        flock(lock_fd_, LOCK_UN);
        close(lock_fd_);
        lock_fd_ = -1;
    }
}

void InitProcess(bool bDaemonize, const std::string &lock_name)
{
    (void)ProcessInit::GetInstance(bDaemonize, lock_name);
}

} // namespace uco
