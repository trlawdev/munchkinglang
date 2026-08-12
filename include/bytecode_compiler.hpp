#pragma once
#include "Opcode.hpp"
#include "ast.hpp"
#include "ast_printer.hpp"
#include "errors.hpp"
#include "lexer.hpp"
#include "logger.hpp"
#include "loop_simd_optimizer.hpp"
#include "parser.hpp"
#include "generic_reflexpr.hpp"
#include "type_checker.hpp"
#include "vm_value.hpp"
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace munx {

using vm::debug_loc_entry;

    /// Read the entire contents of @p path as a UTF-8 string.
    /// @throws compilation_error if the file cannot be opened.
    inline std::string read_file(const std::filesystem::path &path)
    {
        std::ifstream input{path};
        if (!input)
        {
            fail_compile("could not open source file: " + path.string());
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

    /// Print CLI usage for the compiler driver to stderr.
    inline void print_usage(const char *program)
    {
        std::cerr << "Usage: " << program << " <file.mx>\n"
                  << "       " << program << " --ast <file.mx>\n"
                  << "       " << program << " --tokens <file.mx>\n"
                  << "       " << program << " --decode <file.mxb>\n"
                  << "       " << program << " --decompile <file.mxb>\n"
                  << "       " << program << " --files <file.mx> [file.mx ...]\n"
                  << "       " << program << " --tokens --files <file.mx> [file.mx ...]\n"
                  << "\n"
                  << "Compile munx source file(s) to bytecode (<file>.mxb).\n"
                  << "Use --ast to print the AST instead of compiling.\n"
                  << "Use --tokens to print the token stream instead.\n"
                  << "Use --decode to validate and disassemble bytecode.\n"
                  << "Use --decompile to reconstruct source-like Munx.\n"
                  << "Use --files to compile multiple source files in one invocation.\n";
    }

    /// Lex and parse @p source_path into an AST program.
    /// @return The program on success, or empty optional after printing the error.
    inline std::optional<ast::program> compile_file(const std::filesystem::path &source_path)
    {
        std::optional<ast::program> program;
        const munx::error compile_error = munx::run_compile_boundary([&] {
            const std::string source = read_file(source_path);
            munx::lexer lex{source, munx::keywords(), source_path};
            std::vector<munx::token> tokens = lex.read_tokens();
            parser parse{std::move(tokens), source_path};
            program = parse.parse_program();
            expand_generics_and_reflexpr(*program);
        });
        if (!compile_error.ok())
        {
            std::cerr << compile_error.message << '\n';
            return {};
        }
        return program;
    }

/// View wrapper so @ref formatter can stream a path set via ADL.
/// `os << unordered_set<path>` cannot work: formatter's `<<` is resolved by
/// ADL, and both set and path live in `std`, so free operators in `munx` are
/// never considered. Tagging the value with a munx type brings this overload
/// into the associated namespaces.
struct path_set_fmt
{
    const std::unordered_set<std::filesystem::path> &paths; ///< Paths to print.
};

/// Stream @p view as `{path1, path2, …}`.
inline std::ostream &operator<<(std::ostream &os, path_set_fmt view)
{
    os << '{';
    bool first = true;
    for (const auto &item : view.paths)
    {
        if (!first)
        {
            os << ", ";
        }
        first = false;
        os << item.string();
    }
    os << '}';
    return os;
}

/// Resolves `load_package` / `load_packages` imports against `*.mx` files
/// in the current working directory and parses each dependency.
/// Resolution is transitive: dependencies of dependencies are loaded too,
/// each package exactly once (diamond imports and cycles are tolerated).
struct package_resolver {
    std::filesystem::path main_dir_path_;
    formatter formatter_;                                   ///< Error formatter (stderr).
    const ast::program& main_;                              ///< Root program being resolved.
    std::vector<ast::program> imports_package_programs{};   ///< Parsed imports, dependencies first.
    bool ok{true};                                          ///< False after any resolution failure.

    /// Bind to @p main; errors go to stderr.
    package_resolver(std::filesystem::path main_dir_path, const ast::program &main)
        : main_dir_path_(main_dir_path.empty()
                             ? std::filesystem::current_path()
                             : std::move(main_dir_path)),
          formatter_(std::cerr),
          main_(main)
    {}

    /// @return True if @p path has a `.mx` extension.
    bool is_munx_file(const std::filesystem::path &path) const noexcept {
        return path.extension() == ".mx";
    }

    [[nodiscard]] const std::filesystem::path &main_dir_path() const noexcept
    {
        return main_dir_path_;
    }

    /// Emit `file:line:col: error: …` using the import's AST @p loc.
    template <typename... Args>
    void error_at(const ast::source_loc &loc, const char *fmt, Args &&...args)
    {
        formatter_.format("{}: error: ", loc);
        formatter_.format(fmt, std::forward<Args>(args)...);
        ok = false;
    }

    /// Resolve every import of @ref main_ transitively; stop on first failure.
    void resolve() {
        std::unordered_set<std::filesystem::path> dir_package_set{};
        for (auto& dir_entry: std::filesystem::directory_iterator(main_dir_path_)) {
            if (is_munx_file(dir_entry.path())) {
                dir_package_set.emplace(dir_entry.path().filename());
            }
        }
        std::unordered_set<std::string> loaded{};
        loaded.insert(main_.package_name);
        resolve_imports(main_, loaded, dir_package_set);
    }

private:
    /// Depth-first resolution of @p program's imports; dependencies are
    /// appended to @ref imports_package_programs before their importers.
    void resolve_imports(const ast::program &program,
                         std::unordered_set<std::string> &loaded,
                         const std::unordered_set<std::filesystem::path> &dir_package_set)
    {
        for (const auto& package_stmt: program.imports) {
            if (loaded.contains(package_stmt.package)) {
                continue;
            }
            const auto filename = std::filesystem::path{package_stmt.package + ".mx"};
            if (!dir_package_set.contains(filename)) {
                error_at(package_stmt.loc,
                         "failed to find package `{}`\n", package_stmt.package);
                return;
            }
            loaded.insert(package_stmt.package);
            auto compiled_program = compile_file(main_dir_path_ / filename);
            if (!compiled_program.has_value()) {
                error_at(package_stmt.loc,
                         "failed to resolve package `{}`\n", package_stmt.package);
                return;
            }
            resolve_imports(compiled_program.value(), loaded, dir_package_set);
            if (!ok) {
                return;
            }
            imports_package_programs.emplace_back(std::move(compiled_program.value()));
        }
    }
};

// ---------------------------------------------------------------------------
// .mxb on-disk structures
// ---------------------------------------------------------------------------
// File layout (all offsets are absolute byte offsets into the file, except
// *_name_offset fields which are byte offsets into the string table):
//
//   [mx_program_header]
//   [mx_package_descriptor × num_package_import_list]   imported packages
//   per package (imports first, entry point last):
//       [mx_function_descriptor × num_function_descriptors]
//       [function bytecode blobs …]
//       [package top-level bytecode]
//   [string table]                                      raw UTF-8 bytes
//
// Structs are written verbatim (memcpy), so the VM can map them back with a
// single read on the same platform / compiler.

/// Version of the .mxb bytecode format emitted by this compiler.
/// Increment this whenever opcode encodings or the serialized layout change.
inline constexpr uint16_t current_mx_bytecode_version{8};

struct __attribute__((packed)) mx_function_descriptor {
    size_t function_name_offset; // offset into the string table
    size_t function_name_length; // function name length
    size_t function_content_offset;
    size_t function_content_length;
    size_t debug_map_offset;
    size_t debug_map_length;
};

struct __attribute__((packed)) mx_package_descriptor {
  size_t package_name_offset; //offset into the string table
  size_t package_name_length; //package name length
  size_t package_bytecode_offset; //offset in the file where content can be found
  size_t package_bytecode_length; //package bytecode length
  size_t function_descriptor_array_offset; //function table offset, it contains a list of function_descriptors
  size_t num_function_descriptors; //the number of functions in the package
  size_t init_debug_map_offset;
  size_t init_debug_map_length;
};


struct __attribute__((packed)) mx_program_header {
  std::byte mx_signature[2]; // "MX"
  uint16_t mx_bytecode_version; // bytecode format version
  size_t package_name_offset; // offset into the string table
  size_t package_name_length; // length of package name in table
  size_t package_import_array_offset; //offset in the file that contains all package descriptors
  size_t num_package_import_list; //number of imported package descriptors
  size_t entry_point_bytecode_offset;
  size_t entry_point_bytecode_length;
  size_t string_table_offset; //offset in the file where the string table starts
  size_t string_table_length; //string table size in bytes
  mx_package_descriptor entry_point_package_descriptor;
};

static_assert(std::is_trivially_copyable_v<mx_function_descriptor>);
static_assert(std::is_trivially_copyable_v<mx_package_descriptor>);
static_assert(std::is_trivially_copyable_v<mx_program_header>);
static_assert(sizeof(mx_function_descriptor) == 6 * sizeof(size_t),
              "mx_function_descriptor must contain no padding");
static_assert(sizeof(mx_package_descriptor) == 8 * sizeof(size_t),
              "mx_package_descriptor must contain no padding");
static_assert(sizeof(mx_program_header) ==
                  2 * sizeof(std::byte) + sizeof(uint16_t) +
                  8 * sizeof(size_t) + sizeof(mx_package_descriptor),
              "mx_program_header must contain no padding");

// ---------------------------------------------------------------------------
// Compilation building blocks
// ---------------------------------------------------------------------------

/// Reference to an interned string: byte offset + length in the string table.
struct __attribute__((packed)) string_ref
{
    uint32_t offset; ///< Byte offset into the string table.
    uint32_t length; ///< Byte length.
};

static_assert(std::is_trivially_copyable_v<string_ref>);
static_assert(sizeof(string_ref) == 2 * sizeof(uint32_t),
              "string_ref must contain no padding");

/// Deduplicating string table; holds every name and literal of the module.
class string_table
{
    std::vector<std::byte> data_;                        ///< Raw table bytes.
    std::unordered_map<std::string, uint32_t> interned_; ///< text → offset.

public:
    /// Intern @p text, returning its (offset, length) in the table.
    string_ref intern(std::string_view text)
    {
        const auto found = interned_.find(std::string{text});
        if (found != interned_.end())
        {
            return {found->second, static_cast<uint32_t>(text.size())};
        }
        const auto offset = static_cast<uint32_t>(data_.size());
        const auto *bytes = reinterpret_cast<const std::byte *>(text.data());
        data_.insert(data_.end(), bytes, bytes + text.size());
        interned_.emplace(std::string{text}, offset);
        return {offset, static_cast<uint32_t>(text.size())};
    }

    /// @return The raw table bytes.
    const std::vector<std::byte> &bytes() const noexcept { return data_; }
};

/// Append-only bytecode buffer with little-endian operand encoding and
/// back-patching support for forward jump targets.
class code_builder
{
    std::vector<std::byte> code_; ///< Encoded instructions.

public:
    /// @return Current size, i.e. the offset of the next instruction.
    uint32_t here() const noexcept { return static_cast<uint32_t>(code_.size()); }

    void op(Opcode opcode) { code_.push_back(static_cast<std::byte>(opcode)); }
    void u8(uint8_t value) { code_.push_back(static_cast<std::byte>(value)); }
    void u32(uint32_t value) { append(&value, sizeof value); }
    void i64(int64_t value) { append(&value, sizeof value); }
    void f64(double value) { append(&value, sizeof value); }
    void str(string_ref ref) { u32(ref.offset); u32(ref.length); }

    /// Emit @p opcode with a placeholder u32 target.
    /// @return Position of the operand, for a later @ref patch.
    uint32_t jump(Opcode opcode)
    {
        op(opcode);
        const uint32_t operand_at = here();
        u32(0);
        return operand_at;
    }

    /// Overwrite the u32 at @p operand_at with @p target.
    void patch(uint32_t operand_at, uint32_t target)
    {
        std::memcpy(code_.data() + operand_at, &target, sizeof target);
    }

    /// @return The finished blob, leaving this builder empty.
    std::vector<std::byte> take() { return std::move(code_); }

private:
    void append(const void *data, size_t size)
    {
        const auto *bytes = static_cast<const std::byte *>(data);
        code_.insert(code_.end(), bytes, bytes + size);
    }
};

/// One compiled function (named declaration or lambda) awaiting layout.
struct compiled_function
{
    std::string name;             ///< Function name (lambdas get generated names).
    std::vector<std::byte> code;  ///< Body bytecode, ends with RET.
    std::vector<debug_loc_entry> debug_map;
};

/// One compiled package awaiting layout.
struct compiled_package
{
    std::string name;                          ///< Declared package name.
    std::vector<std::byte> init_code;          ///< Top-level bytecode, ends with HALT.
    std::vector<debug_loc_entry> init_debug_map;
    std::vector<compiled_function> functions;  ///< Functions and lambdas.
};

/// Record a compile error formatted as `file:line:col: error: …`.
inline void codegen_error(const ast::source_loc &loc, const std::string &message)
{
    std::ostringstream out;
    out << loc << ": error: " << message;
    fail_compile(out.str());
}

/// Emits stack-VM bytecode for one code blob: either a function body or a
/// package's top-level statement list. Nested lambdas / function
/// declarations are compiled with fresh emitters and appended to the
/// package's function list.
class code_emitter
{
    string_table &strings_;                        ///< Module-wide string table.
    std::vector<compiled_function> &functions_;    ///< Current package's functions.
    size_t &lambda_counter_;                       ///< Package-wide lambda id source.
    std::unordered_set<std::string> &object_types_; ///< User object type names in this package.
    code_builder code_;                            ///< Output blob.
    std::vector<std::vector<uint32_t>> loop_breaks_; ///< Pending break patches per loop.
    std::vector<uint32_t> stray_breaks_;           ///< Breaks outside any loop; patched to blob end.
    const ast::type_node *current_return_type_{nullptr}; ///< Enclosing function return type.
    std::unordered_map<std::string, int64_t> const_locals_; ///< Loop-unroll induction bindings.
    std::vector<debug_loc_entry> debug_map_; ///< PC anchors for the current code blob.
    const type_annotation_map *types_{nullptr};

    static constexpr int64_t k_max_loop_unroll = 32;

    void emit_debug_anchor(const ast::source_loc &loc)
    {
        if (loc.line == 0)
        {
            return;
        }
        const uint32_t pc = static_cast<uint32_t>(code_.here());
        if (!debug_map_.empty() && debug_map_.back().pc == pc &&
            debug_map_.back().line == static_cast<uint32_t>(loc.line) &&
            debug_map_.back().column == static_cast<uint32_t>(loc.column) &&
            debug_map_.back().file == loc.file)
        {
            return;
        }
        debug_loc_entry entry{};
        entry.pc = pc;
        entry.line = static_cast<uint32_t>(loc.line);
        entry.column = static_cast<uint32_t>(loc.column);
        entry.file = loc.file;
        debug_map_.push_back(std::move(entry));
    }

public:
    code_emitter(string_table &strings, std::vector<compiled_function> &functions,
                 size_t &lambda_counter, std::unordered_set<std::string> &object_types,
                 const type_annotation_map *types = nullptr)
        : strings_(strings), functions_(functions), lambda_counter_(lambda_counter),
          object_types_(object_types), types_(types)
    {}

    /// Compile a function body: bind parameters, emit statements, and
    /// guarantee an implicit `return null` fall-through.
    std::vector<std::byte> emit_function_code(const std::vector<ast::parameter> &parameters,
                                              const ast::block_stmt &body,
                                              const ast::type_node *return_type)
    {
        current_return_type_ = return_type;
        // The caller pushes arguments left-to-right, so the last argument is
        // on top: bind parameters in reverse declaration order.
        for (auto it = parameters.rbegin(); it != parameters.rend(); ++it)
        {
            code_.op(Opcode::STORE);
            code_.str(strings_.intern(it->name));
        }
        emit_block_statements(body.statements);
        patch_stray_breaks();
        code_.op(Opcode::PUSH_NULL);
        code_.op(Opcode::RET);
        current_return_type_ = nullptr;
        return code_.take();
    }

    /// Compile a package's top-level statements, terminated by HALT.
    std::vector<std::byte> emit_toplevel(const std::vector<std::unique_ptr<ast::stmt_node>> &statements)
    {
        emit_block_statements(statements);
        patch_stray_breaks();
        code_.op(Opcode::HALT);
        return code_.take();
    }

    void clear_debug_map() { debug_map_.clear(); }

    std::vector<debug_loc_entry> take_debug_map() { return std::move(debug_map_); }

private:
    // -- helpers ------------------------------------------------------------

    /// Compile @p body as a function named @p name using a fresh emitter.
    compiled_function compile_function(const std::string &name,
                                       const std::vector<ast::parameter> &parameters,
                                       const ast::block_stmt &body,
                                       const ast::type_node *return_type)
    {
        code_emitter nested{strings_, functions_, lambda_counter_, object_types_, types_};
        nested.debug_map_.clear();
        compiled_function result{
            name,
            nested.emit_function_code(parameters, body, return_type),
            std::move(nested.debug_map_)};
        return result;
    }

    void emit_block(const ast::block_stmt &block) { emit_block_statements(block.statements); }

    void emit_block_statements(const std::vector<std::unique_ptr<ast::stmt_node>> &statements)
    {
        for (size_t index = 0; index < statements.size(); ++index)
        {
            emit_debug_anchor(statements[index]->loc);
            if (types_ != nullptr)
            {
                const auto simd_match =
                    match_simd_elementwise_loop_sequence(statements, index, *types_);
                if (simd_match.has_value())
                {
                    emit_simd_elementwise_loop(*simd_match);
                    index += simd_match->consumed_statements - 1;
                    continue;
                }
            }
            if (index + 1 < statements.size() &&
                try_emit_unrolled_loop(*statements[index], *statements[index + 1]))
            {
                ++index;
                continue;
            }
            emit_stmt(*statements[index]);
        }
    }

    /// Resolve `break` statements that had no enclosing loop: they jump to
    /// the end of the blob, i.e. fall into the implicit return / HALT.
    void patch_stray_breaks()
    {
        for (const uint32_t jump : stray_breaks_)
        {
            code_.patch(jump, code_.here());
        }
        stray_breaks_.clear();
    }

    // -- statements -----------------------------------------------------------

    void emit_stmt(const ast::stmt_node &stmt)
    {
        switch (stmt.type)
        {
        case ast::stmt_type::Assignment:
            emit_assignment(ast::as_stmt<ast::assignment_stmt>(stmt), stmt.loc);
            break;
        case ast::stmt_type::Expr:
            emit_expr(*ast::as_stmt<ast::expr_stmt>(stmt).expression);
            code_.op(Opcode::POP);
            break;
        case ast::stmt_type::Return:
        {
            const auto &ret = ast::as_stmt<ast::return_stmt>(stmt);
            if (ret.value.has_value())
            {
                emit_expr(**ret.value);
                if (needs_return_clone(**ret.value))
                {
                    code_.op(Opcode::CLONE_OBJECT);
                }
            }
            else
            {
                code_.op(Opcode::PUSH_NULL);
            }
            code_.op(Opcode::RET);
            break;
        }
        case ast::stmt_type::Break:
            if (loop_breaks_.empty())
            {
                stray_breaks_.push_back(code_.jump(Opcode::JMP));
            }
            else
            {
                loop_breaks_.back().push_back(code_.jump(Opcode::JMP));
            }
            break;
        case ast::stmt_type::Block:
            emit_block(ast::as_stmt<ast::block_stmt>(stmt));
            break;
        case ast::stmt_type::If:
            emit_if(ast::as_stmt<ast::if_stmt>(stmt));
            break;
        case ast::stmt_type::Loop:
            emit_loop(ast::as_stmt<ast::loop_stmt>(stmt));
            break;
        case ast::stmt_type::Match:
            emit_match(ast::as_stmt<ast::match_stmt>(stmt));
            break;
        case ast::stmt_type::FuncDecl:
        {
            const auto &decl = ast::as_stmt<ast::func_decl>(stmt);
            functions_.push_back(compile_function(decl.name, decl.parameters, *decl.body,
                                                  decl.return_type.get()));
            break;
        }
        case ast::stmt_type::EnumDecl:
        {
            const auto &decl = ast::as_stmt<ast::enum_decl>(stmt);
            code_.op(Opcode::DEFINE_ENUM);
            code_.str(strings_.intern(decl.name));
            code_.u32(static_cast<uint32_t>(decl.members.size()));
            for (const auto &member : decl.members)
            {
                code_.str(strings_.intern(member));
            }
            break;
        }
        case ast::stmt_type::ObjectDecl:
        {
            const auto &decl = ast::as_stmt<ast::object_decl>(stmt);
            object_types_.insert(decl.name);
            code_.op(Opcode::DEFINE_OBJECT);
            code_.str(strings_.intern(decl.name));
            code_.u32(static_cast<uint32_t>(decl.fields.size()));
            for (const auto &field : decl.fields)
            {
                code_.str(strings_.intern(field.name));
            }
            break;
        }
        case ast::stmt_type::Monitor:
            emit_monitor(ast::as_stmt<ast::monitor_stmt>(stmt));
            break;
        case ast::stmt_type::Lock:
            code_.op(Opcode::LOCK_CREATE);
            code_.str(strings_.intern(ast::as_stmt<ast::lock_stmt>(stmt).lock_name));
            break;
        case ast::stmt_type::Acquire:
            code_.op(Opcode::LOCK_ACQUIRE);
            code_.str(strings_.intern(ast::as_stmt<ast::acquire_stmt>(stmt).lock_name));
            break;
        case ast::stmt_type::Release:
            code_.op(Opcode::LOCK_RELEASE);
            code_.str(strings_.intern(ast::as_stmt<ast::release_stmt>(stmt).lock_name));
            break;
        case ast::stmt_type::LoadPackage:
            // Imports are resolved statically by package_resolver; nothing to emit.
            break;
        case ast::stmt_type::Insert:
        {
            const auto &insert = ast::as_stmt<ast::insert_stmt>(stmt);
            code_.op(Opcode::LOAD);
            code_.str(strings_.intern("insert"));
            emit_expr(*insert.map_expr);
            emit_expr(*insert.entries);
            code_.op(Opcode::CALL);
            code_.u8(2);
            code_.op(Opcode::POP);
            break;
        }
        case ast::stmt_type::ReflectFor:
        case ast::stmt_type::TypeidMatch:
            codegen_error(stmt.loc, "internal: unexpanded compile-time reflexpr statement");
            break;
        }
    }

    void emit_assignment(const ast::assignment_stmt &assign, const ast::source_loc &loc)
    {
        if (assign.targets.size() > 255)
        {
            codegen_error(loc, "too many destructuring targets (max 255)");
        }
        const bool add_assign = assign.op == ast::assign_op::AddAssign;

        if (add_assign && assign.targets.size() == 1)
        {
            const ast::bind_target &target = assign.targets.front();
            if (target.is_discard)
            {
                emit_expr(*assign.value);
                code_.op(Opcode::POP);
                return;
            }
            const string_ref name = strings_.intern(target.name);
            code_.op(Opcode::LOAD);
            code_.str(name);
            emit_expr(*assign.value);
            code_.op(Opcode::ADD);
            code_.op(Opcode::STORE);
            code_.str(name);
            return;
        }

        emit_expr(*assign.value);
        if (assign.targets.size() == 1)
        {
            emit_bind(assign.targets.front());
            return;
        }
        // UNPACK leaves element 0 on top, so targets bind in source order.
        code_.op(Opcode::UNPACK);
        code_.u8(static_cast<uint8_t>(assign.targets.size()));
        for (const auto &target : assign.targets)
        {
            if (add_assign && !target.is_discard)
            {
                // target = target + element: LOAD then SWAP puts the old
                // value on the left of the ADD.
                const string_ref name = strings_.intern(target.name);
                code_.op(Opcode::LOAD);
                code_.str(name);
                code_.op(Opcode::SWAP);
                code_.op(Opcode::ADD);
                code_.op(Opcode::STORE);
                code_.str(name);
            }
            else
            {
                emit_bind(target);
            }
        }
    }

    /// Pop the stack top into @p target (or discard it for `_`).
    void emit_bind(const ast::bind_target &target)
    {
        if (target.is_discard)
        {
            code_.op(Opcode::POP);
            return;
        }
        code_.op(Opcode::STORE);
        code_.str(strings_.intern(target.name));
    }

    void emit_if(const ast::if_stmt &branch_chain)
    {
        std::vector<uint32_t> end_jumps;
        const auto emit_branch = [&](const ast::if_branch &branch)
        {
            emit_expr(*branch.condition);
            // JMP_IF_FALSE: taken ⇒ skip then-arm. `likely` ⇒ fallthrough preferred.
            if (branch.hint == ast::branch_hint::Likely)
            {
                code_.op(Opcode::HINT_BRANCH);
                code_.u8(0);
            }
            else if (branch.hint == ast::branch_hint::Unlikely)
            {
                code_.op(Opcode::HINT_BRANCH);
                code_.u8(1);
            }
            const uint32_t skip = code_.jump(Opcode::JMP_IF_FALSE);
            emit_block(*branch.body);
            end_jumps.push_back(code_.jump(Opcode::JMP));
            code_.patch(skip, code_.here());
        };
        emit_branch(*branch_chain.then_branch);
        for (const auto &branch : branch_chain.else_if_branches)
        {
            emit_branch(*branch);
        }
        if (branch_chain.else_branch.has_value())
        {
            emit_block(**branch_chain.else_branch);
        }
        for (const uint32_t jump : end_jumps)
        {
            code_.patch(jump, code_.here());
        }
    }

    void emit_loop(const ast::loop_stmt &loop)
    {
        const uint32_t start = code_.here();
        std::optional<uint32_t> exit_jump;
        if (loop.condition.has_value())
        {
            emit_expr(**loop.condition);
            exit_jump = code_.jump(Opcode::JMP_IF_FALSE);
        }
        loop_breaks_.emplace_back();
        emit_block(*loop.body);
        code_.op(Opcode::JMP);
        code_.u32(start);
        const uint32_t end = code_.here();
        if (exit_jump.has_value())
        {
            code_.patch(*exit_jump, end);
        }
        for (const uint32_t break_jump : loop_breaks_.back())
        {
            code_.patch(break_jump, end);
        }
        loop_breaks_.pop_back();
    }

    void emit_match(const ast::match_stmt &match)
    {
        emit_expr(*match.scrutinee);
        std::vector<uint32_t> end_jumps;
        for (const auto &arm : match.cases)
        {
            code_.op(Opcode::DUP);
            code_.op(Opcode::PUSH_ENUM);
            code_.str(strings_.intern(arm.enum_name));
            code_.str(strings_.intern(arm.member));
            code_.op(Opcode::EQ);
            const uint32_t next_case = code_.jump(Opcode::JMP_IF_FALSE);
            emit_block(*arm.body);
            end_jumps.push_back(code_.jump(Opcode::JMP));
            code_.patch(next_case, code_.here());
        }
        for (const uint32_t jump : end_jumps)
        {
            code_.patch(jump, code_.here());
        }
        code_.op(Opcode::POP); // drop the scrutinee
    }

    void emit_monitor(const ast::monitor_stmt &monitor)
    {
        const uint32_t handler_operand = code_.jump(Opcode::MONITOR_ENTER);
        emit_block(*monitor.protected_block);
        code_.op(Opcode::MONITOR_EXIT);
        const uint32_t skip_handler = code_.jump(Opcode::JMP);
        // Handler entry: the VM pushes the exception before jumping here.
        code_.patch(handler_operand, code_.here());
        code_.op(Opcode::STORE);
        code_.str(strings_.intern(monitor.trap_name));
        emit_block(*monitor.handler);
        code_.patch(skip_handler, code_.here());
    }

    // -- expressions -------------------------------------------------------------

    void emit_expr(const ast::expr_node &expr)
    {
        switch (expr.type)
        {
        case ast::expr_type::IntLiteral:
            code_.op(Opcode::PUSH_INT);
            code_.i64(static_cast<int64_t>(ast::as<ast::int_literal>(expr).value));
            break;
        case ast::expr_type::FloatLiteral:
            code_.op(Opcode::PUSH_FLOAT);
            code_.f64(static_cast<double>(ast::as<ast::float_literal>(expr).value));
            break;
        case ast::expr_type::StringLiteral:
            code_.op(Opcode::PUSH_STRING);
            code_.str(strings_.intern(ast::as<ast::string_literal>(expr).value));
            break;
        case ast::expr_type::CharLiteral:
            code_.op(Opcode::PUSH_CHAR);
            code_.u8(static_cast<uint8_t>(ast::as<ast::char_literal>(expr).value));
            break;
        case ast::expr_type::BoolLiteral:
            code_.op(Opcode::PUSH_BOOL);
            code_.u8(ast::as<ast::bool_literal>(expr).value ? 1 : 0);
            break;
        case ast::expr_type::NullLiteral:
            code_.op(Opcode::PUSH_NULL);
            break;
        case ast::expr_type::RegexLiteral:
            code_.op(Opcode::PUSH_REGEX);
            code_.str(strings_.intern(ast::as<ast::regex_literal>(expr).pattern));
            break;
        case ast::expr_type::Identifier:
        {
            const std::string &name = ast::as<ast::identifier>(expr).name;
            if (const auto bound = const_locals_.find(name); bound != const_locals_.end())
            {
                code_.op(Opcode::PUSH_INT);
                code_.i64(bound->second);
                break;
            }
            code_.op(Opcode::LOAD);
            code_.str(strings_.intern(name));
            break;
        }
        case ast::expr_type::Binary:
            emit_binary(ast::as<ast::binary_expr>(expr));
            break;
        case ast::expr_type::Unary:
        {
            const auto &unary = ast::as<ast::unary_expr>(expr);
            emit_expr(*unary.operand);
            switch (unary.op)
            {
            case ast::unary_op::Not:
                code_.op(Opcode::NOT);
                break;
            case ast::unary_op::BitwiseNot:
                code_.op(Opcode::BITWISE_NOT);
                break;
            case ast::unary_op::Neg:
                code_.op(Opcode::NEG);
                break;
            }
            break;
        }
        case ast::expr_type::Call:
        {
            const auto &call = ast::as<ast::call_expr>(expr);
            if (call.callee->type == ast::expr_type::Identifier &&
                call.arguments.size() == 1)
            {
                const std::string &cal =
                    ast::as<ast::identifier>(*call.callee).name;
                if (cal == "likely" || cal == "unlikely")
                {
                    // Identity at runtime; `if likely/unlikely` peels the hint.
                    emit_expr(*call.arguments[0]);
                    break;
                }
            }
            if (call.arguments.size() > 255)
            {
                codegen_error(expr.loc, "too many call arguments (max 255)");
            }
            emit_expr(*call.callee);
            for (const auto &argument : call.arguments)
            {
                emit_expr(*argument);
            }
            code_.op(Opcode::CALL);
            code_.u8(static_cast<uint8_t>(call.arguments.size()));
            break;
        }
        case ast::expr_type::Member:
        {
            const auto &member = ast::as<ast::member_expr>(expr);
            emit_expr(*member.object);
            code_.op(Opcode::MEMBER_GET);
            code_.str(strings_.intern(member.member));
            break;
        }
        case ast::expr_type::EnumAccess:
        {
            const auto &access = ast::as<ast::enum_access_expr>(expr);
            code_.op(Opcode::PUSH_ENUM);
            code_.str(strings_.intern(access.enum_name));
            code_.str(strings_.intern(access.member));
            break;
        }
        case ast::expr_type::Index:
        {
            const auto &index = ast::as<ast::index_expr>(expr);
            emit_expr(*index.object);
            emit_expr(*index.index);
            code_.op(Opcode::INDEX_GET);
            break;
        }
        case ast::expr_type::ArrayLiteral:
            emit_sequence(ast::as<ast::array_literal>(expr).elements, Opcode::MAKE_ARRAY);
            break;
        case ast::expr_type::TypedArrayLiteral:
            // The element type is a static annotation; the runtime shape is
            // identical to an untyped array literal.
            emit_sequence(ast::as<ast::typed_array_literal>(expr).elements, Opcode::MAKE_ARRAY);
            break;
        case ast::expr_type::TupleLiteral:
            emit_sequence(ast::as<ast::tuple_literal>(expr).elements, Opcode::MAKE_TUPLE);
            break;
        case ast::expr_type::PipeInsert:
        {
            const auto &insert = ast::as<ast::pipe_insert_expr>(expr);
            emit_expr(*insert.value);
            code_.op(Opcode::PIPE_INSERT);
            code_.str(strings_.intern(insert.pipe_name));
            break;
        }
        case ast::expr_type::PipeExtract:
            code_.op(Opcode::PIPE_EXTRACT);
            code_.str(strings_.intern(ast::as<ast::pipe_extract_expr>(expr).pipe_name));
            break;
        case ast::expr_type::ChannelInsert:
        {
            const auto &insert = ast::as<ast::channel_insert_expr>(expr);
            emit_expr(*insert.value);
            code_.op(Opcode::CHANNEL_INSERT);
            code_.str(strings_.intern(insert.channel_name));
            break;
        }
        case ast::expr_type::ChannelExtract:
            code_.op(Opcode::CHANNEL_EXTRACT);
            code_.str(strings_.intern(
                ast::as<ast::channel_extract_expr>(expr).channel_name));
            break;
        case ast::expr_type::Cast:
        {
            const auto &cast = ast::as<ast::cast_expr>(expr);
            emit_expr(*cast.operand);
            code_.op(Opcode::CAST);
            emit_type(*cast.target_type);
            break;
        }
        case ast::expr_type::Alloc:
        {
            const auto &alloc = ast::as<ast::alloc_expr>(expr);
            emit_expr(*alloc.capacity);
            for (const auto &value : alloc.initial_values)
            {
                emit_expr(*value);
            }
            code_.op(Opcode::ALLOC);
            code_.u32(static_cast<uint32_t>(alloc.initial_values.size()));
            break;
        }
        case ast::expr_type::Free:
            code_.op(Opcode::FREE);
            code_.str(strings_.intern(ast::as<ast::free_expr>(expr).buffer_name));
            break;
        case ast::expr_type::Simd:
        {
            const auto &simd = ast::as<ast::simd_expr>(expr);
            emit_expr(*simd.operand);
            code_.op(Opcode::MAKE_SIMD);
            break;
        }
        case ast::expr_type::Lambda:
        {
            const auto &lambda = ast::as<ast::lambda_expr>(expr);
            std::string name = "<lambda#" + std::to_string(lambda_counter_++) + "@" +
                               std::to_string(expr.loc.line) + ":" +
                               std::to_string(expr.loc.column) + ">";
            functions_.push_back(compile_function(name, lambda.parameters, *lambda.body,
                                                  lambda.return_type.get()));
            code_.op(Opcode::PUSH_FUNC);
            code_.str(strings_.intern(name));
            break;
        }
        case ast::expr_type::MapLiteral:
        {
            const auto &literal = ast::as<ast::map_literal>(expr);
            for (const ast::map_entry &entry : literal.entries)
            {
                emit_expr(*entry.key);
                emit_expr(*entry.value);
            }
            code_.op(Opcode::MAKE_MAP);
            code_.u32(static_cast<uint32_t>(literal.entries.size()));
            break;
        }
        case ast::expr_type::MapEntriesLiteral:
        {
            const auto &entries = ast::as<ast::map_entries_literal>(expr);
            for (const ast::map_entry &entry : entries.entries)
            {
                emit_expr(*entry.key);
                emit_expr(*entry.value);
            }
            code_.op(Opcode::MAKE_MAP);
            code_.u32(static_cast<uint32_t>(entries.entries.size()));
            break;
        }
        }
    }

    [[nodiscard]] bool is_object_return_type() const
    {
        if (current_return_type_ == nullptr ||
            current_return_type_->type != ast::type_kind::Named)
        {
            return false;
        }
        return object_types_.contains(
            std::get<ast::named_type>(current_return_type_->value).name);
    }

    [[nodiscard]] bool is_rvo_return_expr(const ast::expr_node &expr) const
    {
        if (expr.type != ast::expr_type::Call)
        {
            return false;
        }
        const auto &call = ast::as<ast::call_expr>(expr);
        if (call.callee->type != ast::expr_type::Identifier)
        {
            return false;
        }
        return object_types_.contains(ast::as<ast::identifier>(*call.callee).name);
    }

    [[nodiscard]] bool needs_return_clone(const ast::expr_node &expr) const
    {
        return is_object_return_type() && !is_rvo_return_expr(expr);
    }

    [[nodiscard]] static std::optional<int64_t> int_literal_value(const ast::expr_node &expr)
    {
        if (expr.type != ast::expr_type::IntLiteral)
        {
            return std::nullopt;
        }
        return static_cast<int64_t>(ast::as<ast::int_literal>(expr).value);
    }

    [[nodiscard]] static std::optional<std::string>
    assignment_target_name(const ast::assignment_stmt &assign)
    {
        if (assign.targets.size() != 1 || assign.targets.front().is_discard)
        {
            return std::nullopt;
        }
        return assign.targets.front().name;
    }

    [[nodiscard]] static bool references_identifier(const ast::expr_node &expr,
                                                    std::string_view name)
    {
        if (expr.type == ast::expr_type::Identifier)
        {
            return ast::as<ast::identifier>(expr).name == name;
        }
        return false;
    }

    [[nodiscard]] bool body_increments_counter(const ast::block_stmt &body,
                                               std::string_view counter) const
    {
        if (body.statements.empty())
        {
            return false;
        }
        const ast::stmt_node &last = *body.statements.back();
        if (last.type != ast::stmt_type::Assignment)
        {
            return false;
        }
        const auto &assign = ast::as_stmt<ast::assignment_stmt>(last);
        const auto target = assignment_target_name(assign);
        if (!target.has_value() || *target != counter)
        {
            return false;
        }
        if (assign.op == ast::assign_op::AddAssign)
        {
            const auto step = int_literal_value(*assign.value);
            return step.has_value() && *step == 1;
        }
        if (assign.op != ast::assign_op::Assign ||
            assign.value->type != ast::expr_type::Binary)
        {
            return false;
        }
        const auto &binary = ast::as<ast::binary_expr>(*assign.value);
        return binary.op == ast::binary_op::Add &&
               references_identifier(*binary.left, counter) &&
               int_literal_value(*binary.right) == std::optional<int64_t>{1};
    }

    [[nodiscard]] std::optional<std::pair<std::string, int64_t>>
    match_counted_loop(const ast::stmt_node &init_stmt, const ast::loop_stmt &loop) const
    {
        if (init_stmt.type != ast::stmt_type::Assignment || !loop.condition.has_value())
        {
            return std::nullopt;
        }
        const auto &init = ast::as_stmt<ast::assignment_stmt>(init_stmt);
        if (init.op != ast::assign_op::Assign)
        {
            return std::nullopt;
        }
        const auto counter = assignment_target_name(init);
        const auto start = int_literal_value(*init.value);
        if (!counter.has_value() || !start.has_value())
        {
            return std::nullopt;
        }
        const ast::expr_node &condition = **loop.condition;
        if (condition.type != ast::expr_type::Binary)
        {
            return std::nullopt;
        }
        const auto &compare = ast::as<ast::binary_expr>(condition);
        int64_t limit = 0;
        if (compare.op == ast::binary_op::Lt &&
            references_identifier(*compare.left, *counter))
        {
            const auto bound = int_literal_value(*compare.right);
            if (!bound.has_value() || *bound < *start)
            {
                return std::nullopt;
            }
            limit = *bound;
        }
        else if (compare.op == ast::binary_op::Le &&
                 references_identifier(*compare.left, *counter))
        {
            const auto bound = int_literal_value(*compare.right);
            if (!bound.has_value() || *bound < *start)
            {
                return std::nullopt;
            }
            limit = *bound + 1;
        }
        else
        {
            return std::nullopt;
        }
        if (!body_increments_counter(*loop.body, std::string_view{*counter}))
        {
            return std::nullopt;
        }
        const int64_t trips = limit - *start;
        if (trips <= 0 || trips > k_max_loop_unroll)
        {
            return std::nullopt;
        }
        return std::pair<std::string, int64_t>{*counter, trips};
    }

    bool try_emit_unrolled_loop(const ast::stmt_node &init_stmt, const ast::stmt_node &loop_stmt)
    {
        if (loop_stmt.type != ast::stmt_type::Loop)
        {
            return false;
        }
        const auto &loop = ast::as_stmt<ast::loop_stmt>(loop_stmt);
        const auto matched = match_counted_loop(init_stmt, loop);
        if (!matched.has_value())
        {
            return false;
        }
        const auto &[counter, trips] = *matched;
        const int64_t start = [&] {
            const auto &init = ast::as_stmt<ast::assignment_stmt>(init_stmt);
            return *int_literal_value(*init.value);
        }();
        const int64_t saved = const_locals_.contains(counter) ? const_locals_[counter] : 0;
        const bool had_binding = const_locals_.contains(counter);
        for (int64_t step = 0; step < trips; ++step)
        {
            const_locals_[counter] = start + step;
            emit_unrolled_body(*loop.body, counter);
        }
        if (had_binding)
        {
            const_locals_[counter] = saved;
        }
        else
        {
            const_locals_.erase(counter);
        }
        return true;
    }

    void emit_simd_elementwise_loop(const simd_loop_match &match)
    {
        code_.op(Opcode::LOAD);
        code_.str(strings_.intern(match.left_array));
        code_.op(Opcode::MAKE_SIMD);
        code_.op(Opcode::LOAD);
        code_.str(strings_.intern(match.right_array));
        code_.op(Opcode::MAKE_SIMD);
        switch (match.op)
        {
        case ast::binary_op::Add:
            code_.op(Opcode::ADD);
            break;
        case ast::binary_op::Sub:
            code_.op(Opcode::SUB);
            break;
        case ast::binary_op::Mul:
            code_.op(Opcode::MUL);
            break;
        default:
            break;
        }
        code_.op(Opcode::SIMD_TO_ARRAY);
        code_.op(Opcode::STORE);
        code_.str(strings_.intern(match.out_var));
    }

    void emit_unrolled_body(const ast::block_stmt &body, std::string_view counter)
    {
        const size_t statement_count = body.statements.size();
        const size_t emit_count =
            body_increments_counter(body, counter) && statement_count > 0
                ? statement_count - 1
                : statement_count;
        for (size_t index = 0; index < emit_count; ++index)
        {
            emit_stmt(*body.statements[index]);
        }
    }

    void emit_binary(const ast::binary_expr &binary)
    {
        // Short-circuit forms: the left value is the result when it decides.
        if (binary.op == ast::binary_op::And || binary.op == ast::binary_op::Or)
        {
            emit_expr(*binary.left);
            code_.op(Opcode::DUP);
            const Opcode jump_op = binary.op == ast::binary_op::And
                                       ? Opcode::JMP_IF_FALSE
                                       : Opcode::JMP_IF_TRUE;
            const uint32_t short_circuit = code_.jump(jump_op);
            code_.op(Opcode::POP);
            emit_expr(*binary.right);
            code_.patch(short_circuit, code_.here());
            return;
        }

        emit_expr(*binary.left);
        emit_expr(*binary.right);
        switch (binary.op)
        {
        case ast::binary_op::Add: code_.op(Opcode::ADD); break;
        case ast::binary_op::Sub: code_.op(Opcode::SUB); break;
        case ast::binary_op::Mul: code_.op(Opcode::MUL); break;
        case ast::binary_op::Div: code_.op(Opcode::DIV); break;
        case ast::binary_op::Mod: code_.op(Opcode::MOD); break;
        case ast::binary_op::Eq: code_.op(Opcode::EQ); break;
        case ast::binary_op::Ne: code_.op(Opcode::NE); break;
        case ast::binary_op::Lt: code_.op(Opcode::LT); break;
        case ast::binary_op::Gt: code_.op(Opcode::GT); break;
        case ast::binary_op::Le: code_.op(Opcode::LE); break;
        case ast::binary_op::Ge: code_.op(Opcode::GE); break;
        case ast::binary_op::BitwiseAnd: code_.op(Opcode::BITWISE_AND); break;
        case ast::binary_op::BitwiseOr: code_.op(Opcode::BITWISE_OR); break;
        case ast::binary_op::BitwiseXor: code_.op(Opcode::BITWISE_XOR); break;
        case ast::binary_op::And:
        case ast::binary_op::Or:
            break; // handled above
        }
    }

    /// Encode @p type as a CAST operand: u8 type_kind tag plus payload
    /// (see the encoding notes in Opcode.hpp).
    void emit_type(const ast::type_node &type)
    {
        code_.u8(static_cast<uint8_t>(type.type));
        switch (type.type)
        {
        case ast::type_kind::Primitive:
            code_.u8(static_cast<uint8_t>(std::get<ast::primitive_type>(type.value).kind));
            break;
        case ast::type_kind::Named:
            code_.str(strings_.intern(std::get<ast::named_type>(type.value).name));
            break;
        case ast::type_kind::Array:
            emit_type(*std::get<ast::array_type>(type.value).element);
            break;
        case ast::type_kind::Tuple:
        {
            const auto &tuple = std::get<ast::tuple_type>(type.value);
            code_.u32(static_cast<uint32_t>(tuple.elements.size()));
            for (const auto &element : tuple.elements)
            {
                emit_type(*element);
            }
            break;
        }
        case ast::type_kind::Map:
        {
            const auto &map = std::get<ast::map_type>(type.value);
            emit_type(*map.key);
            emit_type(*map.value);
            break;
        }
        case ast::type_kind::Lambda:
        {
            const auto &lambda = std::get<ast::lambda_type>(type.value);
            code_.u32(static_cast<uint32_t>(lambda.params.size()));
            for (const auto &param : lambda.params)
            {
                emit_type(*param);
            }
            emit_type(*lambda.ret);
            break;
        }
        }
    }

    /// Emit @p elements left-to-right then @p make_op with the element count.
    void emit_sequence(const std::vector<std::unique_ptr<ast::expr_node>> &elements,
                       Opcode make_op)
    {
        for (const auto &element : elements)
        {
            emit_expr(*element);
        }
        code_.op(make_op);
        code_.u32(static_cast<uint32_t>(elements.size()));
    }
};

// ---------------------------------------------------------------------------
// Driver: packages → .mxb image
// ---------------------------------------------------------------------------

/// Front-end driver that resolves packages, lowers every package to stack-VM
/// bytecode, and serializes the result as a .mxb image (see the layout
/// comment above @ref mx_function_descriptor).
class bytecode_compiler {
  const ast::program &main_;              ///< Root program.
  package_resolver resolver_;             ///< Import resolver for @ref main_.
  type_annotation_map type_map_;          ///< Expression types for optimizations.
  std::string_view mx_signature_{"MX"};   ///< Two-byte file signature.
  string_table strings_;                  ///< Module-wide string table.
  std::vector<compiled_package> packages_; ///< Imports first, entry point last.
public:
  /// Construct a compiler for @p prog.
  bytecode_compiler(const std::filesystem::path &main_dir_path, const ast::program &prog) : main_(prog), resolver_(main_dir_path, prog) {}

  /// Resolve imports, type-check, compile every package, and lay out the .mxb image.
  /// @throws compilation_error on unresolved imports, type errors, or codegen failures.
  std::vector<std::byte> compile() {
      resolver_.resolve();
      if (!resolver_.ok)
      {
          fail_compile("failed to resolve imports of package `" +
                       main_.package_name + "`");
      }
      type_map_ = type_checker::check_packages_annotated(
          resolver_.main_dir_path(), main_, resolver_.imports_package_programs);
      packages_.clear();
      packages_.reserve(resolver_.imports_package_programs.size() + 1);
      for (const auto &import_program : resolver_.imports_package_programs)
      {
          packages_.push_back(compile_package(import_program));
      }
      packages_.push_back(compile_package(main_));
      return layout();
  }

  /// Compile and write the image to @p output_path.
  /// @return Number of bytes written.
  size_t compile_to_file(const std::filesystem::path &output_path) {
      const std::vector<std::byte> image = compile();
      std::ofstream output{output_path, std::ios::binary};
      if (!output)
      {
          fail_compile("could not open output file: " + output_path.string());
      }
      output.write(reinterpret_cast<const char *>(image.data()),
                   static_cast<std::streamsize>(image.size()));
      if (!output)
      {
          fail_compile("failed to write output file: " + output_path.string());
      }
      return image.size();
  }

private:
  /// Lower one package: top-level statements become its init bytecode,
  /// function declarations and lambdas populate its function table.
  compiled_package compile_package(const ast::program &program) {
      compiled_package package;
      package.name = program.package_name;
      size_t lambda_counter = 0;
      std::unordered_set<std::string> object_types;
      code_emitter emitter{strings_, package.functions, lambda_counter, object_types,
                           &type_map_};
      emitter.clear_debug_map();
      package.init_code = emitter.emit_toplevel(program.statements);
      package.init_debug_map = emitter.take_debug_map();
      return package;
  }

  /// Copy @p value verbatim into @p out at byte offset @p at.
  template <typename T>
  static void write_struct(std::vector<std::byte> &out, size_t at, const T &value) {
      std::memcpy(out.data() + at, &value, sizeof value);
  }

  /// Copy a code blob into @p out at byte offset @p at.
  static void write_blob(std::vector<std::byte> &out, size_t at,
                         const std::vector<std::byte> &blob) {
      std::memcpy(out.data() + at, blob.data(), blob.size());
  }

  /// Serialize a debug map for inclusion in the `.mxb` image.
  static std::vector<std::byte> serialize_debug_map(const std::vector<debug_loc_entry> &map,
                                                    string_table &strings)
  {
      std::vector<std::byte> out;
      const uint32_t count = static_cast<uint32_t>(map.size());
      const auto append_u32 = [&out](uint32_t word) {
          const std::byte *bytes = reinterpret_cast<const std::byte *>(&word);
          out.insert(out.end(), bytes, bytes + sizeof word);
      };
      append_u32(count);
      for (const debug_loc_entry &entry : map)
      {
          append_u32(entry.pc);
          append_u32(entry.line);
          append_u32(entry.column);
          const string_ref file = strings.intern(entry.file);
          append_u32(file.offset);
          append_u32(file.length);
      }
      return out;
  }

  /// Assign file offsets to every section, then serialize header,
  /// descriptors, code blobs, and the string table into one buffer.
  std::vector<std::byte> layout() {
      struct package_layout {
          string_ref name;
          std::vector<string_ref> function_names;
          size_t function_array_offset{0};
          std::vector<size_t> function_code_offsets;
          std::vector<size_t> function_debug_offsets;
          std::vector<size_t> function_debug_lengths;
          size_t init_offset{0};
          size_t init_debug_offset{0};
          size_t init_debug_length{0};
      };

      // Intern all names first so the string table stops growing before its
      // file offset is computed.
      std::vector<package_layout> layouts(packages_.size());
      for (size_t i = 0; i < packages_.size(); ++i)
      {
          layouts[i].name = strings_.intern(packages_[i].name);
          for (const auto &function : packages_[i].functions)
          {
              layouts[i].function_names.push_back(strings_.intern(function.name));
          }
      }

      const size_t import_count = packages_.size() - 1;
      const size_t import_array_offset = sizeof(mx_program_header);
      size_t cursor = import_array_offset + import_count * sizeof(mx_package_descriptor);
      std::vector<std::vector<std::byte>> debug_blobs;
      debug_blobs.reserve(packages_.size() * 8);
      for (size_t i = 0; i < packages_.size(); ++i)
      {
          layouts[i].function_array_offset = cursor;
          cursor += packages_[i].functions.size() * sizeof(mx_function_descriptor);
          for (const auto &function : packages_[i].functions)
          {
              layouts[i].function_code_offsets.push_back(cursor);
              cursor += function.code.size();
              layouts[i].function_debug_offsets.push_back(cursor);
              std::vector<std::byte> debug_blob =
                  serialize_debug_map(function.debug_map, strings_);
              layouts[i].function_debug_lengths.push_back(debug_blob.size());
              debug_blobs.push_back(std::move(debug_blob));
              cursor += layouts[i].function_debug_lengths.back();
          }
          layouts[i].init_offset = cursor;
          cursor += packages_[i].init_code.size();
          layouts[i].init_debug_offset = cursor;
          std::vector<std::byte> init_debug =
              serialize_debug_map(packages_[i].init_debug_map, strings_);
          layouts[i].init_debug_length = init_debug.size();
          debug_blobs.push_back(std::move(init_debug));
          cursor += layouts[i].init_debug_length;
      }
      const size_t string_table_offset = cursor;
      const auto &table = strings_.bytes();

      std::vector<std::byte> out(cursor + table.size(), std::byte{0});

      const auto make_descriptor = [&](size_t i) {
          mx_package_descriptor descriptor{};
          descriptor.package_name_offset = layouts[i].name.offset;
          descriptor.package_name_length = layouts[i].name.length;
          descriptor.package_bytecode_offset = layouts[i].init_offset;
          descriptor.package_bytecode_length = packages_[i].init_code.size();
          descriptor.function_descriptor_array_offset = layouts[i].function_array_offset;
          descriptor.num_function_descriptors = packages_[i].functions.size();
          descriptor.init_debug_map_offset = layouts[i].init_debug_offset;
          descriptor.init_debug_map_length = layouts[i].init_debug_length;
          return descriptor;
      };

      const size_t main_index = packages_.size() - 1;
      mx_program_header header{};
      header.mx_signature[0] = static_cast<std::byte>(mx_signature_[0]);
      header.mx_signature[1] = static_cast<std::byte>(mx_signature_[1]);
      header.mx_bytecode_version = current_mx_bytecode_version;
      header.package_name_offset = layouts[main_index].name.offset;
      header.package_name_length = layouts[main_index].name.length;
      header.package_import_array_offset = import_array_offset;
      header.num_package_import_list = import_count;
      header.entry_point_bytecode_offset = layouts[main_index].init_offset;
      header.entry_point_bytecode_length = packages_[main_index].init_code.size();
      header.string_table_offset = string_table_offset;
      header.string_table_length = table.size();
      header.entry_point_package_descriptor = make_descriptor(main_index);
      write_struct(out, 0, header);

      for (size_t i = 0; i < import_count; ++i)
      {
          write_struct(out, import_array_offset + i * sizeof(mx_package_descriptor),
                       make_descriptor(i));
      }

      size_t global_debug_blob = 0;
      for (size_t i = 0; i < packages_.size(); ++i)
      {
          for (size_t j = 0; j < packages_[i].functions.size(); ++j)
          {
              mx_function_descriptor descriptor{};
              descriptor.function_name_offset = layouts[i].function_names[j].offset;
              descriptor.function_name_length = layouts[i].function_names[j].length;
              descriptor.function_content_offset = layouts[i].function_code_offsets[j];
              descriptor.function_content_length = packages_[i].functions[j].code.size();
              descriptor.debug_map_offset = layouts[i].function_debug_offsets[j];
              descriptor.debug_map_length = layouts[i].function_debug_lengths[j];
              write_struct(out,
                           layouts[i].function_array_offset + j * sizeof(mx_function_descriptor),
                           descriptor);
              write_blob(out, layouts[i].function_code_offsets[j],
                         packages_[i].functions[j].code);
              write_blob(out, layouts[i].function_debug_offsets[j],
                         debug_blobs[global_debug_blob++]);
          }
          write_blob(out, layouts[i].init_offset, packages_[i].init_code);
          write_blob(out, layouts[i].init_debug_offset, debug_blobs[global_debug_blob++]);
      }

      if (!table.empty())
      {
          std::memcpy(out.data() + string_table_offset, table.data(), table.size());
      }
      return out;
  }
};

}
