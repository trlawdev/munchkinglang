#include "../include/ast_printer.hpp"
#include "../include/bytecode_compiler.hpp"
#include "../include/bytecode_decoder.hpp"
#include "../include/bytecode_decompiler.hpp"
#include "../include/errors.hpp"
#include "../include/keywords.hpp"
#include "../include/lexer.hpp"
#include "../include/parser.hpp"
#include "../include/platform.hpp"
#include "../include/vm.hpp"
#if MUNX_VM_HAS_NAMED_PIPES
#include "../include/pipe_hub_daemon.hpp"
#endif
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{

    /// Read the entire contents of @p path as a string.
    /// @throws munx::compilation_error if the file cannot be opened.
    std::string read_file(const std::filesystem::path &path)
    {
        std::ifstream input{path};
        if (!input)
        {
            throw munx::compilation_error{"could not open source file: " + path.string()};
        }
        input.seekg(0, std::ios::end);
        const std::streamsize size = input.tellg();
        input.seekg(0, std::ios::beg);
        std::string contents;
        if (size > 0)
        {
            contents.resize(static_cast<size_t>(size));
            input.read(contents.data(), size);
        }
        return contents;
    }

    /// Print CLI usage for @p program to stderr.
    void print_usage(const char *program)
    {
        std::cerr << "Usage: " << program << " <file.mx>\n"
                  << "       " << program << " --run [--interp] <file.mx|file.mxb> [args ...]\n"
                  << "       " << program << " --ast <file.mx>\n"
                  << "       " << program << " --tokens <file.mx>\n"
                  << "       " << program << " --decode <file.mxb>\n"
                  << "       " << program << " --decompile <file.mxb>\n"
                  << "       " << program << " --files <file.mx> [file.mx ...]\n"
                  << "       " << program << " --tokens --files <file.mx> [file.mx ...]\n"
                  << "\n"
                  << "Compile munx source file(s) to bytecode (<file>.mxb).\n"
                  << "Use --run to execute a program on the munx VM; a `.mx`\n"
                  << "  file is compiled first, and trailing arguments become argv.\n"
                  << "Use --interp with --run to force the bytecode interpreter\n"
                  << "  instead of the default threaded JIT (also MUNX_VM_JIT=0).\n"
                  << "Use --ast to print the AST instead of compiling.\n"
                  << "Use --tokens to print the token stream instead.\n"
                  << "Use --decode to validate and disassemble bytecode.\n"
                  << "Use --decompile to reconstruct source-like Munx.\n"
                  << "Use --files to compile multiple source files in one invocation.\n";
    }

    /// Dump @p tokens as `type @ line:col payload` lines.
    void print_tokens(const std::vector<munx::token> &tokens)
    {
        for (const auto &tok : tokens)
        {
            std::cout << static_cast<int>(tok.type) << " @ " << tok.line << ':'
                      << tok.column;
            if (std::holds_alternative<std::string>(tok.value))
            {
                std::cout << " \"" << std::get<std::string>(tok.value) << '"';
            }
            else if (std::holds_alternative<long long>(tok.value))
            {
                std::cout << ' ' << std::get<long long>(tok.value);
            }
            else if (std::holds_alternative<long double>(tok.value))
            {
                std::cout << ' ' << std::get<long double>(tok.value);
            }
            std::cout << '\n';
        }
    }

    /// Lex, parse, and compile one source file to bytecode (the default),
    /// or print its token stream / AST instead.
    /// @param tokens_only If true, print the token stream and stop.
    /// @param label_file If true, prefix printed output with `; file: path`.
    /// @param print_ast If true, print the AST instead of emitting bytecode.
    /// @return True on success; false after printing a compilation error.
    bool compile_file(const std::filesystem::path &source_path, bool tokens_only,
                      bool label_file, bool print_ast)
    {
        try
        {
            const std::string source = read_file(source_path);
            munx::lexer lex{source, munx::keywords(), source_path};
            std::vector<munx::token> tokens = lex.read_tokens();

            if (tokens_only)
            {
                if (label_file)
                {
                    std::cout << "; file: " << source_path.string() << '\n';
                }
                print_tokens(tokens);
                return true;
            }

            munx::parser parse{std::move(tokens), source_path};
            const munx::ast::program program = parse.parse_program();

            if (print_ast)
            {
                if (label_file)
                {
                    std::cout << "; file: " << source_path.string() << '\n';
                }
                munx::ast_printer printer{std::cout};
                printer.print(program);
                return true;
            }

            munx::bytecode_compiler compiler{source_path.parent_path(), program};
            std::filesystem::path output_path = source_path;
            output_path.replace_extension(".mxb");
            const size_t written = compiler.compile_to_file(output_path);
            std::cout << "wrote " << output_path.string() << " (" << written
                      << " bytes)\n";
            return true;
        }
        catch (const munx::compilation_error &error)
        {
            std::cerr << error.what() << '\n';
            return false;
        }
    }

    /// Validate and disassemble one bytecode image.
    bool decode_file(const std::filesystem::path &path, bool label_file)
    {
        try
        {
            if (label_file)
            {
                std::cout << "; file: " << path.string() << '\n';
            }
            munx::decode_bytecode_file(path, std::cout);
            return true;
        }
        catch (const munx::compilation_error &error)
        {
            std::cerr << error.what() << '\n';
            return false;
        }
    }

    /// Validate and reconstruct source-like Munx from one bytecode image.
    bool decompile_file(const std::filesystem::path &path, bool label_file)
    {
        try
        {
            if (label_file)
            {
                std::cout << "// file: " << path.string() << '\n';
            }
            munx::decompile_bytecode_file(path, std::cout);
            return true;
        }
        catch (const munx::compilation_error &error)
        {
            std::cerr << error.what() << '\n';
            return false;
        }
    }

    /// Execute one program on the munx VM, compiling it first when @p path is
    /// a `.mx` source file.
    /// @param arguments Values exposed to the program as `argv`.
    /// @return The program's exit status, or 1 if it could not be loaded.
    int run_program(const std::filesystem::path &path,
                    std::vector<std::string> arguments)
    {
        try
        {
            std::filesystem::path image = path;
            if (path.extension() != ".mxb")
            {
                std::string source = read_file(path);
                munx::lexer lex{source, munx::keywords(), path};
                munx::parser parse{lex.read_tokens(), path};
                const munx::ast::program program = parse.parse_program();
                munx::bytecode_compiler compiler{path.parent_path(), program};
                image.replace_extension(".mxb");
                compiler.compile_to_file(image);
            }
            return munx::vm::run_bytecode_file(image, std::move(arguments));
        }
        catch (const munx::compilation_error &error)
        {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }

} // namespace

/// Munx compiler CLI: compile `.mx` files to bytecode (`<file>.mxb`).
/// Supports `--ast` / `--tokens` for front-end debugging and `--files`
/// for multi-file batches.
int main(int argc, char *argv[])
{
#if MUNX_VM_HAS_NAMED_PIPES
    if (argc >= 2 && std::string{argv[1]} == "--pipe-hub")
    {
        return munx::vm::pipe_hub::run_daemon();
    }
    munx::vm::pipe_hub::client::instance().set_executable_path(
        std::filesystem::absolute(argv[0]).string());
#endif

    if (argc < 2)
    {
        print_usage(argv[0]);
        return 1;
    }

    bool print_tokens_flag = false;
    bool print_ast_flag = false;
    bool decode_bytecode_flag = false;
    bool decompile_bytecode_flag = false;
    bool run_program_flag = false;
    bool files_mode = false;
    std::vector<std::filesystem::path> source_paths;
    std::vector<std::string> program_arguments;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg{argv[i]};
        // Once --run has its program, everything left belongs to the program.
        if (run_program_flag && !source_paths.empty())
        {
            program_arguments.push_back(arg);
        }
        else if (arg == "--interp")
        {
            munx::vm::jit::jit_force_interpreter() = true;
        }
        else if (arg == "--run")
        {
            run_program_flag = true;
        }
        else if (arg == "--tokens")
        {
            print_tokens_flag = true;
        }
        else if (arg == "--ast")
        {
            print_ast_flag = true;
        }
        else if (arg == "--decode")
        {
            decode_bytecode_flag = true;
        }
        else if (arg == "--decompile")
        {
            decompile_bytecode_flag = true;
        }
        else if (arg == "--files")
        {
            files_mode = true;
            // Collect every subsequent non-option argument as a source file.
            for (++i; i < argc; ++i)
            {
                const std::string file_arg{argv[i]};
                if (file_arg.starts_with('-'))
                {
                    std::cerr << "Unknown option after --files: " << file_arg << '\n';
                    print_usage(argv[0]);
                    return 1;
                }
                source_paths.emplace_back(file_arg);
            }
            break;
        }
        else if (arg.starts_with('-'))
        {
            std::cerr << "Unknown option: " << arg << '\n';
            print_usage(argv[0]);
            return 1;
        }
        else
        {
            source_paths.emplace_back(arg);
        }
    }

    if (source_paths.empty())
    {
        if (files_mode)
        {
            std::cerr << "--files requires at least one source file\n";
        }
        print_usage(argv[0]);
        return 1;
    }

    // Single-file form without --files: exactly one path expected.
    if (!files_mode && source_paths.size() > 1)
    {
        std::cerr << "Multiple source files require --files\n";
        print_usage(argv[0]);
        return 1;
    }

    const unsigned output_mode_count =
        static_cast<unsigned>(print_tokens_flag) +
        static_cast<unsigned>(print_ast_flag) +
        static_cast<unsigned>(decode_bytecode_flag) +
        static_cast<unsigned>(decompile_bytecode_flag) +
        static_cast<unsigned>(run_program_flag);
    if (output_mode_count > 1)
    {
        std::cerr << "--run, --tokens, --ast, --decode, and --decompile are "
                     "mutually exclusive\n";
        print_usage(argv[0]);
        return 1;
    }

    if (run_program_flag)
    {
        return run_program(source_paths.front(), std::move(program_arguments));
    }

    const bool label_file = source_paths.size() > 1;
    bool ok = true;
    for (const auto &path : source_paths)
    {
        bool succeeded = false;
        if (decode_bytecode_flag)
        {
            succeeded = decode_file(path, label_file);
        }
        else if (decompile_bytecode_flag)
        {
            succeeded = decompile_file(path, label_file);
        }
        else
        {
            succeeded = compile_file(path, print_tokens_flag,
                                     label_file, print_ast_flag);
        }
        if (!succeeded)
        {
            ok = false;
        }
    }
    return ok ? 0 : 1;
}
