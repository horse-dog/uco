#pragma once

#include <cstdint>
#include <cstdio>
#include <google/protobuf/message.h>
#include <sstream>
#include <string_view>
#include <vector>
#include <deque>
#include <list>
#include <set>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <array>
#include <tuple>
#include <optional>
#include <type_traits>


enum class LogLevel
{
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL
};

enum class LogMode
{
    CONSOLE,
    FILE,
};

namespace uco
{

void OpenLog(const std::string& module_name, LogLevel level, LogMode mode, bool enable_syslog);

#define NR(X) X

#define SYSDBG(...) uco::__inner__::Logger((int)0, LogLevel::DEBUG, uco::__inner__::basefilename(__FILE__), __func__, __LINE__, #__VA_ARGS__, ##__VA_ARGS__)
#define SYSMSG(...) uco::__inner__::Logger((int)0, LogLevel::INFO,  uco::__inner__::basefilename(__FILE__), __func__, __LINE__, #__VA_ARGS__, ##__VA_ARGS__)
#define SYSWRN(...) uco::__inner__::Logger((int)0, LogLevel::WARN,  uco::__inner__::basefilename(__FILE__), __func__, __LINE__, #__VA_ARGS__, ##__VA_ARGS__)
#define SYSERR(...) uco::__inner__::Logger((int)0, LogLevel::ERROR, uco::__inner__::basefilename(__FILE__), __func__, __LINE__, #__VA_ARGS__, ##__VA_ARGS__)
#define SYSFTL(...) uco::__inner__::Logger((int)0, LogLevel::FATAL, uco::__inner__::basefilename(__FILE__), __func__, __LINE__, #__VA_ARGS__, ##__VA_ARGS__)

#define LOGDBG(...) uco::__inner__::Logger((int)1, LogLevel::DEBUG, uco::__inner__::basefilename(__FILE__), __func__, __LINE__, #__VA_ARGS__, ##__VA_ARGS__)
#define LOGMSG(...) uco::__inner__::Logger((int)1, LogLevel::INFO,  uco::__inner__::basefilename(__FILE__), __func__, __LINE__, #__VA_ARGS__, ##__VA_ARGS__)
#define LOGWRN(...) uco::__inner__::Logger((int)1, LogLevel::WARN,  uco::__inner__::basefilename(__FILE__), __func__, __LINE__, #__VA_ARGS__, ##__VA_ARGS__)
#define LOGERR(...) uco::__inner__::Logger((int)1, LogLevel::ERROR, uco::__inner__::basefilename(__FILE__), __func__, __LINE__, #__VA_ARGS__, ##__VA_ARGS__)
#define LOGFTL(...) uco::__inner__::Logger((int)1, LogLevel::FATAL, uco::__inner__::basefilename(__FILE__), __func__, __LINE__, #__VA_ARGS__, ##__VA_ARGS__)

#define SYSDBGF(fmt, ...) uco::__inner__::LoggerF((int)0, LogLevel::DEBUG, uco::__inner__::basefilename(__FILE__), __func__, __LINE__, fmt, ##__VA_ARGS__)
#define SYSMSGF(fmt, ...) uco::__inner__::LoggerF((int)0, LogLevel::INFO,  uco::__inner__::basefilename(__FILE__), __func__, __LINE__, fmt, ##__VA_ARGS__)
#define SYSWRNF(fmt, ...) uco::__inner__::LoggerF((int)0, LogLevel::WARN,  uco::__inner__::basefilename(__FILE__), __func__, __LINE__, fmt, ##__VA_ARGS__)
#define SYSERRF(fmt, ...) uco::__inner__::LoggerF((int)0, LogLevel::ERROR, uco::__inner__::basefilename(__FILE__), __func__, __LINE__, fmt, ##__VA_ARGS__)
#define SYSFTLF(fmt, ...) uco::__inner__::LoggerF((int)0, LogLevel::FATAL, uco::__inner__::basefilename(__FILE__), __func__, __LINE__, fmt, ##__VA_ARGS__)

#define LOGDBGF(fmt, ...) uco::__inner__::LoggerF((int)1, LogLevel::DEBUG, uco::__inner__::basefilename(__FILE__), __func__, __LINE__, fmt, ##__VA_ARGS__)
#define LOGMSGF(fmt, ...) uco::__inner__::LoggerF((int)1, LogLevel::INFO,  uco::__inner__::basefilename(__FILE__), __func__, __LINE__, fmt, ##__VA_ARGS__)
#define LOGWRNF(fmt, ...) uco::__inner__::LoggerF((int)1, LogLevel::WARN,  uco::__inner__::basefilename(__FILE__), __func__, __LINE__, fmt, ##__VA_ARGS__)
#define LOGERRF(fmt, ...) uco::__inner__::LoggerF((int)1, LogLevel::ERROR, uco::__inner__::basefilename(__FILE__), __func__, __LINE__, fmt, ##__VA_ARGS__)
#define LOGFTLF(fmt, ...) uco::__inner__::LoggerF((int)1, LogLevel::FATAL, uco::__inner__::basefilename(__FILE__), __func__, __LINE__, fmt, ##__VA_ARGS__)

// ==================== STL container operator<< overloads ====================
namespace __inner__
{

// --- helper: print iterable containers (vector, deque, list, set, etc.) ---
template <class _Container>
auto stream_container(std::ostream& os, const _Container& c)
    -> decltype(std::begin(c), std::end(c), void())
{
    os << '[';
    auto it = std::begin(c);
    if (it != std::end(c)) {
        os << *it;
        for (++it; it != std::end(c); ++it)
            os << ", " << *it;
    }
    os << ']';
}

// --- helper: print map-like containers (key-value pairs) ---
template <class _Map>
auto stream_map(std::ostream& os, const _Map& m)
    -> decltype(std::begin(m)->first, std::begin(m)->second, void())
{
    os << '{';
    auto it = std::begin(m);
    if (it != std::end(m)) {
        os << it->first << ": " << it->second;
        for (++it; it != std::end(m); ++it)
            os << ", " << it->first << ": " << it->second;
    }
    os << '}';
}

// --- sequence containers: vector, deque, list ---
template <class _Tp, class _Alloc>
std::ostream& operator<<(std::ostream& os, const std::vector<_Tp, _Alloc>& c) { stream_container(os, c); return os; }

template <class _Tp, class _Alloc>
std::ostream& operator<<(std::ostream& os, const std::deque<_Tp, _Alloc>& c) { stream_container(os, c); return os; }

template <class _Tp, class _Alloc>
std::ostream& operator<<(std::ostream& os, const std::list<_Tp, _Alloc>& c) { stream_container(os, c); return os; }

// --- associative containers: set, multiset ---
template <class _Key, class _Compare, class _Alloc>
std::ostream& operator<<(std::ostream& os, const std::set<_Key, _Compare, _Alloc>& c) { stream_container(os, c); return os; }

template <class _Key, class _Compare, class _Alloc>
std::ostream& operator<<(std::ostream& os, const std::multiset<_Key, _Compare, _Alloc>& c) { stream_container(os, c); return os; }

template <class _Key, class _Hash, class _KeyEq, class _Alloc>
std::ostream& operator<<(std::ostream& os, const std::unordered_set<_Key, _Hash, _KeyEq, _Alloc>& c) { stream_container(os, c); return os; }

template <class _Key, class _Hash, class _KeyEq, class _Alloc>
std::ostream& operator<<(std::ostream& os, const std::unordered_multiset<_Key, _Hash, _KeyEq, _Alloc>& c) { stream_container(os, c); return os; }

// --- map containers: map, multimap, unordered_map, unordered_multimap ---
template <class _Key, class _Val, class _Compare, class _Alloc>
std::ostream& operator<<(std::ostream& os, const std::map<_Key, _Val, _Compare, _Alloc>& m) { stream_map(os, m); return os; }

template <class _Key, class _Val, class _Compare, class _Alloc>
std::ostream& operator<<(std::ostream& os, const std::multimap<_Key, _Val, _Compare, _Alloc>& m) { stream_map(os, m); return os; }

template <class _Key, class _Val, class _Hash, class _KeyEq, class _Alloc>
std::ostream& operator<<(std::ostream& os, const std::unordered_map<_Key, _Val, _Hash, _KeyEq, _Alloc>& m) { stream_map(os, m); return os; }

template <class _Key, class _Val, class _Hash, class _KeyEq, class _Alloc>
std::ostream& operator<<(std::ostream& os, const std::unordered_multimap<_Key, _Val, _Hash, _KeyEq, _Alloc>& m) { stream_map(os, m); return os; }

// --- std::array ---
template <class _Tp, size_t _N>
std::ostream& operator<<(std::ostream& os, const std::array<_Tp, _N>& a) { stream_container(os, a); return os; }

// --- std::pair ---
template <class _T1, class _T2>
std::ostream& operator<<(std::ostream& os, const std::pair<_T1, _T2>& p)
{
    os << '(' << p.first << ", " << p.second << ')';
    return os;
}

std::ostream &operator<<(std::ostream &os, const google::protobuf::Message &msg);

// --- std::tuple ---
template <class _Tuple, size_t... _Is>
void print_tuple_impl(std::ostream& os, const _Tuple& t, std::index_sequence<_Is...>)
{
    os << '(';
    ((os << (_Is == 0 ? "" : ", ") << std::get<_Is>(t)), ...);
    os << ')';
}

template <class... _Args>
std::ostream& operator<<(std::ostream& os, const std::tuple<_Args...>& t)
{
    print_tuple_impl(os, t, std::index_sequence_for<_Args...>{});
    return os;
}

// --- std::optional (C++17) ---
template <class _Tp>
std::ostream& operator<<(std::ostream& os, const std::optional<_Tp>& opt)
{
    if (opt.has_value()) os << *opt;
    else os << "nullopt";
    return os;
}

bool is_dot(const std::string_view& sv);
bool is_with_name(const std::string_view& sv);
bool is_need_log(int role, LogLevel level);
void process_log(LogLevel level, uint64_t timestamp, std::string& log);
void gen_log_header(uint64_t& timestamp, std::ostringstream& sLog, std::vector<std::string_view>& vecNames, 
  int role, int level, const std::string_view& filename, const char* func, int line, const char* varnames);
void gen_log_header(uint64_t& timestamp, std::ostringstream& sLog, int role, int level, 
                    const std::string_view& filename, const char* func, int line);
void gen_log_tail(std::ostringstream& sLog);

#ifndef PROJECT_SOURCE_DIR
#define PROJECT_SOURCE_DIR ""
#endif

constexpr std::string_view project_source_dir = PROJECT_SOURCE_DIR;

constexpr std::string_view basefilename(std::string_view file)
{
    return project_source_dir.empty() ? file : file.substr(project_source_dir.size() + 1);
}

// end of recursion.
template <class... _Args>
void gen_log_body(std::ostream &os, const std::vector<std::string_view>&, size_t)
{
}

template <class _Tp, class... _Args>
void gen_log_body(std::ostream &os, const std::vector<std::string_view>& names,
                     size_t index, _Tp &&first, _Args &&...rest)
{
    if (index < names.size())
    {
        if (index != 0 && !__inner__::is_dot(names[index])) os << ' ';
        if (__inner__::is_with_name(names[index]))
        {
            std::string_view __name(names[index].begin() + 3, names[index].size() - 4);
            if constexpr (std::is_same_v<std::decay_t<_Tp>, bool>)
                os << __name << " " << (first ? "true" : "false");
            else
                os << __name << " " << std::forward<_Tp>(first);
        }
        else
        {
            if constexpr (std::is_same_v<std::decay_t<_Tp>, bool>)
              os << (first ? "true" : "false");
            else
              os << std::forward<_Tp>(first);
        }
    }
    gen_log_body(os, names, index + 1, std::forward<_Args>(rest)...);
}

template <class... _Args>
std::string gen_log(uint64_t& timestamp, int role, int level, const std::string_view& filename, 
                    const char* func, int line, const char* varnames, _Args&&... args)
{
    std::ostringstream sLog;
    std::vector<std::string_view> vecNames;
    gen_log_header(timestamp, sLog, vecNames, role, level, filename, func, line, varnames);
    gen_log_body(sLog, vecNames, 0, std::forward<_Args>(args)...);
    gen_log_tail(sLog);
    return sLog.str();
}

template <class... _Args>
void Logger(int role, LogLevel level, const std::string_view& filename, const char* func, int line, 
            const char* varnames, _Args&&... args)
{
    bool bLog = is_need_log(role, level);
    if (bLog)
    {
        uint64_t timestamp = 0;
        std::string log = gen_log(timestamp, role, (int)level, filename, func, line, varnames, std::forward<_Args>(args)...);
        process_log(level, timestamp, log);
    }
}

template <class... _Args>
void LoggerF(int role, LogLevel level, const std::string_view& filename, const char* func, int line,
             const char* fmt, _Args&&... args)
{
    bool bLog = is_need_log(role, level);
    if (bLog)
    {
        uint64_t timestamp = 0;
        std::ostringstream sLog;
        gen_log_header(timestamp, sLog, role, (int)level, filename, func, line);
        int len = snprintf(nullptr, 0, fmt, std::forward<_Args>(args)...);
        if (len > 0)
        {
            std::vector<char> buf(static_cast<size_t>(len) + 1);
            snprintf(buf.data(), buf.size(), fmt, std::forward<_Args>(args)...);
            sLog << buf.data();
        }
        gen_log_tail(sLog);
        std::string log = sLog.str();
        process_log(level, timestamp, log);
    }
}

};

} // namespace uco
