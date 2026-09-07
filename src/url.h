#pragma once
#include <string>

// URL encode (UTF8)
std::string url_encode(const std::string &value);

// URL decode (UTF8)
// plus_as_space: true 按 query/表单语义（+ → 空格），
//                false 按路径语义（+ 为字面字符）.
// 非法 % 序列按字面保留，不抛异常.
std::string url_decode(const std::string &value, bool plus_as_space = false);
