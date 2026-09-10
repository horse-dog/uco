#include "core/ustring.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <utility>

namespace uco
{
    int CharToHex(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    int HexToChar(int v)
    {
        if (v >= 0 && v <= 9) return '0' + v;
        if (v >= 10 && v <= 15) return 'a' + v - 10;
        return -1;
    }

    std::string Trim(const std::string &s, const std::string &chars)
    {
        size_t b = 0, e = s.size();
        while (b < e && chars.find(s[b]) != std::string::npos)
        {
            b++;
        }
        while (e > b && chars.find(s[e - 1]) != std::string::npos)
        {
            e--;
        }
        return s.substr(b, e - b);
    }

    std::string ToLower(const std::string &s)
    {
        std::string out(s);
        for (char &c : out)
        {
            c = (char)std::tolower((unsigned char)c);
        }
        return out;
    }

    std::string ToUpper(const std::string &s)
    {
        std::string out(s);
        for (char &c : out)
        {
            c = (char)std::toupper((unsigned char)c);
        }
        return out;
    }

    bool EqualsIgnoreCase(const std::string &a, const std::string &b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (std::tolower((unsigned char)a[i]) !=
                std::tolower((unsigned char)b[i]))
            {
                return false;
            }
        }
        return true;
    }

    bool StartsWith(const std::string &s, const std::string &prefix)
    {
        return s.size() >= prefix.size() &&
               s.compare(0, prefix.size(), prefix) == 0;
    }

    bool EndsWith(const std::string &s, const std::string &suffix)
    {
        return s.size() >= suffix.size() &&
               s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    bool Contains(const std::string &s, const std::string &sub)
    {
        return s.find(sub) != std::string::npos;
    }

    std::vector<std::string> Split(const std::string &s, char delim,
                                   SplitOpt opt)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true)
        {
            const size_t pos = s.find(delim, start);
            std::string part = (pos == std::string::npos)
                                   ? s.substr(start)
                                   : s.substr(start, pos - start);
            if ((opt & SplitOpt::kTrim) != SplitOpt::kNone)
            {
                part = Trim(part);
            }
            const bool last = (pos == std::string::npos);
            if ((opt & SplitOpt::kSkipEmpty) != SplitOpt::kNone && part.empty())
            {
                if (last)
                {
                    break;
                }
                start = pos + 1;
                continue;
            }
            parts.push_back(std::move(part));
            if (last)
            {
                break;
            }
            start = pos + 1;
        }
        return parts;
    }

    std::string Join(const std::vector<std::string> &parts,
                     const std::string &delim)
    {
        std::string out;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (i > 0)
            {
                out += delim;
            }
            out += parts[i];
        }
        return out;
    }

    std::string ReplaceAll(const std::string &s, const std::string &from,
                           const std::string &to)
    {
        if (from.empty())
        {
            return s;
        }
        std::string out;
        size_t start = 0;
        while (true)
        {
            const size_t pos = s.find(from, start);
            if (pos == std::string::npos)
            {
                break;
            }
            out.append(s, start, pos - start);
            out += to;
            start = pos + from.size();
        }
        out.append(s, start, std::string::npos);
        return out;
    }

    std::string HexEncode(const std::string &data)
    {
        static const char *kHex = "0123456789abcdef";
        std::string out;
        out.reserve(data.size() * 2);
        for (unsigned char c : data)
        {
            out += kHex[c >> 4];
            out += kHex[c & 0xf];
        }
        return out;
    }

    bool HexDecode(const std::string &hex, std::string &out)
    {
        if (hex.size() % 2 != 0)
        {
            return false;
        }
        std::string buf;
        buf.reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2)
        {
            const int hi = CharToHex(hex[i]);
            const int lo = CharToHex(hex[i + 1]);
            if (hi < 0 || lo < 0)
            {
                return false;
            }
            buf += (char)((hi << 4) | lo);
        }
        out = std::move(buf);
        return true;
    }

    bool StrToInt32(const std::string &s, int32_t &out)
    {
        int64_t v = 0;
        if (!StrToInt64(s, v) || v < INT32_MIN || v > INT32_MAX)
        {
            return false;
        }
        out = (int32_t)v;
        return true;
    }

    bool StrToUint32(const std::string &s, uint32_t &out)
    {
        uint64_t v = 0;
        if (!StrToUint64(s, v) || v > UINT32_MAX)
        {
            return false;
        }
        out = (uint32_t)v;
        return true;
    }

    bool StrToInt64(const std::string &s, int64_t &out)
    {
        if (s.empty())
        {
            return false;
        }
        errno = 0;
        char *end = nullptr;
        const long long v = std::strtoll(s.c_str(), &end, 10);
        if (errno != 0 || end == nullptr || *end != '\0')
        {
            return false;
        }
        out = (int64_t)v;
        return true;
    }

    bool StrToUint64(const std::string &s, uint64_t &out)
    {
        // strtoull 会把 "-1" 折成大数, 显式拒绝负号.
        if (s.empty() || s[0] == '-')
        {
            return false;
        }
        errno = 0;
        char *end = nullptr;
        const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
        if (errno != 0 || end == nullptr || *end != '\0')
        {
            return false;
        }
        out = (uint64_t)v;
        return true;
    }

    std::string UrlEncode(const std::string &value)
    {
        static const char *kHex = "0123456789ABCDEF";
        std::string out;
        out.reserve(value.size() * 3);
        for (unsigned char c : value)
        {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            {
                out += (char)c; // RFC 3986 非保留字符.
            }
            else
            {
                out += '%';
                out += kHex[c >> 4];
                out += kHex[c & 0xf];
            }
        }
        return out;
    }

    std::string UrlDecode(const std::string &value, bool plus_as_space)
    {
        std::string result;
        result.reserve(value.size());
        for (size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] == '+' && plus_as_space)
            {
                result += ' ';
            }
            else if (value[i] == '%' && i + 2 < value.size())
            {
                const int hi = CharToHex(value[i + 1]);
                const int lo = CharToHex(value[i + 2]);
                if (hi >= 0 && lo >= 0)
                {
                    result += (char)((hi << 4) | lo);
                    i += 2;
                }
                else
                {
                    result += '%'; // 非法序列按字面保留.
                }
            }
            else
            {
                result += value[i];
            }
        }
        return result;
    }
}
