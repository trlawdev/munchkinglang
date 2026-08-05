#pragma once

#include "errors.hpp"
#include "platform.hpp"
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <utility>

namespace munx
{
    /// ANSI SGR color codes used as log severity markers.
    enum class log_level
    {
        error = 35,     ///< Magenta — recoverable error.
        critical = 31,  ///< Red — fatal / critical.
        warn = 33,      ///< Yellow — warning.
        info = 32       ///< Green — informational.
    };
    /// Stream manipulator that resets ANSI colors (`\033[0m`).
    struct reset
    {
    };

    /// Emit the ANSI color sequence for @p level.
    inline std::ostream &operator<<(std::ostream &os, log_level level)
    {
        return os << "\033[" << static_cast<int>(level) << "m";
    }

    /// Emit the ANSI reset sequence.
    inline std::ostream &operator<<(std::ostream &os, reset)
    {
        return os << "\033[0m";
    }

    /// Stream manipulator that prints the current local timestamp with milliseconds.
    struct date_time_now {};

    /// Emit `YYYY-MM-DD HH:MM:SS.mmm` for @ref date_time_now.
    inline std::ostream &operator<<(std::ostream &os, date_time_now)
    {
        using namespace std::chrono;
        const auto sys_tp = system_clock::now();
        const auto ms = duration_cast<milliseconds>(sys_tp.time_since_epoch()) % 1000;
        const std::time_t tt = system_clock::to_time_t(sys_tp);
        std::tm tm{};
        munx::platform_localtime(tt, &tm);
        const auto fill = os.fill('0');
        os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.'
           << std::setw(3) << ms.count();
        os.fill(fill);
        return os;
    }

    /// No-op stream tag (placeholder for optional formatting).
    struct stream_nop {};
    /// Leave @p os unchanged.
    inline std::ostream& operator<<(std::ostream& os, stream_nop nop) {
        return os;
    }

    /// Lightweight `{}`-style formatter writing to an @c ostream.
    /// Uses ADL for `operator<<` on each argument (two-phase lookup).
    struct formatter
    {
    private:
        std::ostream &output_stream_handle_; ///< Destination stream.

        /// Count unescaped `{}` placeholders in @p fmt.
        static size_t count_open_close_braces(const char *fmt) noexcept
        {
            size_t count = 0;
            auto fmt_cpy = fmt;
            while (*fmt_cpy && *(fmt_cpy + 1))
            {
                if (*fmt_cpy == '{' && *(fmt_cpy + 1) == '}')
                {
                    count++;
                    fmt_cpy += 2;
                }
                else
                {
                    fmt_cpy++;
                }
            }
            return count;
        }

        /// Consume a `\X` escape at @p fmt and write the decoded char.
        /// @return Pointer past the escape sequence.
        const char *handle_escape_sequences(const char *fmt) const
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
                fail_compile("unrecognized escape character");
            }

