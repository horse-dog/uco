#include "core/ustring.h"

#include <charconv>
#include <cctype>
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

    std::string Trim(std::string_view s, std::string_view chars)
    {
        size_t b = 0, e = s.size();
        while (b < e && chars.find(s[b]) != std::string_view::npos)
        {
            b++;
        }
        while (e > b && chars.find(s[e - 1]) != std::string_view::npos)
        {
            e--;
        }
        return std::string(s.substr(b, e - b));
    }

    std::string ToLower(std::string_view s)
    {
        std::string out(s);
        for (char &c : out)
        {
            c = (char)std::tolower((unsigned char)c);
        }
        return out;
    }

    std::string ToUpper(std::string_view s)
    {
        std::string out(s);
        for (char &c : out)
        {
            c = (char)std::toupper((unsigned char)c);
        }
        return out;
    }

    bool EqualsIgnoreCase(std::string_view a, std::string_view b)
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

    bool StartsWith(std::string_view s, std::string_view prefix)
    {
        return s.starts_with(prefix);
    }

    bool EndsWith(std::string_view s, std::string_view suffix)
    {
        return s.ends_with(suffix);
    }

    bool Contains(std::string_view s, std::string_view sub)
    {
        return s.find(sub) != std::string_view::npos;
    }

    std::vector<std::string> Split(std::string_view s, char delim,
                                   SplitOpt opt)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true)
        {
            const size_t pos = s.find(delim, start);
            const std::string_view part_view =
                pos == std::string_view::npos ? s.substr(start)
                                              : s.substr(start, pos - start);
            std::string part = (opt & SplitOpt::kTrim) != SplitOpt::kNone
                                   ? Trim(part_view)
                                   : std::string(part_view);
            const bool last = pos == std::string_view::npos;
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
                     std::string_view delim)
    {
        std::string out;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (i > 0)
            {
                out.append(delim);
            }
            out += parts[i];
        }
        return out;
    }

    std::string ReplaceAll(std::string_view s, std::string_view from,
                           std::string_view to)
    {
        if (from.empty())
        {
            return std::string(s);
        }
        std::string out;
        out.reserve(s.size());
        size_t start = 0;
        while (true)
        {
            const size_t pos = s.find(from, start);
            if (pos == std::string_view::npos)
            {
                break;
            }
            out.append(s.substr(start, pos - start));
            out.append(to);
            start = pos + from.size();
        }
        out.append(s.substr(start));
        return out;
    }

    std::string HexEncode(std::string_view data)
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

    bool HexDecode(std::string_view hex, std::string &out)
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

    bool StrToInt32(std::string_view s, int32_t &out)
    {
        int64_t v = 0;
        if (!StrToInt64(s, v) || v < INT32_MIN || v > INT32_MAX)
        {
            return false;
        }
        out = (int32_t)v;
        return true;
    }

    bool StrToUint32(std::string_view s, uint32_t &out)
    {
        uint64_t v = 0;
        if (!StrToUint64(s, v) || v > UINT32_MAX)
        {
            return false;
        }
        out = (uint32_t)v;
        return true;
    }

    bool StrToInt64(std::string_view s, int64_t &out)
    {
        if (s.empty())
        {
            return false;
        }
        if (s.front() == '+')
        {
            s.remove_prefix(1);
            if (s.empty())
            {
                return false;
            }
        }

        int64_t value = 0;
        const char *end = s.data() + s.size();
        const auto parsed = std::from_chars(s.data(), end, value, 10);
        if (parsed.ec != std::errc() || parsed.ptr != end)
        {
            return false;
        }
        out = value;
        return true;
    }

    bool StrToUint64(std::string_view s, uint64_t &out)
    {
        if (s.empty() || s.front() == '-')
        {
            return false;
        }
        if (s.front() == '+')
        {
            s.remove_prefix(1);
            if (s.empty())
            {
                return false;
            }
        }

        uint64_t value = 0;
        const char *end = s.data() + s.size();
        const auto parsed = std::from_chars(s.data(), end, value, 10);
        if (parsed.ec != std::errc() || parsed.ptr != end)
        {
            return false;
        }
        out = value;
        return true;
    }

    std::string UrlEncode(std::string_view value)
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

    std::string UrlDecode(std::string_view value, bool plus_as_space)
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
