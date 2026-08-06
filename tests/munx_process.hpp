#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <vector>

namespace munx::test
{

struct process_result
{
    int exit_code{1};
    std::string stdout_text;
    std::string stderr_text;
};

/// Resolve the munxc binary under test (env `MUNXC`, CMake define, or PATH).
inline std::filesystem::path munxc_path()
{
    if (const char *configured = std::getenv("MUNXC"))
    {
        return configured;
    }
#ifdef MUNXC_EXECUTABLE
    return MUNXC_EXECUTABLE;
#else
    return "munxc";
#endif
}

inline std::filesystem::path source_root()
{
    if (const char *configured = std::getenv("MUNX_SOURCE_ROOT"))
    {
        return configured;
    }
#ifdef MUNX_SOURCE_ROOT
    return MUNX_SOURCE_ROOT;
#else
    return std::filesystem::current_path();
#endif
}

inline std::string shell_quote(const std::string &value)
{
    std::string out = "'";
    for (char ch : value)
    {
        if (ch == '\'')
        {
            out += "'\\''";
        }
        else
        {
            out += ch;
        }
    }
    out += '\'';
    return out;
}

/// Run @p args as a subprocess; capture stdout/stderr and exit status.
inline process_result run_process(const std::vector<std::string> &args)
{
    process_result result;
    if (args.empty())
    {
        result.stderr_text = "empty command";
        return result;
    }

    std::ostringstream command;
    command << "MUNX_PIPE_HUB=0";
    for (const std::string &arg : args)
    {
        command << ' ' << shell_quote(arg);
    }
    // Merge stderr into a temp file so exit status is preserved.
    const auto stderr_path =
        std::filesystem::temp_directory_path() / "munx_test_stderr.txt";
    command << " 2>" << shell_quote(stderr_path.string());

    FILE *pipe = popen(command.str().c_str(), "r");
    if (pipe == nullptr)
    {
        result.stderr_text = "popen failed";
        return result;
    }

    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) !=
           nullptr)
    {
        result.stdout_text += buffer.data();
    }

    const int status = pclose(pipe);
    if (WIFEXITED(status))
    {
        result.exit_code = WEXITSTATUS(status);
    }
    else if (WIFSIGNALED(status))
    {
        result.exit_code = 128 + WTERMSIG(status);
    }
    else
    {
        result.exit_code = status;
    }

    if (std::filesystem::exists(stderr_path))
    {
        if (FILE *err = std::fopen(stderr_path.c_str(), "r"))
        {
            while (std::fgets(buffer.data(), static_cast<int>(buffer.size()),
                              err) != nullptr)
            {
                result.stderr_text += buffer.data();
            }
            std::fclose(err);
        }
        std::error_code ec;
        std::filesystem::remove(stderr_path, ec);
    }
    return result;
}

inline process_result compile_source(const std::filesystem::path &source)
{
    return run_process({munxc_path().string(), source.string()});
}

inline process_result run_source(const std::filesystem::path &source,
                                 const std::vector<std::string> &program_args = {})
{
    std::vector<std::string> args{munxc_path().string(), "--run", source.string()};
    args.insert(args.end(), program_args.begin(), program_args.end());
    return run_process(args);
}

inline process_result run_source_interp(const std::filesystem::path &source)
{
    return run_process({munxc_path().string(), "--run", "--interp", source.string()});
}

} // namespace munx::test