            return fmt;
        }

        /// Emit literal text / escapes until the next `{}` (or end of string).
        /// @return Pointer at the placeholder or at `'\0'`.
        const char *emit_until_placeholder(const char *fmt) const
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
                    fail_compile(
                        "expected matching `}` for format specifier in "
                        "string literal");
                }
                case '\\':
                    fmt = handle_escape_sequences(fmt);
                    break;
                case '}':
                    fail_compile("matching `{` not found, if you intend to type `}` escape it `\\`");
                default:
                    output_stream_handle_ << *fmt;
                    fmt++;
                    break;
                }
            }
            return fmt;
        }

        /// Base case: no more arguments; remaining `{}` is an error.
        void format_impl(const char *fmt) const
        {
            fmt = emit_until_placeholder(fmt);
            if (*fmt == '{' && *(fmt + 1) == '}')
            {
                fail_compile("there are more format specifiers than arguments");
            }
        }

        /// Substitute the next `{}` with @p first, then recurse.
        template <typename First, typename... Rest>
        void format_impl(const char *fmt, First &&first, Rest &&...rest) const
        {
            fmt = emit_until_placeholder(fmt);
            if (!(*fmt == '{' && *(fmt + 1) == '}'))
            {
                fail_compile("there are more arguments than format specifiers");
            }
            output_stream_handle_ << std::forward<First>(first);
            format_impl(fmt + 2, std::forward<Rest>(rest)...);
        }

    public:
        /// Bind this formatter to @p stream.
        formatter(std::ostream &stream)
            : output_stream_handle_(stream)
        {
        }

        /// Format @p fmt with @p first / @p rest into the bound stream.
        /// @return Reference to the output stream.
        template <typename First, typename... Rest>
        std::ostream &format(const char *fmt, First &&first, Rest &&...rest) const
        {
            const size_t arg_count = 1 + sizeof...(Rest);
            if (count_open_close_braces(fmt) != arg_count)
            {
                fail_compile("format specifier count does not match argument count");
            }
            format_impl(fmt, std::forward<First>(first), std::forward<Rest>(rest)...);
            return output_stream_handle_;
        }
    };

    /// Colored, timestamped logger that prefixes messages with file:line:col.
    class logger
    {
        std::ostream &stream_handle_;     ///< Destination stream.
        std::filesystem::path path_;      ///< Source path shown in the prefix.
        formatter formatter_;             ///< `{}` formatter over @ref stream_handle_.
        long long line_number_;           ///< 1-based line for the prefix.
        long long line_char_position_;    ///< 1-based column for the prefix.

    public:
        /// Construct a logger writing to @p stream at @p file_path:@p line_number:@p line_char_position.
        logger(std::ostream &stream,  long long line_number, long long line_char_position, const std::filesystem::path& file_path = std::filesystem::current_path())
            : stream_handle_(stream),
              path_(file_path),
              formatter_(stream),
              line_number_(line_number),
              line_char_position_(line_char_position)
        {
        }

        /// Log at compile-time @p level with formatted @p fmt / args.
        template <log_level level, typename First, typename... Rest>
        constexpr void log_fmt(const char *fmt, First &&first, Rest &&...rest)
        {
            stream_handle_ << level;
            switch (level) {
                case munx::log_level::info:
                    stream_handle_ << "📝 info: ";
                    break;
                case munx::log_level::warn:
                    stream_handle_ << "⚠️ warning: ";
                    break;
                case munx::log_level::error:
                    stream_handle_ << "⛔ error: ";
                    break;
                case munx::log_level::critical:
                    stream_handle_ << "🚨 critical: ";
                    break;
            }
            stream_handle_ << date_time_now{} << ' ';
            stream_handle_ << path_.c_str() << ':'  << line_number_ << ':' << line_char_position_ << ' ';
            formatter_.format(fmt, std::forward<First>(first), std::forward<Rest>(rest)...);
            stream_handle_ << reset{};
        }

        /// Log at @ref log_level::error.
        template <typename First, typename... Rest>
        constexpr void log_error(const char* fmt, First&& first, Rest&&... rest) {
            log_fmt<log_level::error>(fmt, std::forward<First>(first), std::forward<Rest>(rest)...);
        }

        /// Log at @ref log_level::info.
        template <typename First, typename... Rest>
        constexpr void log_info(const char* fmt, First&& first, Rest&&... rest) {
            log_fmt<log_level::info>(fmt, std::forward<First>(first), std::forward<Rest>(rest)...);
        }

        /// Log at @ref log_level::warn.
        template <typename First, typename... Rest>
        constexpr void log_warn(const char* fmt, First&& first, Rest&&... rest) {
            log_fmt<log_level::warn>(fmt, std::forward<First>(first), std::forward<Rest>(rest)...);
        }

        /// Log at @ref log_level::critical.
        template <typename First, typename... Rest>
        constexpr void log_critical(const char* fmt, First&& first, Rest&&... rest) {
            log_fmt<log_level::critical>(fmt, std::forward<First>(first), std::forward<Rest>(rest)...);
        }

        /// Write a plain colored line (no timestamp / location prefix).
        template <log_level level>
        void log_line(const char *msg)
        {
            stream_handle_ << level << msg << reset{} << '\n';
        }
    };

} // namespace munx
