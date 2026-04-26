#include <cstdio>
#include <cstdlib>
#include <emmintrin.h>
#include <array>
#include <string>
#include <sstream>
#include <string_view>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>
#include <iomanip>
#include <ctime>
#include <cstring>

#include "ulog.h"
#include "ulogshm_def.h"

namespace uco
{

std::string sMouduleName;
LogLevel CurrentLogLevel = LogLevel::INFO;
LogMode  CurrentLogMode = LogMode::CONSOLE;
bool bLogEnable = false;
bool bSysLogEnable = false;
uint64_t pid = 0;

static uint64_t gettid_cache()
{
    thread_local uint64_t tid = 0;
    if (tid == 0) [[unlikely]] 
    {
        tid = gettid();
    }
    return tid;
}

static std::string_view trim(const std::string_view& s)
{
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string_view::npos) return std::string_view();
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

static std::string_view trim(const char* str)
{
    std::string_view s(str);
    return trim(s);
}

static std::string trim(const std::string& str)
{
    std::string_view s(str);
    return std::string(trim(s));
}

static std::vector<std::string_view> split_args(const std::string_view& s)
{
    std::vector<std::string_view> result;
    std::string_view::size_type last = 0;
    int parentheses = 0;
    bool tag1 = false;
    bool tag2 = false;
    for (size_t i = 0; i < s.size(); i++)
    {
        switch (s[i])
        {
        case '(':
            if (!tag1 && !tag2) ++parentheses;
            break;
        case ')':
            if (!tag1 && !tag2) --parentheses;
            break;
        case '"':
            if (!tag2 && (i == 0 || s[i - 1] != '\\')) tag1 = !tag1;
            break;
        case '\'':
            if (!tag1 && (i == 0 || s[i - 1] != '\\')) tag2 = !tag2;
            break;
        case ',':
            if (parentheses == 0 && !tag1 && !tag2)
            {
                result.push_back(trim(s.substr(last, i - last)));
                last = i + 1;
            }
            break;
        default:
            break;
        }
    }
    if (last < s.size())
    {
        result.push_back(trim(s.substr(last)));
    }
    return result;
}

static std::string get_current_time(uint64_t* pTimeStamp = nullptr)
{
    time_t now = time(NULL);
    std::tm tm_snapshot;
    localtime_r(&now, &tm_snapshot);
    std::ostringstream oss;
    oss << std::put_time(&tm_snapshot, "%Y-%m-%d %H:%M:%S");
    if (pTimeStamp != nullptr) *pTimeStamp = (uint64_t)now;
    return oss.str();
}

namespace __inner__
{

bool is_dot(const std::string_view& sv)
{
    const static std::unordered_set<char> cset = {',', '.', ':', '!'};
    if (sv.size() != 3) return false;
    if (sv[0] != '\'' && sv[0] != '"') return false;
    if (sv.back() != '\'' && sv.back() != '"') return false;
    if (cset.find(sv[1]) == cset.end()) return false;
    return true;
}

bool is_with_name(const std::string_view& sv)
{
    return (sv.size() >=4 && sv[0] == 'N' && sv[1] == 'R' && sv[2] == '(' && sv.back() == ')');
}

bool is_need_log(int role, LogLevel level)
{
    if (!bLogEnable) return false;
    bool bLog = level >= CurrentLogLevel;
    if (role == 0 && !bSysLogEnable) bLog = false;
    if (role == 0 && level >= LogLevel::ERROR) bLog = true;
    return bLog;
}

void process_log(LogLevel level, uint64_t timestamp, std::string& log)
{
    switch (CurrentLogMode)
    {
    case LogMode::CONSOLE:
        printf("%s\n", log.c_str());
        break;
    case LogMode::FILE:
        ulog_push(timestamp, log);
        break;
    default:
        break;
    }
    if (level == LogLevel::FATAL) std::terminate();
}

void gen_log_header(uint64_t& timestamp, std::ostringstream& sLog, std::vector<std::string_view>& vecNames, 
    int role, int level, const std::string_view& filename, const char* func, int line, const char* varnames)
{
    const static char* DefaultFont = "\033[0m";
    const static char* BoldFontSize = "\033[1;";
    const static char* NormalFontSize = "\033[0;";
    const static std::array<std::pair<const char*, const char*>, 5> LevelList = 
    {
        std::pair<const char*, const char*>{"DBG", "32m"},
        std::pair<const char*, const char*>{"MSG", "39m"},
        std::pair<const char*, const char*>{"WRN", "33m"},
        std::pair<const char*, const char*>{"ERR", "31m"},
        std::pair<const char*, const char*>{"FTL", "95m"},
    };
    level = std::max(0, std::min(level, 5));
    vecNames = split_args(varnames);
    const char* sRole = role == 0 ? "[\033[1;38;5;98mSYS\033[0m]" : "[\033[1;34mUSR\033[0m]";
    sLog << sRole << '[' << BoldFontSize << LevelList[level].second << LevelList[level].first << DefaultFont
         << "] " << sMouduleName << '<' << pid << ',' << gettid_cache() << "> " 
         << get_current_time(&timestamp) << " [" << filename << ':' << line << "](" << func << "): " 
         << NormalFontSize << LevelList[level].second;
}

void gen_log_header(uint64_t& timestamp, std::ostringstream& sLog, int role, int level, 
                    const std::string_view& filename, const char* func, int line)
{
    const static char* DefaultFont = "\033[0m";
    const static char* BoldFontSize = "\033[1;";
    const static char* NormalFontSize = "\033[0;";
    const static std::array<std::pair<const char*, const char*>, 5> LevelList = 
    {
        std::pair<const char*, const char*>{"DBG", "32m"},
        std::pair<const char*, const char*>{"MSG", "39m"},
        std::pair<const char*, const char*>{"WRN", "33m"},
        std::pair<const char*, const char*>{"ERR", "31m"},
        std::pair<const char*, const char*>{"FTL", "95m"},
    };
    level = std::max(0, std::min(level, 5));
    const char* sRole = role == 0 ? "[\033[1;38;5;98mSYS\033[0m]" : "[\033[1;34mUSR\033[0m]";
    sLog << sRole << '[' << BoldFontSize << LevelList[level].second << LevelList[level].first << DefaultFont
         << "] " << sMouduleName << '<' << pid << ',' << gettid_cache() << "> " 
         << get_current_time(&timestamp) << " [" << filename << ':' << line << "](" << func << "): " 
         << NormalFontSize << LevelList[level].second;
}

void gen_log_tail(std::ostringstream& sLog)
{
    const static char* DefaultFont = "\033[0m";
    sLog << DefaultFont;
}


static std::string octalToChinese(const std::string &input)
{
    std::string result;
    size_t i = 0;
    while (i < input.length())
    {
        if (input[i] == '\\' && i + 3 < input.length() && input[i + 1] >= '0' &&
            input[i + 1] <= '7' && input[i + 2] >= '0' && input[i + 2] <= '7' &&
            input[i + 3] >= '0' && input[i + 3] <= '7')
        {

            // Extract the octal sequence
            std::string octal = input.substr(i + 1, 3);

            // Convert octal string to integer
            int octalValue = std::stoi(octal, nullptr, 8);

            // Convert integer value to corresponding character
            result += static_cast<char>(octalValue);

            // Move to the next character after the octal sequence
            i += 4;
        }
        else
        {
            // Append regular character to result
            result += input[i];
            i++;
        }
    }
    return result;
}

std::ostream &operator<<(std::ostream &os, const google::protobuf::Message &msg)
{
    return os << octalToChinese(msg.ShortDebugString()).c_str();
}

}

void OpenLog(const std::string& module_name, LogLevel level, LogMode mode, bool enable_syslog)
{
    pid = getpid();
    sMouduleName = module_name;
    bLogEnable = true;
    CurrentLogLevel = level;
    CurrentLogMode = mode;
    bSysLogEnable = enable_syslog;
    if (mode == LogMode::FILE)
    {
        if (ulog_init(false) != 0)
        {
            _exit(1);
        }
    }
}

}
