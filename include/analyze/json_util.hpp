#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace munx::analyze::json
{

inline void append_escaped(std::string &out, std::string_view text)
{
    out.push_back('"');
    for (unsigned char ch : text)
    {
        switch (ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            }
            else
            {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
}

inline std::string quote(std::string_view text)
{
    std::string out;
    append_escaped(out, text);
    return out;
}

/// Minimal JSON value extractor for flat request objects (string/number fields).
class object_view
{
    std::string_view text_;

    [[nodiscard]] static std::string_view trim(std::string_view s)
    {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        {
            s.remove_prefix(1);
        }
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        {
            s.remove_suffix(1);
        }
        return s;
    }

    [[nodiscard]] std::string_view find_value(std::string_view key) const
    {
        const std::string needle = "\"" + std::string{key} + "\"";
        std::size_t pos = 0;
        while (true)
        {
            pos = text_.find(needle, pos);
            if (pos == std::string_view::npos)
            {
                return {};
            }
            std::size_t colon = text_.find(':', pos + needle.size());
            if (colon == std::string_view::npos)
            {
                return {};
            }
            ++colon;
            while (colon < text_.size() &&
                   std::isspace(static_cast<unsigned char>(text_[colon])))
            {
                ++colon;
            }
            if (colon >= text_.size())
            {
                return {};
            }
            if (text_[colon] == '"')
            {
                std::size_t end = colon + 1;
                while (end < text_.size())
                {
                    if (text_[end] == '\\' && end + 1 < text_.size())
                    {
                        end += 2;
                        continue;
                    }
                    if (text_[end] == '"')
                    {
                        return text_.substr(colon + 1, end - colon - 1);
                    }
                    ++end;
                }
                return {};
            }
            std::size_t end = colon;
            while (end < text_.size() && text_[end] != ',' && text_[end] != '}' &&
                   text_[end] != ']' &&
                   !std::isspace(static_cast<unsigned char>(text_[end])))
            {
                ++end;
            }
            return text_.substr(colon, end - colon);
        }
    }

public:
    explicit object_view(std::string_view text) : text_(text) {}

    [[nodiscard]] std::string string_field(std::string_view key,
                                           std::string_view fallback = {}) const
    {
        const std::string_view raw = find_value(key);
        if (raw.empty() && text_.find("\"" + std::string{key} + "\"") ==
                               std::string_view::npos)
        {
            return std::string{fallback};
        }
        std::string out;
        out.reserve(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i)
        {
            if (raw[i] == '\\' && i + 1 < raw.size())
            {
                const char n = raw[++i];
                switch (n)
                {
                case 'n':
                    out.push_back('\n');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case '"':
                case '\\':
                case '/':
                    out.push_back(n);
                    break;
                default:
                    out.push_back(n);
                    break;
                }
            }
            else
            {
                out.push_back(raw[i]);
            }
        }
        return out;
    }

    [[nodiscard]] long long int_field(std::string_view key, long long fallback = 0) const
    {
        const std::string_view raw = find_value(key);
        if (raw.empty())
        {
            return fallback;
        }
        char *end = nullptr;
        const long long value = std::strtoll(std::string{raw}.c_str(), &end, 10);
        if (end == nullptr || end == std::string{raw}.c_str())
        {
            return fallback;
        }
        return value;
    }

    [[nodiscard]] bool has(std::string_view key) const
    {
        return text_.find("\"" + std::string{key} + "\"") != std::string_view::npos;
    }
};

} // namespace munx::analyze::json
