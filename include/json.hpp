#pragma once

/// Minimal JSON → Munx value parser used by `parse_json` / reflection decode.
/// Exceptions are disabled in munxc builds, so errors use out-params / throw_error.

#include "vm_value.hpp"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace munx::vm::json
{

struct parse_result
{
    bool ok{true};
    std::string message;
    /// Parsed Munx value (named `payload` so it does not shadow `munx::vm::value`).
    value payload{};
};

class parser
{
public:
    explicit parser(std::string_view text) : text_(text) {}

    parse_result parse()
    {
        skip_ws();
        value root = parse_value();
        if (!ok_)
        {
            return {false, message_, {}};
        }
        skip_ws();
        if (pos_ != text_.size())
        {
            fail("unexpected trailing input");
            return {false, message_, {}};
        }
        return {true, {}, std::move(root)};
    }

private:
    std::string_view text_;
    size_t pos_{0};
    bool ok_{true};
    std::string message_;

    void fail(const std::string &message)
    {
        if (!ok_)
        {
            return;
        }
        ok_ = false;
        message_ = message + " at offset " + std::to_string(pos_);
    }

    bool at_end() const { return pos_ >= text_.size(); }

    char peek() const { return at_end() ? '\0' : text_[pos_]; }

    char get()
    {
        if (at_end())
        {
            fail("unexpected end of input");
            return '\0';
        }
        return text_[pos_++];
    }

    void skip_ws()
    {
        while (!at_end() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
                text_[pos_] == '\r'))
        {
            ++pos_;
        }
    }

    bool match(char c)
    {
        if (peek() != c)
        {
            return false;
        }
        ++pos_;
        return true;
    }

    void expect(char c)
    {
        if (!match(c))
        {
            fail(std::string{"expected '"} + c + "'");
        }
    }

    value parse_value()
    {
        if (!ok_)
        {
            return value{};
        }
        skip_ws();
        const char c = peek();
        if (c == '{')
        {
            return parse_object();
        }
        if (c == '[')
        {
            return parse_array();
        }
        if (c == '"')
        {
            return value{parse_string()};
        }
        if (c == 't' || c == 'f')
        {
            return parse_bool();
        }
        if (c == 'n')
        {
            return parse_null();
        }
        if (c == '-' || (c >= '0' && c <= '9'))
        {
            return parse_number();
        }
        fail("invalid JSON value");
        return value{};
    }

    value parse_null()
    {
        if (text_.substr(pos_, 4) != "null")
        {
            fail("expected null");
            return value{};
        }
        pos_ += 4;
        return value{};
    }

    value parse_bool()
    {
        if (text_.substr(pos_, 4) == "true")
        {
            pos_ += 4;
            return value{true};
        }
        if (text_.substr(pos_, 5) == "false")
        {
            pos_ += 5;
            return value{false};
        }
        fail("expected true or false");
        return value{};
    }

    std::string parse_string()
    {
        expect('"');
        std::string out;
        while (ok_ && !at_end())
        {
            const char c = get();
            if (!ok_)
            {
                return out;
            }
            if (c == '"')
            {
                return out;
            }
            if (c == '\\')
            {
                const char esc = get();
                if (!ok_)
                {
                    return out;
                }
                switch (esc)
                {
                case '"':
                case '\\':
                case '/':
                    out.push_back(esc);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u':
                {
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i)
                    {
                        const char h = get();
                        if (!ok_)
                        {
                            return out;
                        }
                        code <<= 4;
                        if (h >= '0' && h <= '9')
                        {
                            code |= static_cast<unsigned>(h - '0');
                        }
                        else if (h >= 'a' && h <= 'f')
                        {
                            code |= static_cast<unsigned>(h - 'a' + 10);
                        }
                        else if (h >= 'A' && h <= 'F')
                        {
                            code |= static_cast<unsigned>(h - 'A' + 10);
                        }
                        else
                        {
                            fail("invalid unicode escape");
                            return out;
                        }
                    }
                    if (code <= 0x7F)
                    {
                        out.push_back(static_cast<char>(code));
                    }
                    else if (code <= 0x7FF)
                    {
                        out.push_back(static_cast<char>(0xC0 | ((code >> 6) & 0x1F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    else
                    {
                        out.push_back(static_cast<char>(0xE0 | ((code >> 12) & 0x0F)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default:
                    fail("invalid escape sequence");
                    return out;
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20)
            {
                fail("unescaped control character in string");
                return out;
            }
            out.push_back(c);
        }
        fail("unterminated string");
        return out;
    }

    value parse_number()
    {
        const size_t start = pos_;
        (void)match('-');
        if (peek() == '0')
        {
            ++pos_;
        }
        else if (peek() >= '1' && peek() <= '9')
        {
            while (peek() >= '0' && peek() <= '9')
            {
                ++pos_;
            }
        }
        else
        {
            fail("invalid number");
            return value{};
        }
        bool is_float = false;
        if (match('.'))
        {
            is_float = true;
            if (peek() < '0' || peek() > '9')
            {
                fail("invalid fractional number");
                return value{};
            }
            while (peek() >= '0' && peek() <= '9')
            {
                ++pos_;
            }
        }
        if (peek() == 'e' || peek() == 'E')
        {
            is_float = true;
            ++pos_;
            if (peek() == '+' || peek() == '-')
            {
                ++pos_;
            }
            if (peek() < '0' || peek() > '9')
            {
                fail("invalid exponent");
                return value{};
            }
            while (peek() >= '0' && peek() <= '9')
            {
                ++pos_;
            }
        }
        const std::string token{text_.substr(start, pos_ - start)};
        if (!is_float)
        {
            char *end = nullptr;
            errno = 0;
            const long long parsed = std::strtoll(token.c_str(), &end, 10);
            if (end != nullptr && *end == '\0' && errno != ERANGE)
            {
                return value{static_cast<int64_t>(parsed)};
            }
        }
        char *end = nullptr;
        errno = 0;
        const double parsed = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0' || errno == ERANGE ||
            !std::isfinite(parsed))
        {
            fail("invalid number");
            return value{};
        }
        return value{parsed};
    }

    value parse_array()
    {
        expect('[');
        skip_ws();
        auto items = std::make_shared<sequence_object>();
        if (match(']'))
        {
            return value{array_value{std::move(items)}};
        }
        while (ok_)
        {
            items->items.push_back(parse_value());
            if (!ok_)
            {
                return value{};
            }
            skip_ws();
            if (match(']'))
            {
                break;
            }
            expect(',');
            skip_ws();
        }
        return value{array_value{std::move(items)}};
    }

    value parse_object()
    {
        expect('{');
        skip_ws();
        auto map = std::make_shared<map_object>();
        if (match('}'))
        {
            return value{map_value{std::move(map)}};
        }
        while (ok_)
        {
            skip_ws();
            if (peek() != '"')
            {
                fail("object keys must be strings");
                return value{};
            }
            const std::string key = parse_string();
            if (!ok_)
            {
                return value{};
            }
            skip_ws();
            expect(':');
            if (!ok_)
            {
                return value{};
            }
            value entry = parse_value();
            if (!ok_)
            {
                return value{};
            }
            value key_value{key};
            map_store_entry(*map, key_value, entry);
            skip_ws();
            if (match('}'))
            {
                break;
            }
            expect(',');
            skip_ws();
        }
        return value{map_value{std::move(map)}};
    }
};

inline parse_result parse(std::string_view text)
{
    return parser{text}.parse();
}

inline const char *value_kind_name(const value &item)
{
    if (item.is_null())
    {
        return "null";
    }
    if (item.get_if<string_value>())
    {
        return "string";
    }
    if (item.get_if<int64_t>())
    {
        return "int";
    }
    if (item.get_if<double>())
    {
        return "float";
    }
    if (item.get_if<bool>())
    {
        return "bool";
    }
    if (item.get_if<map_value>())
    {
        return "object";
    }
    if (item.get_if<array_value>())
    {
        return "array";
    }
    return type_name(item);
}

inline value require_kind(const value &item, const std::string &kind,
                          const std::string &context)
{
    if (kind == "string")
    {
        if (item.get_if<string_value>())
        {
            return item;
        }
        throw_error(context + ": expected string, got " + value_kind_name(item));
        return value{};
    }
    if (kind == "int")
    {
        if (item.get_if<int64_t>())
        {
            return item;
        }
        if (const auto *number = item.get_if<double>())
        {
            const double v = *number;
            if (std::isfinite(v) && std::floor(v) == v &&
                v >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
                v <= static_cast<double>(std::numeric_limits<int64_t>::max()))
            {
                return value{static_cast<int64_t>(v)};
            }
        }
        throw_error(context + ": expected int, got " + value_kind_name(item));
        return value{};
    }
    if (kind == "float")
    {
        if (item.get_if<double>())
        {
            return item;
        }
        if (const auto *number = item.get_if<int64_t>())
        {
            return value{static_cast<double>(*number)};
        }
        throw_error(context + ": expected float, got " + value_kind_name(item));
        return value{};
    }
    if (kind == "bool")
    {
        if (item.get_if<bool>())
        {
            return item;
        }
        throw_error(context + ": expected bool, got " + value_kind_name(item));
        return value{};
    }
    if (kind == "object")
    {
        if (item.get_if<map_value>())
        {
            return item;
        }
        throw_error(context + ": expected object, got " + value_kind_name(item));
        return value{};
    }
    if (kind == "array")
    {
        if (item.get_if<array_value>())
        {
            return item;
        }
        throw_error(context + ": expected array, got " + value_kind_name(item));
        return value{};
    }
    throw_error(context + ": unknown JSON kind `" + kind + "`");
    return value{};
}

inline value field_required(const value &object, const std::string &key,
                            const std::string &context)
{
    const map_value *map = object.get_if<map_value>();
    if (map == nullptr || !map->data)
    {
        throw_error(context + ": expected object before reading field `" + key + "`");
        return value{};
    }
    const value key_value{key};
    if (const value *found = map_find_entry(*map->data, key_value))
    {
        return *found;
    }
    throw_error(context + ": missing required field `" + key + "`");
    return value{};
}

} // namespace munx::vm::json
