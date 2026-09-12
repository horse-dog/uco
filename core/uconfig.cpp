#include "core/uconfig.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <string_view>
#include <utility>

namespace uco
{
namespace
{

std::string_view Trim(std::string_view value) noexcept
{
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())))
    {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())))
    {
        value.remove_suffix(1);
    }
    return value;
}

size_t FindUnquoted(std::string_view value, char target) noexcept
{
    char quote = 0;
    bool escaped = false;
    for (size_t i = 0; i < value.size(); ++i)
    {
        const char ch = value[i];
        if (escaped)
        {
            escaped = false;
            continue;
        }
        if (quote == '"' && ch == '\\')
        {
            escaped = true;
            continue;
        }
        if (ch == '\'' || ch == '"')
        {
            if (quote == 0)
                quote = ch;
            else if (quote == ch)
                quote = 0;
            continue;
        }
        if (quote == 0 && ch == target)
        {
            return i;
        }
    }
    return std::string_view::npos;
}

bool StripComment(std::string_view line, std::string &result)
{
    const size_t pos = FindUnquoted(line, '#');
    if (pos != std::string_view::npos)
    {
        line = line.substr(0, pos);
    }
    result.assign(Trim(line));
    return true;
}

bool Unquote(std::string_view value, std::string &result)
{
    value = Trim(value);
    if (value.size() < 2 ||
        ((value.front() != '"' || value.back() != '"') &&
         (value.front() != '\'' || value.back() != '\'')))
    {
        result.assign(value);
        return true;
    }

    const char quote = value.front();
    value.remove_prefix(1);
    value.remove_suffix(1);
    if (quote == '\'')
    {
        result.assign(value);
        return true;
    }

    result.clear();
    result.reserve(value.size());
    bool escaped = false;
    for (char ch : value)
    {
        if (!escaped && ch == '\\')
        {
            escaped = true;
            continue;
        }
        if (escaped)
        {
            switch (ch)
            {
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            default: result.push_back(ch); break;
            }
            escaped = false;
        }
        else
        {
            result.push_back(ch);
        }
    }
    return !escaped;
}

template <class T>
bool ParseInteger(const std::string &value, T &result) noexcept
{
    const char *begin = value.data();
    const char *end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result);
    return parsed.ec == std::errc() && parsed.ptr == end;
}

} // namespace

bool YamlConfig::Load(const std::string &path) noexcept
{
    YamlConfig loaded;
    try
    {
        std::ifstream input(path);
        if (!input)
        {
            m_values.clear();
            return false;
        }

        std::vector<std::pair<size_t, std::string>> sections;
        std::string line;
        while (std::getline(input, line))
        {
            if (line.find('\t') != std::string::npos)
            {
                m_values.clear();
                return false;
            }

            size_t indent = 0;
            while (indent < line.size() && line[indent] == ' ')
            {
                ++indent;
            }

            std::string content;
            StripComment(std::string_view(line).substr(indent), content);
            if (content.empty())
            {
                continue;
            }

            const size_t colon = FindUnquoted(content, ':');
            if (colon == std::string_view::npos)
            {
                m_values.clear();
                return false;
            }

            const std::string key(
                Trim(std::string_view(content).substr(0, colon)));
            if (key.empty())
            {
                m_values.clear();
                return false;
            }

            while (!sections.empty() && sections.back().first >= indent)
            {
                sections.pop_back();
            }

            std::string full_key;
            for (const auto &[_, section] : sections)
            {
                if (!full_key.empty())
                {
                    full_key.push_back('.');
                }
                full_key += section;
            }
            if (!full_key.empty())
            {
                full_key.push_back('.');
            }
            full_key += key;

            std::string raw_value;
            StripComment(std::string_view(content).substr(colon + 1),
                         raw_value);
            if (raw_value.empty())
            {
                sections.emplace_back(indent, key);
                continue;
            }

            std::string value;
            if (!Unquote(raw_value, value) ||
                !loaded.m_values.emplace(full_key, std::move(value)).second)
            {
                m_values.clear();
                return false;
            }
        }
        *this = std::move(loaded);
        return true;
    }
    catch (...)
    {
        m_values.clear();
        return false;
    }
}

bool YamlConfig::Contains(const std::string &key) const noexcept
{
    return m_values.contains(key);
}

bool YamlConfig::TryGetString(const std::string &key,
                              std::string &value) const noexcept
{
    try
    {
        const auto it = m_values.find(key);
        if (it == m_values.end())
        {
            return false;
        }
        value = it->second;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool YamlConfig::TryParseInt(const std::string &text,
                             std::int64_t &value) noexcept
{
    return ParseInteger(text, value);
}

bool YamlConfig::TryParseUInt(const std::string &text,
                              std::uint64_t &value) noexcept
{
    return ParseInteger(text, value);
}

bool YamlConfig::TryParseBool(std::string text, bool &value) noexcept
{
    try
    {
        std::transform(text.begin(), text.end(), text.begin(), [](char ch) {
            return static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch)));
        });
        if (text == "true" || text == "yes" || text == "on" || text == "1")
        {
            value = true;
            return true;
        }
        if (text == "false" || text == "no" || text == "off" || text == "0")
        {
            value = false;
            return true;
        }
        return false;
    }
    catch (...)
    {
        return false;
    }
}

bool YamlConfig::TryGetInt(const std::string &key,
                           std::int64_t &value) const noexcept
{
    std::string text;
    return TryGetString(key, text) && TryParseInt(text, value);
}

bool YamlConfig::TryGetUInt(const std::string &key,
                            std::uint64_t &value) const noexcept
{
    std::string text;
    return TryGetString(key, text) && TryParseUInt(text, value);
}

bool YamlConfig::TryGetBool(const std::string &key, bool &value) const noexcept
{
    std::string text;
    return TryGetString(key, text) && TryParseBool(std::move(text), value);
}

bool YamlConfig::TryGetRawList(const std::string &key,
                               std::vector<std::string> &result) const noexcept
{
    try
    {
        std::string text;
        if (!TryGetString(key, text))
        {
            return false;
        }

        std::string_view value = Trim(text);
        if (value.size() < 2 || value.front() != '[' || value.back() != ']')
        {
            return false;
        }

        value.remove_prefix(1);
        value.remove_suffix(1);
        result.clear();
        while (!Trim(value).empty())
        {
            const size_t comma = FindUnquoted(value, ',');
            const std::string_view item = comma == std::string_view::npos
                                              ? value
                                              : value.substr(0, comma);
            if (Trim(item).empty())
            {
                return false;
            }
            std::string parsed;
            if (!Unquote(item, parsed))
            {
                return false;
            }
            result.push_back(std::move(parsed));
            if (comma == std::string_view::npos)
            {
                break;
            }
            value.remove_prefix(comma + 1);
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace uco
