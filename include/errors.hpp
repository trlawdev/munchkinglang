#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

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

/// Accumulates the first compile-time diagnostic in a translation unit pass.
struct compile_context
{
    error err{};

    [[nodiscard]] bool failed() const noexcept { return !err.ok(); }

    void fail(error_code code, const std::string &message)
    {
        if (!failed())
        {
            err = error::make(code, message);
        }
    }

    void fail_compile(const std::string &message) { fail(error_code::compile, message); }
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
    // Outside a compile scope — treat as fatal for the process.
    std::fputs(message.c_str(), stderr);
    std::fputc('\n', stderr);
    std::abort();
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
