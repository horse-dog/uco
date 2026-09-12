#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uco
{

/**
 * @brief 轻量 YAML 配置读取器。
 *
 * 支持由空格缩进组成的嵌套 mapping、标量以及行内列表；配置键通过点号路径
 * 访问，例如 Get<int>("http.port", 8080)。不支持锚点、标签、多行字符串等
 * 完整 YAML 特性。所有读取接口均不抛异常，文件、语法、类型或范围错误时
 * 返回调用方提供的默认值。
 */
class YamlConfig
{
  public:
    /**
     * @brief 从 YAML 文件加载配置。
     * @param path 配置文件路径。
     * @return 文件读取和 YAML 解析均成功时返回 true，否则清空当前配置并返回
     *         false。
     */
    bool Load(const std::string &path) noexcept;

    /** @brief 判断点号路径对应的配置是否存在。 */
    bool Contains(const std::string &key) const noexcept;

    /**
     * @brief 按类型读取标量，失败时返回 default_value。
     * @tparam T 支持 std::string、bool、整数和枚举类型。
     */
    template <class T>
    T Get(const std::string &key, T default_value) const noexcept
    {
        using Value = std::remove_cv_t<T>;
        try
        {
            if constexpr (std::same_as<Value, std::string>)
            {
                Value result;
                return TryGetString(key, result) ? std::move(result)
                                                 : std::move(default_value);
            }
            else if constexpr (std::same_as<Value, bool>)
            {
                Value result{};
                return TryGetBool(key, result) ? result : default_value;
            }
            else if constexpr (std::signed_integral<Value>)
            {
                std::int64_t result{};
                return TryGetInt(key, result) && std::in_range<Value>(result)
                           ? static_cast<Value>(result)
                           : default_value;
            }
            else if constexpr (std::unsigned_integral<Value>)
            {
                std::uint64_t result{};
                return TryGetUInt(key, result) && std::in_range<Value>(result)
                           ? static_cast<Value>(result)
                           : default_value;
            }
            else if constexpr (std::is_enum_v<Value>)
            {
                using Underlying = std::underlying_type_t<Value>;
                return static_cast<Value>(
                    Get<Underlying>(key, static_cast<Underlying>(default_value)));
            }
            else
            {
                static_assert(kUnsupportedType<Value>,
                              "YamlConfig::Get supports only string, bool, integers and enums");
            }
        }
        catch (...)
        {
            return default_value;
        }
    }

    /**
     * @brief 读取 YAML 行内列表，任意失败均返回空 vector。
     * @tparam T 支持 std::string、bool、整数和枚举类型。
     */
    template <class T>
    std::vector<T> GetList(const std::string &key) const noexcept
    {
        try
        {
            std::vector<std::string> values;
            if (!TryGetRawList(key, values))
            {
                return {};
            }

            std::vector<T> result;
            result.reserve(values.size());
            for (const std::string &value : values)
            {
                T item{};
                if (!TryConvertListValue(value, item))
                {
                    return {};
                }
                result.push_back(std::move(item));
            }
            return result;
        }
        catch (...)
        {
            return {};
        }
    }

  private:
    template <class> static constexpr bool kUnsupportedType = false;

    template <class T>
    bool TryConvertListValue(const std::string &value,
                             T &result) const noexcept
    {
        using Value = std::remove_cv_t<T>;
        if constexpr (std::same_as<Value, std::string>)
        {
            try
            {
                result = value;
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
        else if constexpr (std::same_as<Value, bool>)
        {
            return TryParseBool(value, result);
        }
        else if constexpr (std::signed_integral<Value>)
        {
            std::int64_t parsed{};
            if (!TryParseInt(value, parsed) || !std::in_range<Value>(parsed))
            {
                return false;
            }
            result = static_cast<Value>(parsed);
            return true;
        }
        else if constexpr (std::unsigned_integral<Value>)
        {
            std::uint64_t parsed{};
            if (!TryParseUInt(value, parsed) || !std::in_range<Value>(parsed))
            {
                return false;
            }
            result = static_cast<Value>(parsed);
            return true;
        }
        else if constexpr (std::is_enum_v<Value>)
        {
            using Underlying = std::underlying_type_t<Value>;
            Underlying parsed{};
            if (!TryConvertListValue(value, parsed))
            {
                return false;
            }
            result = static_cast<Value>(parsed);
            return true;
        }
        else
        {
            static_assert(kUnsupportedType<Value>,
                          "YamlConfig::GetList supports only string, bool, integers and enums");
        }
    }

    bool TryGetString(const std::string &key, std::string &value) const noexcept;
    bool TryGetInt(const std::string &key, std::int64_t &value) const noexcept;
    bool TryGetUInt(const std::string &key, std::uint64_t &value) const noexcept;
    bool TryGetBool(const std::string &key, bool &value) const noexcept;
    bool TryGetRawList(const std::string &key,
                       std::vector<std::string> &value) const noexcept;
    static bool TryParseInt(const std::string &text,
                            std::int64_t &value) noexcept;
    static bool TryParseUInt(const std::string &text,
                             std::uint64_t &value) noexcept;
    static bool TryParseBool(std::string text, bool &value) noexcept;

    std::unordered_map<std::string, std::string> m_values;
};

} // namespace uco
