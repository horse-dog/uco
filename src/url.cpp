#include "url.h"
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
std::string url_decode(const std::string &value)
{
    std::string result;
    size_t i = 0;
    while (i < value.size())
    {
        if (value[i] == '%' && i + 2 < value.size())
        {
            std::string hex = value.substr(i + 1, 2);
            char decoded = static_cast<char>(std::stoi(hex, nullptr, 16));
            result += decoded;
            i += 3;
        }
        else if (value[i] == '+')
        {
            result += ' ';
            i++;
        }
        else
        {
            result += value[i++];
        }
    }
    return result;
}
