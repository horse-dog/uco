#pragma once
#include <string>

// URL encode (UTF8)
std::string url_encode(const std::string &value);

// URL decode (UTF8)
std::string url_decode(const std::string &value);
