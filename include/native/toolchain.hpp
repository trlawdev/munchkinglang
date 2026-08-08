#pragma once

#include "../errors.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace munx::native
{

#ifndef MUNX_NATIVE_RUNTIME_DIR
#define MUNX_NATIVE_RUNTIME_DIR ""
#endif

inline std::filesystem::path runtime_dir()
{
    const char *env = std::getenv("MUNX_NATIVE_RUNTIME_DIR");
    if (env && env[0] != '\0')
    {
        return std::filesystem::path{env};
    }
    if (MUNX_NATIVE_RUNTIME_DIR[0] != '\0')
    {
        return std::filesystem::path{MUNX_NATIVE_RUNTIME_DIR};
    }
    return std::filesystem::path{"native/runtime"};
}

inline std::string find_host_cc()
{
    const char *env = std::getenv("CC");
    if (env && env[0] != '\0')
    {
        return env;
    }
#if defined(_WIN32)
    return "clang";
#else
    if (std::system("command -v clang >/dev/null 2>&1") == 0)
    {
        return "clang";
    }
    if (std::system("command -v cc >/dev/null 2>&1") == 0)
    {
        return "cc";
    }
    return "gcc";
#endif
}

inline int run_command(const std::string &cmd, std::string &stderr_out)
{
    stderr_out.clear();
#if defined(_WIN32)
    FILE *pipe = _popen((cmd + " 2>&1").c_str(), "r");
#else
    FILE *pipe = popen((cmd + " 2>&1").c_str(), "r");
#endif
    if (!pipe)
    {
        stderr_out = "failed to spawn host compiler";
        return 1;
    }
    std::array<char, 512> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr)
    {
        stderr_out += buf.data();
    }
#if defined(_WIN32)
    return _pclose(pipe);
#else
    const int rc = pclose(pipe);
    if (WIFEXITED(rc))
    {
        return WEXITSTATUS(rc);
    }
    return 1;
#endif
}

inline void ensure_runtime(std::filesystem::path &value_c,
                           std::filesystem::path &print_c,
                           std::filesystem::path &pipe_c)
{
    const std::filesystem::path rt = runtime_dir();
    value_c = rt / "munx_value.c";
    print_c = rt / "munx_print.c";
    pipe_c = rt / "munx_pipe.c";
    if (!std::filesystem::exists(value_c) || !std::filesystem::exists(print_c) ||
        !std::filesystem::exists(pipe_c))
    {
        fail_compile("native: runtime not found at " + rt.string() +
            " (set MUNX_NATIVE_RUNTIME_DIR)");
    }
}

inline std::string find_host_clang()
{
    const char *env = std::getenv("CLANG");
    if (env && env[0] != '\0')
    {
        return env;
    }
#if defined(_WIN32)
    return "clang";
#else
    if (std::system("command -v clang >/dev/null 2>&1") == 0)
    {
        return "clang";
    }
    if (std::system("command -v clang-14 >/dev/null 2>&1") == 0)
    {
        return "clang-14";
    }
    return "clang";
#endif
}

/// Compile generated C + runtime into @p output_exe using the host C driver.
inline void link_generated_c(const std::string &c_source,
                             const std::filesystem::path &output_exe)
{
    std::filesystem::path value_c;
    std::filesystem::path print_c;
    std::filesystem::path pipe_c;
    ensure_runtime(value_c, print_c, pipe_c);
    const std::filesystem::path rt = runtime_dir();

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("munx_native_" + std::to_string(stamp) + ".c");

    {
        std::ofstream out{tmp};
        if (!out)
        {
            fail_compile("native: could not write temp C file");
            return;
        }
        out << c_source;
    }

    if (active_compile_context != nullptr && active_compile_context->failed())
    {
        return;
    }

    const std::string cc = find_host_cc();
    std::ostringstream cmd;
    cmd << cc << " -std=c11 -O2 -pthread -I\"" << rt.string() << "\" \""
        << tmp.string() << "\" \"" << value_c.string() << "\" \""
        << print_c.string() << "\" \"" << pipe_c.string() << "\" -o \""
        << output_exe.string() << "\"";

    std::string err;
    const int rc = run_command(cmd.str(), err);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    if (rc != 0)
    {
        fail_compile("native: host C compile failed:\n" + err);
        return;
    }
}

/// Compile generated LLVM IR + runtime via clang (LLVM opts/codegen).
inline void link_generated_ll(const std::string &ll_source,
                              const std::filesystem::path &output_exe)
{
    std::filesystem::path value_c;
    std::filesystem::path print_c;
    std::filesystem::path pipe_c;
    ensure_runtime(value_c, print_c, pipe_c);
    const std::filesystem::path rt = runtime_dir();

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tmp = std::filesystem::temp_directory_path() /
                     ("munx_native_" + std::to_string(stamp) + ".ll");

    {
        std::ofstream out{tmp};
        if (!out)
        {
            fail_compile("native: could not write temp LLVM IR file");
            return;
        }
        out << ll_source;
    }

    const std::string clang = find_host_clang();
    std::ostringstream cmd;
    cmd << clang << " -O2 -pthread -Wno-override-module -I\"" << rt.string()
        << "\" \"" << tmp.string() << "\" \"" << value_c.string() << "\" \""
        << print_c.string() << "\" \"" << pipe_c.string() << "\" -o \""
        << output_exe.string() << "\"";

    std::string err;
    const int rc = run_command(cmd.str(), err);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    if (rc != 0)
    {
        fail_compile("native: LLVM IR compile failed:\n" + err);
        return;
    }
}

} // namespace munx::native
