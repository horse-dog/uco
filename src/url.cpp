#include "url.h"
#include "string_utils.h"
#include <iomanip>
#include <sstream>

// URL encode (UTF8)
std::string url_encode(const std::string &value)
{
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (unsigned char c : value)
    {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            escaped << c;
        }
        else
        {
            escaped << '%' << std::uppercase << std::setw(2) << int(c)
                    << std::nouppercase;
        }
    }
    return escaped.str();
}

// URL decode (UTF8)
// plus_as_space: true 按 query/表单语义（+ → 空格），
//                false 按路径语义（+ 为字面字符）.
// 非法 % 序列按字面保留，不抛异常.
std::string url_decode(const std::string &value, bool plus_as_space)
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
            const int hi = uco::CharToHex(value[i + 1]);
            const int lo = uco::CharToHex(value[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                result += static_cast<char>((hi << 4) | lo);
                i += 2;
            }
            else
            {
                result += '%';
            }
        }
        else
        {
            result += value[i];
        }
    }
    return result;
}
