#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uco
{
    /**
     * @brief 单字符转 16 进制值.
     * @param c 待转字符.
     * @return 0-15; 非十六进制字符返回 -1.
     */
    int CharToHex(char c);

    /**
     * @brief 16 进制值转对应字符 (小写).
     * @param v 待转值.
     * @return '0'-'9' 或 'a'-'f'; 超出 0-15 返回 -1.
     */
    int HexToChar(int v);

    /**
     * @brief 去除两端位于字符集内的字符.
     * @param s     输入串.
     * @param chars 待去字符集 (默认空白: 空格/制表/回车/换行).
     * @return 修剪后的新串.
     */
    std::string Trim(std::string_view s, std::string_view chars = " \t\r\n");

    /**
     * @brief ASCII 转小写.
     * @param s 输入串.
     * @return 转换后的新串.
     */
    std::string ToLower(std::string_view s);

    /**
     * @brief ASCII 转大写.
     * @param s 输入串.
     * @return 转换后的新串.
     */
    std::string ToUpper(std::string_view s);

    /**
     * @brief 忽略大小写比较相等.
     * @param a 输入串.
     * @param b 输入串.
     * @return 忽略大小写相等返回 true.
     */
    bool EqualsIgnoreCase(std::string_view a, std::string_view b);

    /**
     * @brief 是否以 prefix 开头.
     * @param s      输入串.
     * @param prefix 前缀 (空恒匹配).
     * @return 是前缀返回 true.
     */
    bool StartsWith(std::string_view s, std::string_view prefix);

    /**
     * @brief 是否以 suffix 结尾.
     * @param s      输入串.
     * @param suffix 后缀 (空恒匹配).
     * @return 是后缀返回 true.
     */
    bool EndsWith(std::string_view s, std::string_view suffix);

    /**
     * @brief 是否包含子串.
     * @param s   输入串.
     * @param sub 子串 (空恒包含).
     * @return 包含返回 true.
     */
    bool Contains(std::string_view s, std::string_view sub);

    /// @brief Split 选项 (可按位组合).
    enum class SplitOpt : uint8_t
    {
        kNone = 0,      ///< 无附加处理.
        kTrim = 1,      ///< 每段去两端空白.
        kSkipEmpty = 2, ///< 丢弃空段.
    };

    /// @brief 选项按位组合.
    inline SplitOpt operator|(SplitOpt a, SplitOpt b)
    {
        return (SplitOpt)((uint8_t)a | (uint8_t)b);
    }

    /// @brief 选项按位检测.
    inline SplitOpt operator&(SplitOpt a, SplitOpt b)
    {
        return (SplitOpt)((uint8_t)a & (uint8_t)b);
    }

    /**
     * @brief 按单字符分隔.
     * @param s     输入串.
     * @param delim 分隔符 (默认空格).
     * @param opt   附加选项 (默认无; kTrim / kSkipEmpty 可组合).
     * @return 分段结果 (默认连续分隔符产生空段).
     */
    std::vector<std::string> Split(std::string_view s, char delim = ',',
                                   SplitOpt opt = SplitOpt::kNone);

    /**
     * @brief 以 delim 拼接.
     * @param parts 分段.
     * @param delim 分隔符.
     * @return 拼接结果.
     */
    std::string Join(const std::vector<std::string> &parts,
                     std::string_view delim);

    /**
     * @brief 全量替换 from 为 to.
     * @param s    输入串.
     * @param from 待替换串 (空则原样返回).
     * @param to   替换为.
     * @return 替换后的新串.
     */
    std::string ReplaceAll(std::string_view s, std::string_view from,
                           std::string_view to);

    /**
     * @brief 字节串转小写 hex.
     * @param data 输入字节串.
     * @return hex 串 (长度加倍).
     */
    std::string HexEncode(std::string_view data);

    /**
     * @brief hex 串转字节.
     * @param hex 输入 hex 串.
     * @param out 输出字节串.
     * @return 成功 true 且写 out; 长度奇数或含非法字符 false (不写 out).
     */
    bool HexDecode(std::string_view hex, std::string &out);

    /**
     * @brief 十进制字符串转 int32 (支持正负号, 全串校验).
     * @param s   输入串.
     * @param out 输出值.
     * @return 成功 true 且写 out; 空串/非法/溢出 false (不写 out).
     */
    bool StrToInt32(std::string_view s, int32_t &out);

    /**
     * @brief 十进制字符串转 uint32 (仅非负, 全串校验).
     * @param s   输入串.
     * @param out 输出值.
     * @return 成功 true 且写 out; 空串/非法/负号/溢出 false (不写 out).
     */
    bool StrToUint32(std::string_view s, uint32_t &out);

    /**
     * @brief 十进制字符串转 int64 (支持正负号, 全串校验).
     * @param s   输入串.
     * @param out 输出值.
     * @return 成功 true 且写 out; 空串/非法/溢出 false (不写 out).
     */
    bool StrToInt64(std::string_view s, int64_t &out);

    /**
     * @brief 十进制字符串转 uint64 (仅非负, 全串校验).
     * @param s   输入串.
     * @param out 输出值.
     * @return 成功 true 且写 out; 空串/非法/负号/溢出 false (不写 out).
     */
    bool StrToUint64(std::string_view s, uint64_t &out);

    /**
     * @brief URL 编码 (UTF-8; 非保留字符原样, 其余 %XX).
     * @param value 输入串.
     * @return 编码后的新串.
     */
    std::string UrlEncode(std::string_view value);

    /**
     * @brief URL 解码 (UTF-8); 非法 % 序列按字面保留.
     * @param value         输入串.
     * @param plus_as_space true 按 query/表单语义 (+ -> 空格),
     *                      false 按路径语义 (+ 为字面字符).
     * @return 解码后的新串.
     */
    std::string UrlDecode(std::string_view value, bool plus_as_space = false);
}
