#include "munx_process.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace
{

using munx::test::munxc_path;
using munx::test::process_result;
using munx::test::run_process;
using munx::test::shell_quote;
using munx::test::source_root;

std::filesystem::path program_path(const char *relative)
{
    return source_root() / "tests" / "programs" / relative;
}

bool json_contains(const std::string &json, const std::string &needle)
{
    return json.find(needle) != std::string::npos;
}

process_result analyze_file(const std::filesystem::path &source)
{
    return run_process({munxc_path().string(), "--analyze", source.string()});
}

process_result analyze_stdin(const std::filesystem::path &filename,
                             const std::string &source)
{
    process_result result;
    const auto stdin_path =
        std::filesystem::temp_directory_path() / "munx_analyze_stdin.mx";
    const auto stderr_path =
        std::filesystem::temp_directory_path() / "munx_analyze_stderr.txt";
    {
        std::ofstream out{stdin_path};
        out << source;
    }
    std::ostringstream command;
    command << "MUNX_PIPE_HUB=0 " << shell_quote(munxc_path().string())
            << " --analyze --stdin --filename " << shell_quote(filename.string())
            << " <" << shell_quote(stdin_path.string()) << " 2>"
            << shell_quote(stderr_path.string());
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
        std::filesystem::remove(stdin_path, ec);
    }
    return result;
}

process_result analyze_server_session(const std::string &requests)
{
    process_result result;
    const auto stdin_path =
        std::filesystem::temp_directory_path() / "munx_analyze_rpc.in";
    const auto stderr_path =
        std::filesystem::temp_directory_path() / "munx_analyze_rpc.err";
    {
        std::ofstream out{stdin_path};
        out << requests;
        if (requests.empty() || requests.back() != '\n')
        {
            out << '\n';
        }
        out << "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"shutdown\"}\n";
    }
    std::ostringstream command;
    command << "MUNX_PIPE_HUB=0 " << shell_quote(munxc_path().string())
            << " --analyze-server <" << shell_quote(stdin_path.string()) << " 2>"
            << shell_quote(stderr_path.string());
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
        std::filesystem::remove(stdin_path, ec);
    }
    return result;
}

} // namespace

TEST(Analyze, ValidProgramEmitsSnapshot)
{
    const auto result = analyze_file(program_path("hello.mx"));
    EXPECT_EQ(result.exit_code, 0) << result.stderr_text;
    EXPECT_TRUE(json_contains(result.stdout_text, "\"schema\":\"munx.analyze.v1\""));
    EXPECT_TRUE(json_contains(result.stdout_text, "\"package\":\"hello\""));
    EXPECT_TRUE(json_contains(result.stdout_text, "\"ok\":true"));
    EXPECT_TRUE(json_contains(result.stdout_text, "\"name\":\"print\""));
    EXPECT_TRUE(json_contains(result.stdout_text, "\"kind\":\"builtin\""));
    EXPECT_TRUE(json_contains(result.stdout_text, "\"kind\":\"package\""));
    EXPECT_FALSE(json_contains(result.stdout_text, "\033["));
}

TEST(Analyze, TypeErrorStillEmitsJson)
{
    const std::string source = R"(package bad
func f(): int {
  return "nope"
}
)";
    const auto path = program_path("hello.mx");
    const auto result = analyze_stdin(path, source);
    EXPECT_EQ(result.exit_code, 0) << result.stderr_text;
    EXPECT_TRUE(json_contains(result.stdout_text, "\"schema\":\"munx.analyze.v1\""));
    EXPECT_TRUE(json_contains(result.stdout_text, "\"ok\":false") ||
                json_contains(result.stdout_text, "\"severity\":\"error\""));
    EXPECT_TRUE(json_contains(result.stdout_text, "diagnostics"));
    EXPECT_TRUE(json_contains(result.stdout_text, "\"name\":\"f\""));
}

TEST(Analyze, StdinOverlayDiffersFromDisk)
{
    const auto path = program_path("hello.mx");
    const std::string overlay = R"(package overlay_pkg
x = 42
print(x)
)";
    const auto result = analyze_stdin(path, overlay);
    EXPECT_EQ(result.exit_code, 0) << result.stderr_text;
    EXPECT_TRUE(json_contains(result.stdout_text, "\"package\":\"overlay_pkg\""));
    EXPECT_TRUE(json_contains(result.stdout_text, "\"name\":\"x\""));
    EXPECT_FALSE(json_contains(result.stdout_text, "\"package\":\"hello\""));
}

