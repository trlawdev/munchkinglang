#pragma once

#include "errors.hpp"
#include <cstddef>
#include <iostream>
#include <utility>

namespace munx
{
    enum class log_level
    {
        error = 31,
        critical = 31,
        warn = 33,
        info = 32
    };
    struct reset
    {
    };

    // Structural string wrapper so format literals can be NTTPs (C++20).
    template <std::size_t N>
    struct fixed_string
    {
        char data[N]{};

        constexpr fixed_string(const char (&s)[N]) noexcept
        {
            for (std::size_t i = 0; i < N; ++i)
            {
                data[i] = s[i];
            }
        }

        constexpr const char *c_str() const noexcept { return data; }
        static constexpr std::size_t size() noexcept { return N - 1; }
    };

    constexpr std::ostream &operator<<(std::ostream &os, log_level level)
    {
        return os << "\033[" << static_cast<int>(level) << "m";
    }

    constexpr std::ostream &operator<<(std::ostream &os, reset)
    {
        return os << "\033[0m";
    }

    // Pure compile-time helpers — no I/O, so they may be consteval.
    consteval void validate_escape(const char *fmt)
    {
        switch (*(fmt + 1))
        {
        case '{':
        case '}':
        case '\\':
        case 'a':
        case 'n':
        case 't':
            return;
        default:
            throw compilation_error{"unrecognized escape character"};
        }
    }

    // Counts `{}` placeholders, honoring `\` escapes (same rules as the runtime formatter).
    consteval std::size_t count_open_close_braces(const char *fmt)
    {
        std::size_t count = 0;
        while (*fmt)
        {
            if (*fmt == '\\')
            {
                if (!*(fmt + 1))
                {
                    throw compilation_error{"escape sequence required after \\"};
                }
                validate_escape(fmt);
                fmt += 2;
            }
            else if (*fmt == '{' && *(fmt + 1) == '}')
            {
                count++;
                fmt += 2;
            }
            else
            {
                fmt++;
            }
        }
        return count;
    }

    consteval void validate_format_string(const char *fmt)
    {
        while (*fmt)
        {
            switch (*fmt)
            {
            case '{':
                if (*(fmt + 1) && *(fmt + 1) == '}')
                {
                    fmt += 2;
                    break;
                }
                throw compilation_error{
                    "expected matching `}` for format specifier in "
                    "string literal"};
            case '}':
                throw compilation_error{
                    "matching `{` not found, if you intend to type `}` escape it `\\`"};
            case '\\':
                if (!*(fmt + 1))
                {
                    throw compilation_error{"escape sequence required after \\"};
                }
                validate_escape(fmt);
                fmt += 2;
                break;
            default:
                fmt++;
                break;
            }
        }
    }

    consteval void validate_format(const char *fmt, std::size_t arg_count)
    {
        validate_format_string(fmt);
        if (count_open_close_braces(fmt) != arg_count)
        {
            throw compilation_error{"format specifier count does not match argument count"};
        }
    }

    class logger
    {
        struct formatter
        {
        private:
            std::ostream &output_stream_handle_;

            constexpr const char *handle_escape_sequences(const char *fmt) const
            {
                switch (*(fmt + 1))
                {
                case '{':
                    output_stream_handle_ << '{';
                    fmt += 2;
                    break;
                case '}':
                    output_stream_handle_ << '}';
                    fmt += 2;
                    break;
                case '\\':
                    output_stream_handle_ << '\\';
                    fmt += 2;
                    break;
                case 'a':
                    output_stream_handle_ << '\a';
                    fmt += 2;
                    break;
                case 'n':
                    output_stream_handle_ << '\n';
                    fmt += 2;
                    break;
                case 't':
                    output_stream_handle_ << '\t';
                    fmt += 2;
                    break;
                default:
                    throw compilation_error{"unrecognized escape character"};
                }

                return fmt;
            }

            // Emit literal text / escapes until the next `{}`, then stop (or finish).
            constexpr const char *emit_until_placeholder(const char *fmt) const
            {
                while (*fmt)
                {
                    switch (*fmt)
                    {
                    case '{':
                    {
                        if (*(fmt + 1) && *(fmt + 1) == '}')
                        {
                            return fmt; // caller consumes the placeholder
                        }
                        throw compilation_error{
                            "expected matching `}` for format specifier in "
                            "string literal"};
                    }
                    case '\\':
                        fmt = handle_escape_sequences(fmt);
                        break;
                    case '}':
                        throw compilation_error{"matching `{` not found, if you intend to type `}` escape it `\\`"};
                    default:
                        output_stream_handle_ << *fmt;
                        fmt++;
                        break;
                    }
                }
                return fmt;
            }

            // Base case: no more arguments — format string must be exhausted of `{}`.
            constexpr void format_impl(const char *fmt) const
            {
                fmt = emit_until_placeholder(fmt);
                if (*fmt == '{' && *(fmt + 1) == '}')
                {
                    throw compilation_error{"there are more format specifiers than arguments"};
                }
            }

            template <typename First, typename... Rest>
            constexpr void format_impl(const char *fmt, First &&first, Rest &&...rest) const
            {
                fmt = emit_until_placeholder(fmt);
                if (!(*fmt == '{' && *(fmt + 1) == '}'))
                {
                    throw compilation_error{"there are more arguments than format specifiers"};
                }
                output_stream_handle_ << std::forward<First>(first);
                format_impl(fmt + 2, std::forward<Rest>(rest)...);
            }

        public:
            constexpr formatter(std::ostream &stream)
                : output_stream_handle_(stream)
            {
            }

            template <typename First, typename... Rest>
            constexpr std::ostream &format(const char *fmt, First &&first, Rest &&...rest) const
            {
                format_impl(fmt, std::forward<First>(first), std::forward<Rest>(rest)...);
                return output_stream_handle_;
            }
        };

        std::ostream &stream_handle_; // output stream handle
        formatter formatter_;
        long long line_number_;
        long long line_char_position_;

    public:
        constexpr logger(std::ostream &stream, long long line_number, long long line_char_position)
            : stream_handle_(stream),
              formatter_(stream),
              line_number_(line_number),
              line_char_position_(line_char_position)
        {
        }

        // Format string is an NTTP so validate_format is a true consteval call.
        template <log_level level, fixed_string fmt, typename First, typename... Rest>
        constexpr void log_fmt(First &&first, Rest &&...rest)
        {
            validate_format(fmt.c_str(), 1 + sizeof...(Rest));
            stream_handle_ << level;
            formatter_.format(fmt.c_str(), std::forward<First>(first), std::forward<Rest>(rest)...);
            stream_handle_ << reset{};
        }

        template <log_level level>
        constexpr void log_line(const char *msg)
        {
            stream_handle_ << level << msg << reset{} << '\n';
        }
    };

} // namespace munx
