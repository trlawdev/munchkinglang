#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace munx
{

/// Stable error categories for compile-time, runtime, and I/O failures.
enum class error_code : uint32_t
{
    ok = 0,
    compile,
    runtime,
    division_by_zero,
    overflow,
    invalid_argument,
    io,
    protocol,
    internal,
};

/// Lightweight status value returned from API boundaries.
struct error
{
    error_code code{error_code::ok};
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return code == error_code::ok; }

    static error success() { return {}; }

    static error make(error_code code, const std::string &message)
    {
        return error{code, message};
    }
};

/// Severity of a compiler diagnostic collected for the language server.
enum class diagnostic_severity : uint8_t
{
    error = 1,
    warning = 2,
};

/// File + 1-based span for a compile diagnostic or semantic occurrence.
struct diag_span
{
    std::string file{};
    long long line{0};
    long long column{0};
    long long end_line{0};
    long long end_column{0};
};

/// One parser / type-checker / resolver diagnostic.
struct diagnostic
{
    diagnostic_severity severity{diagnostic_severity::error};
    diag_span span{};
    std::string message;
};

/// Accumulates compile-time diagnostics for a translation-unit pass.
struct compile_context
{
    error err{};
    std::vector<diagnostic> diagnostics;
    /// When true, collect every diagnostic instead of stopping at the first error.
    bool collect_all{false};

    [[nodiscard]] bool failed() const noexcept { return !err.ok(); }

    void fail(error_code code, const std::string &message)
    {
        report(diagnostic_severity::error, {}, 0, 0, 0, 0, message, code);
    }

    void fail_compile(const std::string &message) { fail(error_code::compile, message); }

    void report(diagnostic_severity severity, const std::string &file, long long line,
                long long column, long long end_line, long long end_column,
                const std::string &message, error_code code = error_code::compile)
    {
        if (severity == diagnostic_severity::error && failed() && !collect_all)
        {
            return;
        }

        diagnostic item{};
        item.severity = severity;
        item.span.file = file;
        item.span.line = line;
        item.span.column = column;
        item.span.end_line = end_line != 0 ? end_line : line;
        item.span.end_column =
            end_column != 0 ? end_column : (column != 0 ? column + 1 : 0);
        item.message = message;
        diagnostics.push_back(std::move(item));

        if (severity == diagnostic_severity::error && !failed())
        {
            std::string formatted = message;
            if (!file.empty() && line != 0)
            {
                formatted = file + ':' + std::to_string(line) + ':' +
                            std::to_string(column) + ": error: " + message;
            }
            err = error::make(code, std::move(formatted));
        }
    }
};

inline thread_local compile_context *active_compile_context{nullptr};

/// RAII scope binding @p ctx as the active compile context for @ref fail_compile.
class compile_context_scope
{
    compile_context *previous_{active_compile_context};
    compile_context &ctx_;

public:
    explicit compile_context_scope(compile_context &ctx) : ctx_(ctx)
    {
        ctx_.err = {};
        ctx_.diagnostics.clear();
        active_compile_context = &ctx_;
    }

    ~compile_context_scope() { active_compile_context = previous_; }

    [[nodiscard]] compile_context &context() noexcept { return ctx_; }
};

/// Record a compile-time failure; returns immediately (no stack unwinding).
inline void fail_compile(const std::string &message)
{
    if (active_compile_context != nullptr)
    {
        active_compile_context->fail_compile(message);
        return;
    }
    std::fputs(message.c_str(), stderr);
    std::fputc('\n', stderr);
    std::abort();
}

/// Record a compile error at a source span.
inline void fail_compile_at(const std::string &file, long long line, long long column,
                            const std::string &message, long long end_column = 0)
{
    if (active_compile_context != nullptr)
    {
        active_compile_context->report(diagnostic_severity::error, file, line, column,
                                       line, end_column, message);
        return;
    }
    std::string formatted = file + ':' + std::to_string(line) + ':' +
                            std::to_string(column) + ": error: " + message;
    std::fputs(formatted.c_str(), stderr);
    std::fputc('\n', stderr);
    std::abort();
}

/// Record a compile warning at a source span.
inline void warn_compile_at(const std::string &file, long long line, long long column,
                            const std::string &message, long long end_column = 0)
{
    if (active_compile_context != nullptr)
    {
        active_compile_context->report(diagnostic_severity::warning, file, line, column,
                                       line, end_column, message);
        if (!active_compile_context->collect_all)
        {
            std::fprintf(stderr, "%s:%lld:%lld: warning: %s\n", file.c_str(),
                         static_cast<long long>(line), static_cast<long long>(column),
                         message.c_str());
        }
        return;
    }
    std::fprintf(stderr, "%s:%lld:%lld: warning: %s\n", file.c_str(),
                 static_cast<long long>(line), static_cast<long long>(column),
                 message.c_str());
}

/// Run @p body under a fresh @ref compile_context and return its error state.
template <typename Fn>
error run_compile_boundary(Fn &&body)
{
    compile_context ctx;
    compile_context_scope scope{ctx};
    body();
    return ctx.err;
}

/// Optional success value or an @ref error (C++20-friendly `expected` subset).
template <typename T>
struct result
{
    error err{};
    T value{};

    [[nodiscard]] bool ok() const noexcept { return err.ok(); }
    explicit operator bool() const noexcept { return ok(); }

    static result success(T v)
    {
        result out;
        out.value = std::move(v);
        return out;
    }

    static result failure(const error &e)
    {
        result out;
        out.err = e;
        return out;
    }
};

} // namespace munx