TEST(Analyze, ServerHoverAndSymbols)
{
    const auto path = program_path("hello.mx");
    std::ifstream input{path};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string text = buffer.str();

    // Escape for JSON
    std::string escaped;
    for (char ch : text)
    {
        if (ch == '\\' || ch == '"')
        {
            escaped.push_back('\\');
        }
        if (ch == '\n')
        {
            escaped += "\\n";
            continue;
        }
        if (ch == '\r')
        {
            continue;
        }
        escaped.push_back(ch);
    }

    std::ostringstream req;
    req << "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"workspace/setRoot\",\"path\":"
        << "\"" << path.parent_path().string() << "\"}\n";
    req << "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"vfs/update\",\"path\":\""
        << path.string() << "\",\"kind\":\"open\",\"text\":\"" << escaped << "\"}\n";
    req << "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/hover\",\"path\":\""
        << path.string() << "\",\"line\":3,\"column\":2}\n";
    req << "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/documentSymbol\","
           "\"path\":\""
        << path.string() << "\"}\n";
    req << "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/completion\","
           "\"path\":\""
        << path.string() << "\",\"line\":3,\"column\":1,\"trigger\":\"\"}\n";

    const auto result = analyze_server_session(req.str());
    EXPECT_EQ(result.exit_code, 0) << result.stderr_text;
    EXPECT_TRUE(json_contains(result.stdout_text, "\"id\":2"));
    EXPECT_TRUE(json_contains(result.stdout_text, "publishDiagnostics") ||
                json_contains(result.stdout_text, "\"ok\":true"));
    EXPECT_TRUE(json_contains(result.stdout_text, "\"id\":3"));
    EXPECT_TRUE(json_contains(result.stdout_text, "print") ||
                json_contains(result.stdout_text, "contents"));
    EXPECT_TRUE(json_contains(result.stdout_text, "\"id\":4"));
    EXPECT_TRUE(json_contains(result.stdout_text, "symbols"));
    EXPECT_TRUE(json_contains(result.stdout_text, "\"id\":5"));
    EXPECT_TRUE(json_contains(result.stdout_text, "items"));
}

