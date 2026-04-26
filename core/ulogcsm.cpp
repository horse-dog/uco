#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>

#include "udaemon.h"
#include "ulogshm_def.h"

constexpr size_t MAX_LOG_FILE_SIZE = 5UL * 1024 * 1024 * 1024; // 5GB.
constexpr size_t FLUSH_BYTE_THRESHOLD = 12 * 1024;
constexpr time_t FLUSH_TIME_INTERVAL_SEC = 1; // 1 Second.

static const char *FLOCK_NAME = "uco_logcsm";

static std::string    g_log_dir;
std::atomic<bool> g_running = true;

static std::string get_current_time()
{
    time_t now = time(NULL);
    std::tm tm_snapshot;
    localtime_r(&now, &tm_snapshot);
    std::ostringstream oss;
    oss << std::put_time(&tm_snapshot, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

static int get_hour_timestamp()
{
    time_t now = time(NULL);
    std::tm tm_snapshot;
    localtime_r(&now, &tm_snapshot);
    return tm_snapshot.tm_hour;
}

static std::string get_log_filename()
{
    time_t now = time(NULL);
    std::tm tm_snapshot;
    localtime_r(&now, &tm_snapshot);
    std::ostringstream oss;
    oss << std::put_time(&tm_snapshot, "%Y%m%d%H");
    return g_log_dir + "/" + oss.str() + ".log";
}

static size_t get_file_size(FILE *fp)
{
    if (!fp) return 0;
    struct stat st;
    if (fstat(fileno(fp), &st) != 0) return 0;
    return static_cast<size_t>(st.st_size);
}

static uint64_t reverse_log_list(uint64_t head_idx, char *node_base)
{
    std::unordered_set<uint64_t> visited;

    uint64_t prev_idx = LOG_NODE_IDX_NULL;
    uint64_t curr_idx = head_idx;
    while (curr_idx != LOG_NODE_IDX_NULL)
    {
        if (visited.contains(curr_idx))
        {
          printf("WARN: Log overload. (detected in reverse_log_list)\n");
          break;
        }
        visited.insert(curr_idx);
        LogNode_t *curr = (LogNode_t *)(node_base + curr_idx * LOG_NODE_SIZE);
        uint64_t next_idx = curr->next;
        curr->next = prev_idx;
        prev_idx = curr_idx;
        curr_idx = next_idx;
    }
    return prev_idx;
}

static int get_hour(uint64_t timestamp)
{
    time_t t = (time_t)timestamp;
    std::tm tm_snapshot;
    localtime_r(&t, &tm_snapshot);
    return tm_snapshot.tm_hour;
}

static void consume_and_write(uint64_t& total_written_prev_, int cur_hour, uint64_t head_idx, char *node_base, FILE *fp, FILE *fp_prev, size_t &total_written, size_t &accumulated_written)
{
    if (head_idx == LOG_NODE_IDX_NULL || fp == NULL)
    {
        return;
    }

    uint64_t prev_hour_count = 0;
    int prev_hour = (cur_hour - 1) > 0 ? (cur_hour - 1) : 23;
    uint64_t curr_idx = reverse_log_list(head_idx, node_base);
    std::unordered_set<uint64_t> visited;

    while (curr_idx != LOG_NODE_IDX_NULL)
    {
        if (visited.contains(curr_idx))
        {
          printf("WARN: Log overload. (detected in consume_and_write)\n");
          break;
        }
        visited.insert(curr_idx);
        LogNode_t *curr = (LogNode_t *)(node_base + curr_idx * LOG_NODE_SIZE);
        size_t len = std::min(curr->log_len, (size_t)LOG_NODE_DATA_MAX_SIZE);

        if (get_hour(curr->timestamp) == prev_hour && fp_prev != nullptr)
        {
            if (total_written_prev_ + len < MAX_LOG_FILE_SIZE)
            {
                size_t written = fwrite(curr->log_data, 1, len, fp_prev);
                if (written > 0)
                {
                    total_written_prev_ += written;
                    prev_hour_count += 1;
                }
            }
        }
        else
        {
            size_t written = fwrite(curr->log_data, 1, len, fp);
            if (written > 0)
            {
                total_written += written;
                accumulated_written += written;
            }
        }
        curr_idx = curr->next;
    }

    if (prev_hour_count > 0 && fp_prev != nullptr)
    {
        fflush(fp_prev);
    }
}

static int parse_hour_from_filename(const std::filesystem::path& path)
{
    std::string name = path.stem().string();
    if (name.size() > 4 && name.substr(name.size() - 4) == ".log")
        name = name.substr(0, name.size() - 4);

    if (name.size() != 10) return -1;
    if (!std::all_of(name.begin(), name.end(), ::isdigit)) return -1;

    struct tm tm = {};
    std::istringstream ss(name);
    ss >> std::get_time(&tm, "%Y%m%d%H");
    if (ss.fail()) return -1;

    time_t file_time = mktime(&tm);
    if (file_time == -1) return -1;

    time_t now = time(NULL);
    return static_cast<int>((now - file_time) / 3600);
}

static void do_clean(const std::string& log_dir)
{
    std::error_code ec;
    auto dir_it = std::filesystem::directory_iterator(log_dir, ec);
    if (ec)
    {
        std::cerr << "[" << get_current_time() << "] Failed to open log directory: " << log_dir
                  << " - " << ec.message() << std::endl;
        return;
    }

    for (const auto& entry : dir_it)
    {
        auto path = entry.path();
        if (!entry.is_regular_file()) continue;

        auto ext = path.extension().string();
        if (ext != ".log" && ext != ".gz") continue;

        int age_hours = parse_hour_from_filename(path);
        if (age_hours < 0) continue;  // 无法解析的文件名跳过

        // > 30d 删除
        if (age_hours > 30 * 24)
        {
            std::filesystem::remove(path);
            continue;
        }

        // >20h 压缩 .log
        if (age_hours > 20 && ext == ".log")
        {
            pid_t pid = fork();
            if (pid == 0) { execlp("gzip", "gzip", path.c_str(), nullptr); }
            waitpid(pid, nullptr, 0);
        }
    }
}

class LogConsumer
{
  public:
    LogConsumer()
        : fp_(nullptr), fp_prev_(nullptr),
          current_hour_ts_(-1), total_written_(0), total_written_prev_(0),
          accumulated_written_(0), last_flush_time_(0)
    {
    }

    ~LogConsumer()
    {
        if (fp_) // fflush and fsync.
        {
            fflush(fp_);
            int fd = fileno(fp_);
            if (fd >= 0)
            {
                fsync(fd);
            }
            fclose(fp_);
            fp_ = nullptr;
        }
        if (fp_prev_) // fflush and fsync.
        {
            fflush(fp_prev_);
            int fd = fileno(fp_prev_);
            if (fd >= 0)
            {
                fsync(fd);
            }
            fclose(fp_prev_);
            fp_prev_ = nullptr;
        }
    }

    bool init()
    {
        int ret = ulog_init(true);
        if (ret != 0)
        {
            printf("[%s] Failed to create/open log shm\n", get_current_time().c_str());
            return false;
        }
        return true;
    }

    void run()
    {
        while (g_running)
        {
            check_rotate_file();
            bool has_data = consume_logs();
            if (!has_data) usleep(1000);
        }

        consume_logs();
    }

  private:
    bool consume_logs()
    {
        uint64_t head_idx = log_shm_drain_fg_stack();
        if (head_idx == LOG_NODE_IDX_NULL)
        {
            check_flush();
            return false;
        }

        if (total_written_ >= MAX_LOG_FILE_SIZE)
        {
            check_flush();
            return false;
        }

        char *node_base = log_shm_get_node_array();
        consume_and_write(total_written_prev_, current_hour_ts_, head_idx, node_base, fp_, fp_prev_, total_written_, accumulated_written_);
        check_flush();
        return true;
    }

    void check_flush()
    {
        bool need_flush = false;

        if (accumulated_written_ >= FLUSH_BYTE_THRESHOLD)
        {
            need_flush = true;
        }

        time_t now = time(NULL);
        if (last_flush_time_ > 0 && (now - last_flush_time_) >= FLUSH_TIME_INTERVAL_SEC)
        {
            need_flush = true;
        }

        if (need_flush)
        {
            if (fp_)
            {
                fflush(fp_);
            }
            accumulated_written_ = 0;
            last_flush_time_ = now;
        }
    }

    void check_rotate_file()
    {
        int hour_ts = get_hour_timestamp();

        if (hour_ts != current_hour_ts_)
        {
            if (fp_prev_)
            {
                fclose(fp_prev_);
                fp_prev_ = nullptr;
                printf("[%s] Closed log file for hour: %2d\n", get_current_time().c_str(), current_hour_ts_);
            }

            fp_prev_ = fp_;
            total_written_prev_ = total_written_;

            std::string filename = get_log_filename();
            fp_ = fopen(filename.c_str(), "a");
            if (fp_)
            {
                current_hour_ts_ = hour_ts;
                total_written_ = get_file_size(fp_);
                accumulated_written_ = 0;
                last_flush_time_ = time(NULL);
            }
            else
            {
                printf("[%s] Failed to open log file: %s\n", get_current_time().c_str(), filename.c_str());
            }
        }
    }

    FILE *fp_;
    FILE *fp_prev_;
    int current_hour_ts_;
    size_t total_written_;
    size_t total_written_prev_;
    size_t accumulated_written_;
    time_t last_flush_time_;
};

class LogCleaner
{
  public:
    void run()
    {
        while (g_running)
        {
            do_clean(g_log_dir);
            for (int i = 0; i < 3600 && g_running; ++i)
                sleep(1);
        }

        do_clean(g_log_dir);
    }
};

static void signal_handler(int signo)
{
    g_running.store(false);
}

static void register_signal_handlers()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}

int main(int argc, const char *argv[])
{
    bool daemon_mode = false;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            std::cout << "Usage: " << argv[0] << " -o <output_dir> [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -o <output_dir>  Output directory for log files (required)" << std::endl;
            std::cout << "  -d               Run as daemon" << std::endl;
            std::cout << "  -h, --help       Show this help message" << std::endl;
            return 0;
        }
        else if (strcmp(argv[i], "-o") == 0)
        {
            if (i + 1 < argc)
            {
                g_log_dir = argv[++i];
            }
            else
            {
                std::cerr << "Error: -o requires an argument" << std::endl;
                return 1;
            }
        }
        else if (strcmp(argv[i], "-d") == 0)
        {
            daemon_mode = true;
        }
    }

    if (g_log_dir.empty())
    {
        std::cerr << "Error: -o <output_dir> is required" << std::endl;
        std::cerr << "Usage: " << argv[0] << " -o <output_dir> [-d]" << std::endl;
        return 1;
    }

    uco::InitProcess(daemon_mode, FLOCK_NAME);

    LogConsumer consumer;
    if (!consumer.init())
    {
        std::cerr << "[" << get_current_time()
                  << "] Failed to initialize log consumer" << std::endl;
        return 1;
    }

    std::cout << "[" << get_current_time() << "] Log Consumer Starting... Node Count: "
              << LOG_NODE_COUNT << std::endl;
    std::cout << "[" << get_current_time() << "] Output directory: " << g_log_dir << std::endl;

    std::thread cleaner([] {
        LogCleaner cleaner;
        cleaner.run();
    });

    register_signal_handlers();
    consumer.run();
    cleaner.join();
    return 0;
}
