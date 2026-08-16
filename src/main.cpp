#include "../include/analyze.hpp"
#include "../include/ast_printer.hpp"
#include "../include/bytecode_compiler.hpp"
#include "../include/generic_reflexpr.hpp"
#include "../include/bytecode_decoder.hpp"
#include "../include/bytecode_decompiler.hpp"
#include "../include/errors.hpp"
#include "../include/keywords.hpp"
#include "../include/lexer.hpp"
#include "../include/native/native_compiler.hpp"
#include "../include/parser.hpp"
#include "../include/platform.hpp"
#include "../include/vm.hpp"
#if MUNX_VM_HAS_NAMED_PIPES
#include "../include/pipe_hub_daemon.hpp"
#endif
#include <fstream>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{

    std::string read_file_or_fail(const std::filesystem::path &path)
    {
        std::ifstream input{path};
        if (!input)
        {
            munx::fail_compile("could not open source file: " + path.string());
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

    void print_usage(const char *program)
    {
        std::cerr << "Usage: " << program << " <file.mx>\n"
                  << "       " << program
                  << " --native [-o out] [--backend custom|llvm] [--asm] "
                     "[--arch x86_64|aarch64] <file.mx>\n"
                  << "       " << program
                  << " --run [--interp] <file.mx|file.mxb> [args ...]\n"
                  << "       " << program << " --ast <file.mx>\n"
                  << "       " << program << " --tokens <file.mx>\n"
                  << "       " << program << " --decode <file.mxb>\n"
                  << "       " << program << " --decompile <file.mxb>\n"
                  << "       " << program << " --files <file.mx> [file.mx ...]\n"
                  << "       " << program << " --tokens --files <file.mx> [file.mx ...]\n"
                  << "       " << program
                  << " --analyze [--stdin --filename path.mx] <file.mx>\n"
                  << "       " << program << " --analyze-server\n"
                  << "\n"
                  << "Compile munx source file(s) to bytecode (<file>.mxb).\n"
                  << "Use --native to emit a host native executable (subset of the language).\n"
                  << "  --asm lowers MIR to machine code + object (ELF/Mach-O/COFF) then links.\n"
                  << "  --arch selects the ISA for --asm (default: host).\n"
                  << "Use --run to execute a program on the munx VM; a `.mx`\n"
                  << "  file is compiled first, and trailing arguments become argv.\n"
                  << "Use --interp with --run to force the bytecode interpreter\n"
                  << "  instead of the default threaded JIT (also MUNX_VM_JIT=0).\n"
                  << "Use --ast to print the AST instead of compiling.\n"
                  << "Use --tokens to print the token stream instead.\n"
                  << "Use --decode to validate and disassemble bytecode.\n"
                  << "Use --decompile to reconstruct source-like Munx.\n"
                  << "Use --files to compile multiple source files in one invocation.\n"
                  << "Use --analyze to emit a semantic JSON snapshot for tooling/LSP.\n"
                  << "Use --analyze-server for a persistent JSON-RPC analyze daemon.\n";
    }

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

    bool compile_file(const std::filesystem::path &source_path, bool tokens_only,
                      bool label_file, bool print_ast)
    {
        const munx::error compile_error = munx::run_compile_boundary([&] {
            const std::string source = read_file_or_fail(source_path);
            munx::lexer lex{source, munx::keywords(), source_path};
            std::vector<munx::token> tokens = lex.read_tokens();

            if (tokens_only)
            {
                if (label_file)
                {
                    std::cout << "; file: " << source_path.string() << '\n';
                }
                print_tokens(tokens);
                return;
            }

            munx::parser parse{std::move(tokens), source_path};
            munx::ast::program program = parse.parse_program();
            munx::expand_generics_and_reflexpr(program);

            if (print_ast)
            {
                if (label_file)
                {
                    std::cout << "; file: " << source_path.string() << '\n';
                }
                munx::ast_printer printer{std::cout};
                printer.print(program);
                return;
            }

            munx::bytecode_compiler compiler{source_path.parent_path(), program};
            std::filesystem::path output_path = source_path;
            output_path.replace_extension(".mxb");
            const size_t written = compiler.compile_to_file(output_path);
            if (munx::active_compile_context != nullptr &&
                munx::active_compile_context->failed())
            {
                return;
            }
            std::cout << "wrote " << output_path.string() << " (" << written
                      << " bytes)\n";
        });
        if (!compile_error.ok())
        {
            std::cerr << compile_error.message << '\n';
            return false;
        }
        return true;
    }

bool compile_native_file(const std::filesystem::path &source_path,
                             const std::filesystem::path &output_path,
                             munx::native::compile_options opts)
    {
        const munx::error compile_error = munx::run_compile_boundary([&] {
            const std::string source = read_file_or_fail(source_path);
            munx::lexer lex{source, munx::keywords(), source_path};
            munx::parser parse{lex.read_tokens(), source_path};
            munx::ast::program program = parse.parse_program();
            munx::expand_generics_and_reflexpr(program);
            munx::native::compile_to_executable(source_path.parent_path(), program,
                                                output_path, opts);
            std::cout << "wrote " << output_path.string() << '\n';
        });
        if (!compile_error.ok())
        {
            std::cerr << compile_error.message << '\n';
            return false;
        }
        return true;
    }

    bool decode_file(const std::filesystem::path &path, bool label_file)
    {
        const munx::error compile_error = munx::run_compile_boundary([&] {
            if (label_file)
            {
                std::cout << "; file: " << path.string() << '\n';
            }
            munx::decode_bytecode_file(path, std::cout);
        });
        if (!compile_error.ok())
        {
            std::cerr << compile_error.message << '\n';
            return false;
        }
        return true;
    }

    bool decompile_file(const std::filesystem::path &path, bool label_file)
    {
        const munx::error compile_error = munx::run_compile_boundary([&] {
            if (label_file)
            {
                std::cout << "// file: " << path.string() << '\n';
            }
            munx::decompile_bytecode_file(path, std::cout);
        });
        if (!compile_error.ok())
        {
            std::cerr << compile_error.message << '\n';
            return false;
        }
        return true;
    }

    int run_program(const std::filesystem::path &path,
                    std::vector<std::string> arguments)
    {
        std::filesystem::path image = path;
        int exit_status = 1;
        const munx::error compile_error = munx::run_compile_boundary([&] {
            if (path.extension() != ".mxb")
            {
                const std::string source = read_file_or_fail(path);
                munx::lexer lex{source, munx::keywords(), path};
                munx::parser parse{lex.read_tokens(), path};
                munx::ast::program program = parse.parse_program();
                munx::expand_generics_and_reflexpr(program);
                munx::bytecode_compiler compiler{path.parent_path(), program};
                image.replace_extension(".mxb");
                compiler.compile_to_file(image);
                if (munx::active_compile_context != nullptr &&
                    munx::active_compile_context->failed())
                {
                    return;
                }
            }
            exit_status = munx::vm::run_bytecode_file(image, arguments);
        });
        if (!compile_error.ok())
        {
            std::cerr << compile_error.message << '\n';
            return 1;
        }
        return exit_status;
    }

} // namespace

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

    if (argc >= 2 && std::string{argv[1]} == "--analyze-server")
    {
        return munx::analyze::run_analyze_server(std::cin, std::cout);
    }

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
    bool native_flag = false;
    bool files_mode = false;
    bool analyze_flag = false;
    bool analyze_stdin = false;
    std::filesystem::path analyze_filename;
    std::filesystem::path native_output;
    bool backend_explicit = false;
    bool asm_explicit = false;
    bool arch_explicit = false;
    munx::native::compile_options native_opts;
    std::vector<std::filesystem::path> source_paths;
    std::vector<std::string> program_arguments;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg{argv[i]};
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
        else if (arg == "--native")
        {
            native_flag = true;
        }
        else if (arg == "--analyze")
        {
            analyze_flag = true;
        }
        else if (arg == "--stdin")
        {
            analyze_stdin = true;
        }
        else if (arg == "--filename")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "--filename requires a path\n";
                print_usage(argv[0]);
                return 1;
            }
            analyze_filename = argv[++i];
        }
        else if (arg == "--asm")
        {
            asm_explicit = true;
            native_opts.backend = munx::native::backend_kind::asm_;
        }
        else if (arg == "--arch")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "--arch requires x86_64 or aarch64\n";
                print_usage(argv[0]);
                return 1;
            }
            const std::string arch{argv[++i]};
            if (!munx::native::asm_backend::parse_arch(arch, native_opts.asm_arch))
            {
                std::cerr << "Unknown --arch: " << arch << '\n';
                print_usage(argv[0]);
                return 1;
            }
            arch_explicit = true;
        }
        else if (arg == "-o")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "-o requires an output path\n";
                print_usage(argv[0]);
                return 1;
            }
            native_output = argv[++i];
        }
        else if (arg == "--backend")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "--backend requires custom, llvm, or asm\n";
                print_usage(argv[0]);
                return 1;
            }
            const std::string be{argv[++i]};
            backend_explicit = true;
            if (be == "custom")
            {
                native_opts.backend = munx::native::backend_kind::custom;
            }
            else if (be == "llvm")
            {
                native_opts.backend = munx::native::backend_kind::llvm;
            }
            else if (be == "asm")
            {
                native_opts.backend = munx::native::backend_kind::asm_;
            }
            else
            {
                std::cerr << "Unknown --backend: " << be << '\n';
                print_usage(argv[0]);
                return 1;
            }
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

    if (source_paths.empty() && !(analyze_flag && analyze_stdin))
    {
        if (files_mode)
        {
            std::cerr << "--files requires at least one source file\n";
        }
        print_usage(argv[0]);
        return 1;
    }

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
        static_cast<unsigned>(run_program_flag) +
        static_cast<unsigned>(native_flag) +
        static_cast<unsigned>(analyze_flag);
    if (output_mode_count > 1)
    {
        std::cerr << "--native, --run, --tokens, --ast, --decode, "
                     "--decompile, and --analyze are mutually exclusive\n";
        print_usage(argv[0]);
        return 1;
    }

    if (analyze_stdin && !analyze_flag)
    {
        std::cerr << "--stdin requires --analyze\n";
        print_usage(argv[0]);
        return 1;
    }
    if (!analyze_filename.empty() && !analyze_flag)
    {
        std::cerr << "--filename requires --analyze\n";
        print_usage(argv[0]);
        return 1;
    }

    if (analyze_flag)
    {
        std::filesystem::path path =
            !analyze_filename.empty()
                ? analyze_filename
                : (!source_paths.empty() ? source_paths.front()
                                         : std::filesystem::path{"stdin.mx"});
        std::optional<std::string> overlay;
        if (analyze_stdin)
        {
            std::ostringstream buffer;
            buffer << std::cin.rdbuf();
            overlay = buffer.str();
        }
        return munx::analyze::run_analyze_batch(path, overlay, std::cout);
    }

    if (backend_explicit && !native_flag)
    {
        std::cerr << "--backend is only valid with --native\n";
        print_usage(argv[0]);
        return 1;
    }
    if (asm_explicit && !native_flag)
    {
        std::cerr << "--asm is only valid with --native\n";
        print_usage(argv[0]);
        return 1;
    }
    if (arch_explicit && !native_flag)
    {
        std::cerr << "--arch is only valid with --native\n";
        print_usage(argv[0]);
        return 1;
    }
    if (backend_explicit && asm_explicit &&
        native_opts.backend != munx::native::backend_kind::asm_)
    {
        std::cerr << "--asm conflicts with --backend " 
                  << (native_opts.backend == munx::native::backend_kind::llvm
                          ? "llvm"
                          : "custom")
                  << '\n';
        return 1;
    }
    if (arch_explicit && native_opts.backend != munx::native::backend_kind::asm_)
    {
        std::cerr << "--arch requires --asm (or --backend asm)\n";
        return 1;
    }
    if (!native_output.empty() && !native_flag)
    {
        std::cerr << "-o is only valid with --native\n";
        print_usage(argv[0]);
        return 1;
    }

    if (run_program_flag)
    {
        return run_program(source_paths.front(), std::move(program_arguments));
    }

    if (native_flag)
    {
        if (files_mode || source_paths.size() != 1)
        {
            std::cerr << "--native requires exactly one source file\n";
            print_usage(argv[0]);
            return 1;
        }
#if MUNX_NATIVE_CUSTOM && MUNX_NATIVE_LLVM
        // both: default custom; --backend / --asm selects
#elif MUNX_NATIVE_LLVM && !MUNX_NATIVE_CUSTOM
        if (!backend_explicit && !asm_explicit)
        {
            native_opts.backend = munx::native::backend_kind::llvm;
        }
#elif !MUNX_NATIVE_CUSTOM && !MUNX_NATIVE_LLVM && !MUNX_NATIVE_ASM
#error "native backend configuration is empty"
#endif
        if (!munx::native::backend_available(native_opts.backend))
        {
            std::cerr << "native backend not available in this munxc build\n";
            return 1;
        }
        std::filesystem::path out = native_output;
        if (out.empty())
        {
            out = source_paths.front();
            out.replace_extension("");
#if defined(_WIN32)
            out += ".exe";
#endif
        }
        return compile_native_file(source_paths.front(), out, native_opts) ? 0 : 1;
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