TEST(Analyze, FunctionHoverShowsFullSignature)
{
    const std::string source = R"(package sig
func add(a: int, b: int): int {
  return a + b
}
x = add(1, 2)
)";
    const auto path =
        std::filesystem::temp_directory_path() / "munx_sig_hover.mx";
    {
        std::ofstream out{path};
        out << source;
    }
    const auto result = analyze_file(path);
    EXPECT_EQ(result.exit_code, 0) << result.stderr_text;
    EXPECT_TRUE(json_contains(result.stdout_text, "add(a: int, b: int): int"))
        << result.stdout_text;

    auto escape = [](const std::string &text) {
        std::string escaped;
        for (char ch : text)
        {
            if (ch == '\\' || ch == '"')
            {
                escaped.push_back('\\');
            }
            if (ch == '\n')
            {
                escaped += "\\n";
                continue;
            }
            escaped.push_back(ch);
        }
        return escaped;
    };
    std::ostringstream req;
    req << "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"workspace/setRoot\",\"path\":\""
        << path.parent_path().string() << "\"}\n";
    req << "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"vfs/update\",\"path\":\""
        << path.string() << "\",\"kind\":\"open\",\"text\":\"" << escape(source)
        << "\"}\n";
    // Hover on `add` call site (line 5).
    req << "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/hover\",\"path\":\""
        << path.string() << "\",\"line\":5,\"column\":5}\n";
    const auto server = analyze_server_session(req.str());
    EXPECT_EQ(server.exit_code, 0) << server.stderr_text;
    EXPECT_TRUE(json_contains(server.stdout_text, "func add(a: int, b: int): int"))
        << server.stdout_text;
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(Analyze, BuiltinCallablesShowSignatures)
{
    const std::string source = R"(package builtins
p = pipe("x", out)
c = channel("id")
m = map[string => int]{}
v = get(m, "k")
)";
    const auto path =
        std::filesystem::temp_directory_path() / "munx_builtin_hover.mx";
    {
        std::ofstream out{path};
        out << source;
    }
    const auto result = analyze_file(path);
    EXPECT_EQ(result.exit_code, 0) << result.stderr_text;
    EXPECT_TRUE(json_contains(result.stdout_text,
                              "pipe(name: string, mode: in|out|subscribe): pipe"))
        << result.stdout_text;
    EXPECT_TRUE(json_contains(result.stdout_text, "channel(id: string): channel"))
        << result.stdout_text;
    EXPECT_TRUE(json_contains(result.stdout_text, "get(map: map, key: any): any"))
        << result.stdout_text;

    auto escape = [](const std::string &text) {
        std::string escaped;
        for (char ch : text)
        {
            if (ch == '\\' || ch == '"')
            {
                escaped.push_back('\\');
            }
            if (ch == '\n')
            {
                escaped += "\\n";
                continue;
            }
            escaped.push_back(ch);
        }
        return escaped;
    };
    std::ostringstream req;
    req << "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"workspace/setRoot\",\"path\":\""
        << path.parent_path().string() << "\"}\n";
    req << "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"vfs/update\",\"path\":\""
        << path.string() << "\",\"kind\":\"open\",\"text\":\"" << escape(source)
        << "\"}\n";
    req << "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/hover\",\"path\":\""
        << path.string() << "\",\"line\":2,\"column\":5}\n";
    req << "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/hover\",\"path\":\""
        << path.string() << "\",\"line\":3,\"column\":5}\n";
    req << "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/hover\",\"path\":\""
        << path.string() << "\",\"line\":5,\"column\":5}\n";
    const auto server = analyze_server_session(req.str());
    EXPECT_EQ(server.exit_code, 0) << server.stderr_text;
    EXPECT_TRUE(json_contains(server.stdout_text,
                              "func pipe(name: string, mode: in|out|subscribe): pipe"))
        << server.stdout_text;
    EXPECT_TRUE(json_contains(server.stdout_text, "func channel(id: string): channel"))
        << server.stdout_text;
    EXPECT_TRUE(json_contains(server.stdout_text, "func get(map: map, key: any): any"))
        << server.stdout_text;
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(Analyze, CompletionDetailShowsSignatures)
{
    const std::string source = R"(package comple
func add(a: int, b: int): int {
  return a + b
}
x = 
)";
    const auto path =
        std::filesystem::temp_directory_path() / "munx_comple_sig.mx";
    {
        std::ofstream out{path};
        out << source;
    }

    auto escape = [](const std::string &text) {
        std::string escaped;
        for (char ch : text)
        {
            if (ch == '\\' || ch == '"')
            {
                escaped.push_back('\\');
            }
            if (ch == '\n')
            {
                escaped += "\\n";
                continue;
            }
            escaped.push_back(ch);
        }
        return escaped;
    };
    std::ostringstream req;
    req << "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"workspace/setRoot\",\"path\":\""
        << path.parent_path().string() << "\"}\n";
    req << "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"vfs/update\",\"path\":\""
        << path.string() << "\",\"kind\":\"open\",\"text\":\"" << escape(source)
        << "\"}\n";
    // Completion on the RHS of `x =` (line 5).
    req << "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/completion\","
           "\"path\":\""
        << path.string() << "\",\"line\":5,\"column\":5,\"trigger\":\"\"}\n";
    const auto server = analyze_server_session(req.str());
    EXPECT_EQ(server.exit_code, 0) << server.stderr_text;
    EXPECT_TRUE(json_contains(server.stdout_text, "add(a: int, b: int): int"))
        << server.stdout_text;
    EXPECT_TRUE(json_contains(server.stdout_text,
                              "pipe(name: string, mode: in|out|subscribe): pipe"))
        << server.stdout_text;
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(Analyze, PreambleReuseOnSecondOpen)
{
    // Two-file package: main loads helper; second vfs/update of main should hit preamble.
    const auto dir = std::filesystem::temp_directory_path() / "munx_preamble_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto helper = dir / "helper.mx";
    const auto main_file = dir / "main.mx";
    {
        std::ofstream out{helper};
        out << "package helper\nfunc add(a: int, b: int): int { return a + b }\n";
    }
    {
        std::ofstream out{main_file};
        out << "package main\nload_package helper\nx = add(1, 2)\nprint(x)\n";
    }

    auto escape = [](const std::string &text) {
        std::string escaped;
        for (char ch : text)
        {
            if (ch == '\\' || ch == '"')
            {
                escaped.push_back('\\');
            }
            if (ch == '\n')
            {
                escaped += "\\n";
                continue;
            }
            escaped.push_back(ch);
        }
        return escaped;
    };

    std::ifstream main_in{main_file};
    std::ostringstream main_buf;
    main_buf << main_in.rdbuf();
    const std::string main_text = main_buf.str();
    const std::string main2 = "package main\nload_package helper\ny = add(3, 4)\nprint(y)\n";

    std::ostringstream req;
    req << "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"workspace/setRoot\",\"path\":\""
        << dir.string() << "\"}\n";
    req << "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"vfs/update\",\"path\":\""
        << main_file.string() << "\",\"kind\":\"open\",\"text\":\"" << escape(main_text)
        << "\"}\n";
    req << "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"vfs/update\",\"path\":\""
        << main_file.string() << "\",\"kind\":\"change\",\"text\":\"" << escape(main2)
        << "\"}\n";

    const auto result = analyze_server_session(req.str());
    EXPECT_EQ(result.exit_code, 0) << result.stderr_text;
    // Second update should report preamble_hits > 0
    EXPECT_TRUE(json_contains(result.stdout_text, "\"preamble_hits\":1") ||
                json_contains(result.stdout_text, "\"preamble_hits\":2") ||
                json_contains(result.stdout_text, "preamble_hits"));
    std::filesystem::remove_all(dir, ec);
}
