#pragma once

#include "ast.hpp"
#include "errors.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace munx
{

/// Canonical resolved type used during semantic analysis.
struct resolved_type
{
    enum class kind
    {
        Unknown,
        Void,
        Null,
        Primitive,
        Named,
        Array,
        Tuple,
        Simd,
        EnumMember,
        Buffer,
        Map,
        Lambda,
    };

    kind tag{kind::Unknown};
    ast::primitive_kind prim{ast::primitive_kind::Int};
    std::string name; ///< Named / enum / object / enum-member owner.
    std::vector<resolved_type /* this will not work, it should be a shared or unique pointer */> elements; ///< Array element, tuple fields, map key/value, or lambda params + return (last).

    [[nodiscard]] static resolved_type unknown() noexcept { return {}; }

    [[nodiscard]] static resolved_type make_primitive(ast::primitive_kind p) noexcept
    {
        resolved_type out{};
        out.tag = kind::Primitive;
        out.prim = p;
        return out;
    }

    [[nodiscard]] static resolved_type named(const std::string& type_name)
    {
        resolved_type out{};
        out.tag = kind::Named;
        out.name = std::move(type_name);
        return out;
    }

    [[nodiscard]] static resolved_type array(const resolved_type& element)
    {
        resolved_type out{};
        out.tag = kind::Array;
        out.elements.push_back(std::move(element));
        return out;
    }

    [[nodiscard]] static resolved_type tuple(const std::vector<resolved_type>& fields)
    {
        resolved_type out{};
        out.tag = kind::Tuple;
        out.elements = std::move(fields);
        return out;
    }

    [[nodiscard]] static resolved_type enum_member(const std::string& enum_name)
    {
        resolved_type out{};
        out.tag = kind::EnumMember;
        out.name = std::move(enum_name);
        return out;
    }

    [[nodiscard]] static resolved_type buffer() noexcept
    {
        resolved_type out{};
        out.tag = kind::Buffer;
        return out;
    }

    [[nodiscard]] static resolved_type simd(const resolved_type& lane)
    {
        resolved_type out{};
        out.tag = kind::Simd;
        out.elements.push_back(std::move(lane));
        return out;
    }

    [[nodiscard]] static resolved_type map_of(const resolved_type& key, const resolved_type& value)
    {
        resolved_type out{};
        out.tag = kind::Map;
        out.elements.push_back(std::move(key));
        out.elements.push_back(std::move(value));
        return out;
    }

    [[nodiscard]] static resolved_type lambda_sig(std::vector<resolved_type> params,
                                                  resolved_type ret)
    {
        resolved_type out{};
        out.tag = kind::Lambda;
        out.elements = std::move(params);
        out.elements.push_back(std::move(ret));
        return out;
    }

    [[nodiscard]] const resolved_type *lambda_return_type() const noexcept
    {
        if (tag != kind::Lambda || elements.empty())
        {
            return nullptr;
        }
        return &elements.back();
    }

    [[nodiscard]] std::span<const resolved_type> lambda_param_types() const noexcept
    {
        if (tag != kind::Lambda || elements.empty())
        {
            return {};
        }
        return {elements.data(), elements.size() - 1};
    }

    [[nodiscard]] bool is_simd() const noexcept { return tag == kind::Simd; }

    [[nodiscard]] bool is_simd_lane_primitive() const noexcept
    {
        return tag == kind::Primitive &&
               (prim == ast::primitive_kind::Int || prim == ast::primitive_kind::Float ||
                prim == ast::primitive_kind::Character || prim == ast::primitive_kind::Bool);
    }

    [[nodiscard]] bool is_unknown() const noexcept { return tag == kind::Unknown; }

    [[nodiscard]] bool is_copyable() const noexcept
    {
        switch (tag)
        {
        case kind::Primitive:
            return prim != ast::primitive_kind::String;
        case kind::Null:
        case kind::Void:
        case kind::Unknown:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] bool is_reference() const noexcept { return !is_copyable() && tag != kind::Void; }

    [[nodiscard]] bool operator==(const resolved_type &other) const noexcept
    {
        if (tag != other.tag)
        {
            return false;
        }
        switch (tag)
        {
        case kind::Unknown:
        case kind::Void:
        case kind::Null:
        case kind::Buffer:
            return true;
        case kind::Primitive:
            return prim == other.prim;
        case kind::Named:
        case kind::EnumMember:
            return name == other.name;
        case kind::Array:
        case kind::Tuple:
        case kind::Simd:
        case kind::Map:
            return elements == other.elements;
        case kind::Lambda:
            return elements == other.elements;
        }
        return false;
    }

    [[nodiscard]] std::string stringify() const
    {
        switch (tag)
        {
        case kind::Unknown:
            return "<unknown>";
        case kind::Void:
            return "void";
        case kind::Null:
            return "null";
        case kind::Buffer:
            return "buffer";
        case kind::Primitive:
            return std::string{primitive_name(prim)};
        case kind::Named:
        case kind::EnumMember:
            return name;
        case kind::Array:
            return "[" + elements.front().stringify() + "]";
        case kind::Simd:
            return "simd<" + elements.front().stringify() + ">";
        case kind::Tuple:
        {
            std::string text = "tuple[";
            for (size_t i = 0; i < elements.size(); ++i)
            {
                if (i != 0)
                {
                    text += ", ";
                }
                text += elements[i].stringify();
            }
            text += ']';
            return text;
        }
        case kind::Map:
            return "map[" + elements[0].stringify() + " => " + elements[1].stringify() +
                   ']';
        case kind::Lambda:
        {
            if (elements.empty())
            {
                return "Lambda[{ } => <unknown>]";
            }
            std::string text = "Lambda[{";
            for (size_t i = 0; i + 1 < elements.size(); ++i)
            {
                if (i != 0)
                {
                    text += ", ";
                }
                text += elements[i].stringify();
            }
            text += "} => ";
            text += elements.back().stringify();
            text += ']';
            return text;
        }
        }
        return "<invalid>";
    }

    [[nodiscard]] static std::string_view primitive_name(ast::primitive_kind p) noexcept
    {
        switch (p)
        {
        case ast::primitive_kind::Int:
            return "int";
        case ast::primitive_kind::Float:
            return "float";
        case ast::primitive_kind::Bool:
            return "bool";
        case ast::primitive_kind::String:
            return "string";
        case ast::primitive_kind::Character:
            return "character";
        case ast::primitive_kind::Void:
            return "void";
        case ast::primitive_kind::Socket:
            return "socket";
        case ast::primitive_kind::File:
            return "file";
        case ast::primitive_kind::Term:
            return "term";
        case ast::primitive_kind::Exception:
            return "exception";
        }
        return "<?>";
    }
};

/// Expression types recorded during semantic analysis (for codegen optimizations).
struct type_annotation_map
{
    std::unordered_map<const ast::expr_node *, resolved_type> exprs;

    [[nodiscard]] resolved_type lookup(const ast::expr_node &expr) const
    {
        const auto found = exprs.find(&expr);
        if (found != exprs.end())
        {
            return found->second;
        }
        return resolved_type::unknown();
    }
};

/// Semantic analysis pass: validates types, signatures, and control-flow rules.
class type_checker
{
public:
    /// Check @p program and every imported package in dependency order.
    /// @throws compilation_error on the first type error.
    static void check_packages(const std::filesystem::path &main_dir_path,
                               const ast::program &main,
                               const std::vector<ast::program> &imports)
    {
        (void)check_packages_annotated(main_dir_path, main, imports);
    }

    /// Type-check all packages and return expression type annotations.
    /// @throws compilation_error on the first type error.
    static type_annotation_map check_packages_annotated(
        const std::filesystem::path &main_dir_path, const ast::program &main,
        const std::vector<ast::program> &imports)
    {
        type_checker checker{main_dir_path, true};
        for (const ast::program &imported : imports)
        {
            checker.check_program_body(imported);
        }
        checker.check_program_body(main);
        return std::move(checker.expr_types_);
    }

    /// Check a single translation unit.
    /// @throws compilation_error on the first type error.
    static void check_single_program(const ast::program &program)
    {
        type_checker checker{};
        checker.check_program_body(program);
    }

private:
    struct object_field
    {
        std::string name;
        resolved_type type;
    };

    struct function_sig
    {
        std::vector<resolved_type> params;
        resolved_type ret;
        ast::source_loc loc{};
        bool variadic{false};
    };

    struct enum_info
    {
        std::vector<std::string> members;
        ast::source_loc loc{};
    };

    struct scope_frame
    {
        std::unordered_map<std::string, resolved_type> vars;
        std::unordered_map<std::string, function_sig> funcs;
        std::unordered_map<std::string, enum_info> enums;
        std::unordered_map<std::string, std::vector<object_field>> objects;
        scope_frame *parent{nullptr};

        [[nodiscard]] resolved_type lookup_var(std::string_view name) const
        {
            for (const scope_frame *frame = this; frame != nullptr; frame = frame->parent)
            {
                const auto found = frame->vars.find(std::string{name});
                if (found != frame->vars.end())
                {
                    return found->second;
                }
            }
            return resolved_type::unknown();
        }

        [[nodiscard]] const function_sig *lookup_func(std::string_view name) const
        {
            for (const scope_frame *frame = this; frame != nullptr; frame = frame->parent)
            {
                const auto found = frame->funcs.find(std::string{name});
                if (found != frame->funcs.end())
                {
                    return &found->second;
                }
            }
            return nullptr;
        }

        [[nodiscard]] const enum_info *lookup_enum(std::string_view name) const
        {
            for (const scope_frame *frame = this; frame != nullptr; frame = frame->parent)
            {
                const auto found = frame->enums.find(std::string{name});
                if (found != frame->enums.end())
                {
                    return &found->second;
                }
            }
            return nullptr;
        }

        [[nodiscard]] const std::vector<object_field> *lookup_object(std::string_view name) const
        {
            for (const scope_frame *frame = this; frame != nullptr; frame = frame->parent)
            {
                const auto found = frame->objects.find(std::string{name});
                if (found != frame->objects.end())
                {
                    return &found->second;
                }
            }
            return nullptr;
        }
    };

    std::filesystem::path main_dir_path_;
    scope_frame package_scope_{};
    scope_frame *current_scope_{&package_scope_};
    const function_sig *current_function_{nullptr};
    bool saw_return_{false};
    bool annotate_{false};
    type_annotation_map expr_types_{};

    explicit type_checker(std::filesystem::path main_dir_path = {}, bool annotate = false)
        : main_dir_path_(std::move(main_dir_path)), annotate_(annotate)
    {
        register_prelude();
    }

    void register_prelude()
    {
        auto any = [] { return resolved_type::unknown(); };
        auto void_ret = [] {
            resolved_type out{};
            out.tag = resolved_type::kind::Void;
            return out;
        };
        auto int_ret = [] { return resolved_type::make_primitive(ast::primitive_kind::Int); };
        auto str_ret = [] { return resolved_type::make_primitive(ast::primitive_kind::String); };
        auto bool_ret = [] { return resolved_type::make_primitive(ast::primitive_kind::Bool); };
        auto arr_ret = [] { return resolved_type::array(resolved_type::unknown()); };

        auto declare = [&](std::string_view name, std::vector<resolved_type> params,
                           resolved_type ret, bool variadic = false) {
            function_sig sig{};
            sig.params = std::move(params);
            sig.ret = std::move(ret);
            sig.variadic = variadic;
            package_scope_.funcs.emplace(std::string{name}, std::move(sig));
        };

        declare("print", {}, void_ret(), true);
        declare("println", {}, void_ret(), true);
        declare("readln", {}, str_ret(), true);
        declare("fail", {any()}, void_ret());
        declare("fix", {any()}, any());
        declare("concat", {}, str_ret(), true);
        declare("trim", {str_ret()}, str_ret());
        declare("split", {any(), any()}, arr_ret(), true);
        declare("has_substring_regex", {str_ret(), any()}, bool_ret());
        declare("len", {any()}, int_ret());
        declare("queue", {}, arr_ret(), true);
        declare("append", {arr_ret(), any()}, arr_ret());
        declare("push", {arr_ret(), any()}, arr_ret());
        declare("pop", {arr_ret()}, any());
        declare("remove_at", {arr_ret(), int_ret()}, arr_ret());
        declare("open", {}, any(), true);
        declare("read", {any(), any()}, any(), true);
        declare("write", {any(), any(), any()}, int_ret(), true);
        declare("close", {any()}, void_ret());
        declare("bind", {any(), any(), any()}, void_ret(), true);
        declare("listen", {any(), any()}, void_ret(), true);
        declare("accept", {any()}, resolved_type::tuple({any(), any()}));
        declare("thread", {any(), any()}, any(), true);
        declare("join", {any()}, void_ret(), true);
        declare("pipe", {str_ret(), any()}, any(), true);
        declare("sleep", {int_ret()}, void_ret());
        declare("get", {any(), any()}, any());
        declare("insert", {any(), any()}, void_ret());
        declare("env", {str_ret()}, any());
    }

    static void fail(const ast::source_loc &loc, const std::string &message)
    {
        fail_compile(loc.file + ':' + std::to_string(loc.line) + ':' +
                                std::to_string(loc.column) + ": error: " + message);
    }

    static void warn_at(const ast::source_loc &loc, const std::string &message)
    {
        std::cerr << loc.file << ':' << loc.line << ':' << loc.column << ": ";
        std::cerr << log_level::warn << "warning: " << message << reset{} << '\n';
    }

    [[nodiscard]] static std::optional<int64_t>
    try_int_literal(const ast::expr_node &expr) noexcept
    {
        if (expr.type != ast::expr_type::IntLiteral)
        {
            return std::nullopt;
        }
        return static_cast<int64_t>(ast::as<ast::int_literal>(expr).value);
    }

    static void check_constant_int_overflow(const ast::binary_expr &expr,
                                            const ast::source_loc &loc)
    {
        const std::optional<int64_t> left = try_int_literal(*expr.left);
        const std::optional<int64_t> right = try_int_literal(*expr.right);
        if (!left.has_value() || !right.has_value())
        {
            return;
        }
        int64_t result = 0;
        bool overflow = false;
        switch (expr.op)
        {
        case ast::binary_op::Add:
#if defined(__GNUC__) || defined(__clang__)
            overflow = __builtin_add_overflow(*left, *right, &result);
#else
            overflow = (*right > 0 &&
                        *left > std::numeric_limits<int64_t>::max() - *right) ||
                       (*right < 0 &&
                        *left < std::numeric_limits<int64_t>::min() - *right);
#endif
            break;
        case ast::binary_op::Sub:
#if defined(__GNUC__) || defined(__clang__)
            overflow = __builtin_sub_overflow(*left, *right, &result);
#else
            overflow = (*right > 0 &&
                        *left < std::numeric_limits<int64_t>::min() + *right) ||
                       (*right < 0 &&
                        *left > std::numeric_limits<int64_t>::max() + *right);
#endif
            break;
        case ast::binary_op::Mul:
#if defined(__GNUC__) || defined(__clang__)
            overflow = __builtin_mul_overflow(*left, *right, &result);
#else
            overflow = *left != 0 && *right != 0 &&
                         (*left > std::numeric_limits<int64_t>::max() / *right ||
                          *left < std::numeric_limits<int64_t>::min() / *right);
#endif
            break;
        default:
            break;
        }
        if (overflow)
        {
            warn_at(loc, "integer overflow in constant expression");
        }
    }

    static bool compatible(const resolved_type &expected, const resolved_type &actual)
    {
        if (expected.is_unknown() || actual.is_unknown())
        {
            return true;
        }
        if (actual.tag == resolved_type::kind::Null)
        {
            return true;
        }
        if (expected.tag == resolved_type::kind::EnumMember &&
            actual.tag == resolved_type::kind::EnumMember)
        {
            return expected.name == actual.name;
        }
        if (expected.tag == resolved_type::kind::EnumMember &&
            actual.tag == resolved_type::kind::Named)
        {
            return expected.name == actual.name;
        }
        if (expected.tag == resolved_type::kind::Named &&
            actual.tag == resolved_type::kind::EnumMember)
        {
            return expected.name == actual.name;
        }
        if (expected.tag == resolved_type::kind::Array &&
            actual.tag == resolved_type::kind::Array)
        {
            if (expected.elements.empty() || actual.elements.empty())
            {
                return true;
            }
            if (expected.elements.front().is_unknown() || actual.elements.front().is_unknown())
            {
                return true;
            }
            return compatible(expected.elements.front(), actual.elements.front());
        }
        if (expected.tag == resolved_type::kind::Tuple &&
            actual.tag == resolved_type::kind::Tuple)
        {
            if (expected.elements.size() != actual.elements.size())
            {
                return false;
            }
            for (size_t i = 0; i < expected.elements.size(); ++i)
            {
                if (!compatible(expected.elements[i], actual.elements[i]))
                {
                    return false;
                }
            }
            return true;
        }
        if (expected.tag == resolved_type::kind::Map &&
            actual.tag == resolved_type::kind::Map)
        {
            if (expected.elements.size() != 2 || actual.elements.size() != 2)
            {
                return true;
            }
            return compatible(expected.elements[0], actual.elements[0]) &&
                   compatible(expected.elements[1], actual.elements[1]);
        }
        if (expected.tag == resolved_type::kind::Lambda &&
            actual.tag == resolved_type::kind::Lambda)
        {
            return expected.elements == actual.elements;
        }
        if (expected.tag == resolved_type::kind::Primitive &&
            actual.tag == resolved_type::kind::Primitive)
        {
            if (expected.prim == actual.prim)
            {
                return true;
            }
            if (expected.prim == ast::primitive_kind::Float &&
                actual.prim == ast::primitive_kind::Int)
            {
                return true;
            }
        }
        return expected == actual;
    }

    static void require_compatible(const ast::source_loc &loc, const resolved_type &expected,
                                 const resolved_type &actual, std::string_view context)
    {
        if (!compatible(expected, actual))
        {
            fail(loc, std::string{context} + ": expected `" + expected.stringify() +
                         "`, got `" + actual.stringify() + '`');
        }
    }

    [[nodiscard]] resolved_type resolve_type_node(const ast::type_node &type) const
    {
        switch (type.type)
        {
        case ast::type_kind::Primitive:
        {
            const auto kind = std::get<ast::primitive_type>(type.value).kind;
            if (kind == ast::primitive_kind::Void)
            {
                resolved_type out{};
                out.tag = resolved_type::kind::Void;
                return out;
            }
            return resolved_type::make_primitive(kind);
        }
        case ast::type_kind::Named:
            return resolved_type::named(std::get<ast::named_type>(type.value).name);
        case ast::type_kind::Array:
            return resolved_type::array(
                resolve_type_node(*std::get<ast::array_type>(type.value).element));
        case ast::type_kind::Tuple:
        {
            std::vector<resolved_type> fields;
            for (const auto &element : std::get<ast::tuple_type>(type.value).elements)
            {
                fields.push_back(resolve_type_node(*element));
            }
            return resolved_type::tuple(std::move(fields));
        }
        case ast::type_kind::Map:
        {
            const auto &map = std::get<ast::map_type>(type.value);
            return resolved_type::map_of(resolve_type_node(*map.key),
                                         resolve_type_node(*map.value));
        }
        case ast::type_kind::Lambda:
        {
            const auto &lambda = std::get<ast::lambda_type>(type.value);
            std::vector<resolved_type> params;
            for (const auto &param : lambda.params)
            {
                params.push_back(resolve_type_node(*param));
            }
            return resolved_type::lambda_sig(std::move(params),
                                             resolve_type_node(*lambda.ret));
        }
        }
        return resolved_type::unknown();
    }

    void check_program_body(const ast::program &program)
    {
        scope_frame package{};
        package.parent = nullptr;
        package_scope_ = std::move(package);
        current_scope_ = &package_scope_;
        register_prelude();

        for (const auto &stmt : program.statements)
        {
            collect_declarations(*stmt);
        }

        for (const auto &stmt : program.statements)
        {
            check_stmt(*stmt);
        }
    }

    void collect_declarations(const ast::stmt_node &stmt)
    {
        switch (stmt.type)
        {
        case ast::stmt_type::FuncDecl:
        {
            const auto &decl = ast::as_stmt<ast::func_decl>(stmt);
            function_sig sig{};
            sig.loc = stmt.loc;
            sig.ret = resolve_type_node(*decl.return_type);
            sig.params.reserve(decl.parameters.size());
            for (const ast::parameter &param : decl.parameters)
            {
                sig.params.push_back(resolve_type_node(*param.type));
            }
            current_scope_->funcs.emplace(decl.name, std::move(sig));
            break;
        }
        case ast::stmt_type::EnumDecl:
        {
            const auto &decl = ast::as_stmt<ast::enum_decl>(stmt);
            enum_info info{};
            info.loc = stmt.loc;
            info.members = decl.members;
            current_scope_->enums.emplace(decl.name, std::move(info));
            break;
        }
        case ast::stmt_type::ObjectDecl:
        {
            const auto &decl = ast::as_stmt<ast::object_decl>(stmt);
            std::vector<object_field> fields;
            fields.reserve(decl.fields.size());
            for (const ast::object_field &field : decl.fields)
            {
                fields.push_back(object_field{field.name, resolve_type_node(*field.type)});
            }
            current_scope_->objects.emplace(decl.name, std::move(fields));
            break;
        }
        case ast::stmt_type::Block:
            for (const auto &inner : ast::as_stmt<ast::block_stmt>(stmt).statements)
            {
                collect_declarations(*inner);
            }
            break;
        case ast::stmt_type::If:
        {
            const auto &if_stmt = ast::as_stmt<ast::if_stmt>(stmt);
            for (const auto &inner : if_stmt.then_branch->body->statements)
            {
                collect_declarations(*inner);
            }
            for (const auto &branch : if_stmt.else_if_branches)
            {
                for (const auto &inner : branch->body->statements)
                {
                    collect_declarations(*inner);
                }
            }
            if (if_stmt.else_branch)
            {
                for (const auto &inner : (*if_stmt.else_branch)->statements)
                {
                    collect_declarations(*inner);
                }
            }
            break;
        }
        case ast::stmt_type::Loop:
            for (const auto &inner : ast::as_stmt<ast::loop_stmt>(stmt).body->statements)
            {
                collect_declarations(*inner);
            }
            break;
        case ast::stmt_type::Monitor:
        {
            const auto &mon = ast::as_stmt<ast::monitor_stmt>(stmt);
            for (const auto &inner : mon.protected_block->statements)
            {
                collect_declarations(*inner);
            }
            for (const auto &inner : mon.handler->statements)
            {
                collect_declarations(*inner);
            }
            break;
        }
        default:
            break;
        }
    }

    void check_stmt(const ast::stmt_node &stmt)
    {
        switch (stmt.type)
        {
        case ast::stmt_type::Assignment:
            check_assignment(ast::as_stmt<ast::assignment_stmt>(stmt), stmt.loc);
            break;
        case ast::stmt_type::Expr:
            (void)check_expr(*ast::as_stmt<ast::expr_stmt>(stmt).expression);
            break;
        case ast::stmt_type::Return:
            check_return(ast::as_stmt<ast::return_stmt>(stmt), stmt.loc);
            break;
        case ast::stmt_type::Break:
            break;
        case ast::stmt_type::Block:
            check_block(ast::as_stmt<ast::block_stmt>(stmt).statements);
            break;
        case ast::stmt_type::If:
            check_if(ast::as_stmt<ast::if_stmt>(stmt));
            break;
        case ast::stmt_type::Loop:
            check_loop(ast::as_stmt<ast::loop_stmt>(stmt));
            break;
        case ast::stmt_type::Match:
            check_match(ast::as_stmt<ast::match_stmt>(stmt), stmt.loc);
            break;
        case ast::stmt_type::FuncDecl:
            check_function(ast::as_stmt<ast::func_decl>(stmt));
            break;
        case ast::stmt_type::EnumDecl:
        case ast::stmt_type::ObjectDecl:
        case ast::stmt_type::Lock:
        case ast::stmt_type::Acquire:
        case ast::stmt_type::Release:
        case ast::stmt_type::LoadPackage:
            break;
        case ast::stmt_type::Monitor:
            check_monitor(ast::as_stmt<ast::monitor_stmt>(stmt));
            break;
        case ast::stmt_type::Insert:
            check_insert(ast::as_stmt<ast::insert_stmt>(stmt), stmt.loc);
            break;
        }
    }

    void check_insert(const ast::insert_stmt &stmt, const ast::source_loc &loc)
    {
        const resolved_type map_type = check_expr(*stmt.map_expr);
        if (map_type.tag != resolved_type::kind::Map && !map_type.is_unknown())
        {
            fail(loc, "insert expects a map, got `" + map_type.stringify() + '`');
        }
        if (stmt.entries->type != ast::expr_type::MapLiteral &&
            stmt.entries->type != ast::expr_type::MapEntriesLiteral)
        {
            fail(loc, "insert second argument must be a map literal");
        }
        resolved_type literal_type = resolved_type::unknown();
        if (stmt.entries->type == ast::expr_type::MapLiteral)
        {
            literal_type = check_map_literal(ast::as<ast::map_literal>(*stmt.entries),
                                             stmt.entries->loc);
        }
        else
        {
            literal_type =
                check_map_entries_literal(ast::as<ast::map_entries_literal>(*stmt.entries),
                                          map_type, stmt.entries->loc);
        }
        require_compatible(loc, map_type, literal_type, "insert map type");
        if (stmt.map_expr->type == ast::expr_type::Identifier &&
            stmt.receiver != ast::as<ast::identifier>(*stmt.map_expr).name)
        {
            fail(loc, "insert receiver must match the map argument");
        }
    }

    [[nodiscard]] resolved_type check_map_literal(const ast::map_literal &literal,
                                                  const ast::source_loc &loc)
    {
        const resolved_type key_type = resolve_type_node(*literal.key_type);
        const resolved_type value_type = resolve_type_node(*literal.value_type);
        for (const ast::map_entry &entry : literal.entries)
        {
            const resolved_type key = check_expr(*entry.key);
            require_compatible(loc, key_type, key, "map key");
            const resolved_type value = check_expr(*entry.value);
            require_compatible(loc, value_type, value, "map value");
        }
        return resolved_type::map_of(key_type, value_type);
    }

    [[nodiscard]] resolved_type
    check_map_entries_literal(const ast::map_entries_literal &literal,
                              const resolved_type &map_type,
                              const ast::source_loc &loc)
    {
        resolved_type key_type = resolved_type::unknown();
        resolved_type value_type = resolved_type::unknown();
        if (map_type.tag == resolved_type::kind::Map && map_type.elements.size() == 2)
        {
            key_type = map_type.elements[0];
            value_type = map_type.elements[1];
        }
        for (const ast::map_entry &entry : literal.entries)
        {
            const resolved_type key = check_expr(*entry.key);
            if (!key_type.is_unknown())
            {
                require_compatible(loc, key_type, key, "map key");
            }
            const resolved_type value = check_expr(*entry.value);
            if (!value_type.is_unknown())
            {
                require_compatible(loc, value_type, value, "map value");
            }
        }
        return resolved_type::map_of(key_type, value_type);
    }

    void check_block(const std::vector<std::unique_ptr<ast::stmt_node>> &statements)
    {
        scope_frame block{};
        block.parent = current_scope_;
        scope_frame *previous = current_scope_;
        current_scope_ = &block;

        for (const auto &stmt : statements)
        {
            collect_declarations(*stmt);
        }
        for (const auto &stmt : statements)
        {
            check_stmt(*stmt);
        }

        current_scope_ = previous;
    }

    void check_if(const ast::if_stmt &stmt)
    {
        (void)check_expr(*stmt.then_branch->condition);
        check_block(stmt.then_branch->body->statements);
        for (const auto &branch : stmt.else_if_branches)
        {
            (void)check_expr(*branch->condition);
            check_block(branch->body->statements);
        }
        if (stmt.else_branch)
        {
            check_block((*stmt.else_branch)->statements);
        }
    }

    void check_loop(const ast::loop_stmt &stmt)
    {
        if (stmt.condition)
        {
            (void)check_expr(**stmt.condition);
        }
        check_block(stmt.body->statements);
    }

    void check_monitor(const ast::monitor_stmt &stmt)
    {
        check_block(stmt.protected_block->statements);

        scope_frame handler{};
        handler.parent = current_scope_;
        scope_frame *previous = current_scope_;
        current_scope_ = &handler;
        current_scope_->vars.emplace(stmt.trap_name,
                                     resolve_type_node(*stmt.trap_type));
        check_block(stmt.handler->statements);
        current_scope_ = previous;
    }

    static void register_nested_decl(scope_frame &scope, const ast::stmt_node &stmt)
    {
        switch (stmt.type)
        {
        case ast::stmt_type::FuncDecl:
        {
            const auto &fn = ast::as_stmt<ast::func_decl>(stmt);
            function_sig sig{};
            sig.loc = stmt.loc;
            sig.ret = resolved_type::unknown();
            sig.params.reserve(fn.parameters.size());
            sig.params.assign(fn.parameters.size(), resolved_type::unknown());
            scope.funcs.emplace(fn.name, std::move(sig));
            break;
        }
        case ast::stmt_type::EnumDecl:
        {
            const auto &en = ast::as_stmt<ast::enum_decl>(stmt);
            scope.enums.emplace(en.name, enum_info{en.members, stmt.loc});
            break;
        }
        case ast::stmt_type::ObjectDecl:
        {
            const auto &obj = ast::as_stmt<ast::object_decl>(stmt);
            std::vector<object_field> fields;
            for (const ast::object_field &field : obj.fields)
            {
                fields.push_back(object_field{field.name, resolved_type::unknown()});
            }
            scope.objects.emplace(obj.name, std::move(fields));
            break;
        }
        case ast::stmt_type::Block:
            for (const auto &inner : ast::as_stmt<ast::block_stmt>(stmt).statements)
            {
                register_nested_decl(scope, *inner);
            }
            break;
        case ast::stmt_type::If:
        {
            const auto &if_stmt = ast::as_stmt<ast::if_stmt>(stmt);
            for (const auto &inner : if_stmt.then_branch->body->statements)
            {
                register_nested_decl(scope, *inner);
            }
            for (const auto &branch : if_stmt.else_if_branches)
            {
                for (const auto &inner : branch->body->statements)
                {
                    register_nested_decl(scope, *inner);
                }
            }
            if (if_stmt.else_branch)
            {
                for (const auto &inner : (*if_stmt.else_branch)->statements)
                {
                    register_nested_decl(scope, *inner);
                }
            }
            break;
        }
        case ast::stmt_type::Loop:
            for (const auto &inner : ast::as_stmt<ast::loop_stmt>(stmt).body->statements)
            {
                register_nested_decl(scope, *inner);
            }
            break;
        default:
            break;
        }
    }

    void register_function_in_scope(scope_frame &scope, const ast::func_decl &decl)
    {
        function_sig sig{};
        sig.loc = decl.body->loc;
        sig.ret = resolve_type_node(*decl.return_type);
        sig.params.reserve(decl.parameters.size());
        for (const ast::parameter &param : decl.parameters)
        {
            sig.params.push_back(resolve_type_node(*param.type));
        }
        scope.funcs.insert_or_assign(decl.name, std::move(sig));
    }

    void check_function(const ast::func_decl &decl)
    {
        scope_frame body{};
        body.parent = current_scope_;
        register_function_in_scope(body, decl);

        const function_sig *sig = body.lookup_func(decl.name);
        if (sig == nullptr)
        {
            return;
        }

        for (size_t i = 0; i < decl.parameters.size(); ++i)
        {
            body.vars.emplace(decl.parameters[i].name, sig->params[i]);
        }
        for (const auto &stmt : decl.body->statements)
        {
            register_nested_decl(body, *stmt);
        }
        for (const auto &stmt : decl.body->statements)
        {
            if (stmt->type == ast::stmt_type::FuncDecl)
            {
                register_function_in_scope(body, ast::as_stmt<ast::func_decl>(*stmt));
            }
        }

        scope_frame *previous_scope = current_scope_;
        const function_sig *previous_function = current_function_;
        current_scope_ = &body;
        current_function_ = sig;
        saw_return_ = sig->ret.tag == resolved_type::kind::Void;

        for (const auto &stmt : decl.body->statements)
        {
            check_stmt(*stmt);
        }

        if (sig->ret.tag != resolved_type::kind::Void && !saw_return_)
        {
            fail(decl.body->loc, "function `" + decl.name +
                                 "` with return type `" + sig->ret.stringify() +
                                 "` may fall through without returning a value");
        }

        current_scope_ = previous_scope;
        current_function_ = previous_function;
    }

    void check_return(const ast::return_stmt &stmt, const ast::source_loc &loc)
    {
        saw_return_ = true;
        if (current_function_ == nullptr)
        {
            if (stmt.value)
            {
                fail(loc, "`return` with a value is only allowed inside a function");
            }
            return;
        }

        if (current_function_->ret.tag == resolved_type::kind::Void)
        {
            if (stmt.value)
            {
                fail(loc, "function returning `void` cannot return a value");
            }
            return;
        }

        if (!stmt.value)
        {
            fail(loc, "function returning `" + current_function_->ret.stringify() +
                         "` requires a return value");
        }

        const resolved_type value_type = check_expr(**stmt.value);
        require_compatible(loc, current_function_->ret, value_type, "return type mismatch");
    }

    void bind_targets(const std::vector<ast::bind_target> &targets,
                      const resolved_type &value_type, const ast::source_loc &loc)
    {
        if (targets.size() == 1)
        {
            if (!targets.front().is_discard)
            {
                current_scope_->vars.insert_or_assign(targets.front().name, value_type);
            }
            return;
        }

        if (value_type.tag != resolved_type::kind::Tuple)
        {
            if (value_type.is_unknown() || value_type.tag == resolved_type::kind::Array)
            {
                for (const ast::bind_target &target : targets)
                {
                    if (!target.is_discard)
                    {
                        resolved_type bound = resolved_type::unknown();
                        if (value_type.tag == resolved_type::kind::Array &&
                            !value_type.elements.empty())
                        {
                            bound = value_type.elements.front();
                        }
                        current_scope_->vars.insert_or_assign(target.name, bound);
                    }
                }
                return;
            }
            fail(loc, "destructure of `" + value_type.stringify() +
                         "` requires a tuple with " + std::to_string(targets.size()) +
                         " element(s)");
        }
        if (targets.size() != value_type.elements.size())
        {
            fail(loc, "destructure expects " +
                         std::to_string(value_type.elements.size()) +
                         " target(s), got " + std::to_string(targets.size()));
        }
        for (size_t i = 0; i < targets.size(); ++i)
        {
            if (targets[i].is_discard)
            {
                continue;
            }
            current_scope_->vars.insert_or_assign(targets[i].name, value_type.elements[i]);
        }
    }

    void check_assignment(const ast::assignment_stmt &stmt, const ast::source_loc &loc)
    {
        const resolved_type value_type = check_expr(*stmt.value);

        if (stmt.op == ast::assign_op::AddAssign)
        {
            for (const ast::bind_target &target : stmt.targets)
            {
                if (target.is_discard)
                {
                    continue;
                }
                const resolved_type existing = current_scope_->lookup_var(target.name);
                if (!existing.is_unknown())
                {
                    require_compatible(loc, existing, value_type, "`:+=` result mismatch");
                }
                current_scope_->vars.insert_or_assign(target.name, existing.is_unknown()
                                                                      ? value_type
                                                                      : existing);
            }
            return;
        }

        bind_targets(stmt.targets, value_type, loc);
    }

    void check_match(const ast::match_stmt &stmt, const ast::source_loc &loc)
    {
        const resolved_type scrutinee = check_expr(*stmt.scrutinee);
        for (const ast::match_case &match_case : stmt.cases)
        {
            const enum_info *enum_type = current_scope_->lookup_enum(match_case.enum_name);
            if (enum_type != nullptr)
            {
                const auto member = std::find(enum_type->members.begin(),
                                              enum_type->members.end(), match_case.member);
                if (member == enum_type->members.end())
                {
                    fail(match_case.loc, "enum `" + match_case.enum_name + "` has no member `" +
                                         match_case.member + '`');
                }
                if (!scrutinee.is_unknown())
                {
                    require_compatible(loc, resolved_type::named(match_case.enum_name),
                                       scrutinee, "match scrutinee type mismatch");
                }
            }
            check_block(match_case.body->statements);
        }
    }

    [[nodiscard]] resolved_type check_simd_expr(const ast::simd_expr &expr,
                                                const ast::source_loc &loc)
    {
        const resolved_type operand = check_expr(*expr.operand);
        if (operand.tag == resolved_type::kind::Array && !operand.elements.empty())
        {
            const resolved_type &lane = operand.elements.front();
            if (!lane.is_unknown() && !lane.is_simd_lane_primitive())
            {
                fail(loc, "`simd` requires a homogeneous array of int, float, char, or bool");
            }
            return resolved_type::simd(lane);
        }
        if (operand.is_unknown())
        {
            return resolved_type::simd(resolved_type::unknown());
        }
        fail(loc, "`simd` operand must be an array of primitive values");
        return resolved_type::unknown();
    }

    [[nodiscard]] resolved_type check_binary(const ast::binary_expr &expr,
                                             const ast::source_loc &loc)
    {
        const resolved_type left = check_expr(*expr.left);
        const resolved_type right = check_expr(*expr.right);

        switch (expr.op)
        {
        case ast::binary_op::Add:
        case ast::binary_op::Sub:
        case ast::binary_op::Mul:
        case ast::binary_op::Div:
        case ast::binary_op::Mod:
            check_constant_int_overflow(expr, loc);
            if (left.is_simd() && right.is_simd() &&
                (expr.op == ast::binary_op::Add || expr.op == ast::binary_op::Sub ||
                 expr.op == ast::binary_op::Mul))
            {
                if (!left.is_unknown() && !right.is_unknown() &&
                    left.elements.front() != right.elements.front())
                {
                    fail(loc, "SIMD operands must have the same lane type");
                }
                return left.is_unknown() ? right : left;
            }
            if (expr.op == ast::binary_op::Mul && left.tag == resolved_type::kind::Array &&
                right.tag == resolved_type::kind::Primitive &&
                right.prim == ast::primitive_kind::Int)
            {
                return left;
            }
            if (left.tag == resolved_type::kind::Primitive &&
                left.prim == ast::primitive_kind::String && expr.op == ast::binary_op::Add)
            {
                return resolved_type::make_primitive(ast::primitive_kind::String);
            }
            if ((left.tag == resolved_type::kind::Primitive &&
                 left.prim == ast::primitive_kind::Float) ||
                (right.tag == resolved_type::kind::Primitive &&
                 right.prim == ast::primitive_kind::Float))
            {
                return resolved_type::make_primitive(ast::primitive_kind::Float);
            }
            return resolved_type::make_primitive(ast::primitive_kind::Int);
        case ast::binary_op::Eq:
        case ast::binary_op::Ne:
        case ast::binary_op::Lt:
        case ast::binary_op::Gt:
        case ast::binary_op::Le:
        case ast::binary_op::Ge:
        case ast::binary_op::And:
        case ast::binary_op::Or:
            return resolved_type::make_primitive(ast::primitive_kind::Bool);
        case ast::binary_op::BitwiseAnd:
        case ast::binary_op::BitwiseOr:
        case ast::binary_op::BitwiseXor:
            return resolved_type::make_primitive(ast::primitive_kind::Int);
        }
        fail(loc, "unsupported binary operator");
    }

    [[nodiscard]] resolved_type check_unary(const ast::unary_expr &expr,
                                            const ast::source_loc &loc)
    {
        (void)check_expr(*expr.operand);
        switch (expr.op)
        {
        case ast::unary_op::Not:
            return resolved_type::make_primitive(ast::primitive_kind::Bool);
        case ast::unary_op::Neg:
        {
            if (const std::optional<int64_t> literal = try_int_literal(*expr.operand);
                literal.has_value() &&
                *literal == std::numeric_limits<int64_t>::min())
            {
                warn_at(loc, "integer overflow in constant expression");
            }
            return resolved_type::make_primitive(ast::primitive_kind::Int);
        }
        case ast::unary_op::BitwiseNot:
            return resolved_type::make_primitive(ast::primitive_kind::Int);
        }
        fail(loc, "unsupported unary operator");
    }

    [[nodiscard]] resolved_type check_call(const ast::call_expr &expr,
                                           const ast::source_loc &loc)
    {
        const resolved_type callee_type = check_expr(*expr.callee);

        if (callee_type.tag == resolved_type::kind::Named)
        {
            const std::vector<object_field> *object = current_scope_->lookup_object(callee_type.name);
            if (object != nullptr)
            {
                if (expr.arguments.size() != object->size())
                {
                    fail(loc, "object `" + callee_type.name + "` expects " +
                                 std::to_string(object->size()) + " field argument(s), got " +
                                 std::to_string(expr.arguments.size()));
                }
                for (size_t i = 0; i < expr.arguments.size(); ++i)
                {
                    const resolved_type arg_type = check_expr(*expr.arguments[i]);
                    require_compatible(loc, (*object)[i].type, arg_type,
                                       "constructor argument `" + (*object)[i].name + '`');
                }
                return resolved_type::named(callee_type.name);
            }
        }

        if (expr.callee->type == ast::expr_type::Identifier)
        {
            const auto &identifier = ast::as<ast::identifier>(*expr.callee);
            const function_sig *sig = current_scope_->lookup_func(identifier.name);
            if (sig != nullptr)
            {
                if (!sig->variadic && expr.arguments.size() != sig->params.size())
                {
                    fail(loc, "function `" + identifier.name + "` expects " +
                                 std::to_string(sig->params.size()) + " argument(s), got " +
                                 std::to_string(expr.arguments.size()));
                }
                if (sig->variadic && sig->params.empty())
                {
                    for (const auto &argument : expr.arguments)
                    {
                        (void)check_expr(*argument);
                    }
                    return sig->ret;
                }
                const size_t check_count =
                    sig->variadic ? std::min(expr.arguments.size(), sig->params.size())
                                  : expr.arguments.size();
                for (size_t i = 0; i < check_count; ++i)
                {
                    const resolved_type arg_type = check_expr(*expr.arguments[i]);
                    require_compatible(loc, sig->params[i], arg_type,
                                       "argument `" + identifier.name + '`');
                }
                return sig->ret;
            }
        }

        for (const auto &argument : expr.arguments)
        {
            (void)check_expr(*argument);
        }

        return resolved_type::unknown();
    }

    [[nodiscard]] resolved_type check_member(const ast::member_expr &expr,
                                             const ast::source_loc &loc)
    {
        const resolved_type object_type = check_expr(*expr.object);

        if (expr.member == "len")
        {
            if (object_type.tag == resolved_type::kind::Array ||
                (object_type.tag == resolved_type::kind::Primitive &&
                 object_type.prim == ast::primitive_kind::String))
            {
                return resolved_type::make_primitive(ast::primitive_kind::Int);
            }
            if (object_type.is_unknown())
            {
                return resolved_type::make_primitive(ast::primitive_kind::Int);
            }
        }

        if (object_type.tag == resolved_type::kind::Named)
        {
            const std::vector<object_field> *object = current_scope_->lookup_object(object_type.name);
            if (object != nullptr)
            {
                for (const object_field &field : *object)
                {
                    if (field.name == expr.member)
                    {
                        return field.type;
                    }
                }
                fail(loc, "object `" + object_type.name + "` has no field `" + expr.member + '`');
            }
        }

        if (expr.member == "id" && object_type.tag == resolved_type::kind::Named &&
            object_type.name == "process")
        {
            return resolved_type::make_primitive(ast::primitive_kind::Int);
        }

        return resolved_type::unknown();
    }

    [[nodiscard]] resolved_type check_index(const ast::index_expr &expr,
                                            const ast::source_loc &loc)
    {
        const resolved_type container = check_expr(*expr.object);
        const resolved_type index = check_expr(*expr.index);
        require_compatible(loc, resolved_type::make_primitive(ast::primitive_kind::Int), index,
                           "array index");

        if (container.tag == resolved_type::kind::Array && !container.elements.empty())
        {
            return container.elements.front();
        }
        return resolved_type::unknown();
    }

    [[nodiscard]] resolved_type check_expr(const ast::expr_node &expr)
    {
        resolved_type result = check_expr_impl(expr);
        if (annotate_)
        {
            expr_types_.exprs[&expr] = result;
        }
        return result;
    }

    [[nodiscard]] resolved_type check_expr_impl(const ast::expr_node &expr)
    {
        switch (expr.type)
        {
        case ast::expr_type::IntLiteral:
            return resolved_type::make_primitive(ast::primitive_kind::Int);
        case ast::expr_type::FloatLiteral:
            return resolved_type::make_primitive(ast::primitive_kind::Float);
        case ast::expr_type::StringLiteral:
            return resolved_type::make_primitive(ast::primitive_kind::String);
        case ast::expr_type::CharLiteral:
            return resolved_type::make_primitive(ast::primitive_kind::Character);
        case ast::expr_type::BoolLiteral:
            return resolved_type::make_primitive(ast::primitive_kind::Bool);
        case ast::expr_type::NullLiteral:
        {
            resolved_type out{};
            out.tag = resolved_type::kind::Null;
            return out;
        }
        case ast::expr_type::RegexLiteral:
            return resolved_type::unknown();
        case ast::expr_type::Identifier:
        {
            const auto &name = ast::as<ast::identifier>(expr).name;
            if (const std::vector<object_field> *object = current_scope_->lookup_object(name))
            {
                (void)object;
                return resolved_type::named(name);
            }
            return current_scope_->lookup_var(name);
        }
        case ast::expr_type::Binary:
            return check_binary(ast::as<ast::binary_expr>(expr), expr.loc);
        case ast::expr_type::Unary:
            return check_unary(ast::as<ast::unary_expr>(expr), expr.loc);
        case ast::expr_type::Call:
            return check_call(ast::as<ast::call_expr>(expr), expr.loc);
        case ast::expr_type::Member:
            return check_member(ast::as<ast::member_expr>(expr), expr.loc);
        case ast::expr_type::EnumAccess:
        {
            const auto &access = ast::as<ast::enum_access_expr>(expr);
            const enum_info *info = current_scope_->lookup_enum(access.enum_name);
            if (info != nullptr)
            {
                const auto member = std::find(info->members.begin(), info->members.end(),
                                              access.member);
                if (member == info->members.end())
                {
                    fail(expr.loc, "enum `" + access.enum_name + "` has no member `" +
                                     access.member + '`');
                }
            }
            return resolved_type::enum_member(access.enum_name);
        }
        case ast::expr_type::Index:
            return check_index(ast::as<ast::index_expr>(expr), expr.loc);
        case ast::expr_type::ArrayLiteral:
        {
            resolved_type element = resolved_type::unknown();
            for (const auto &item : ast::as<ast::array_literal>(expr).elements)
            {
                const resolved_type item_type = check_expr(*item);
                if (element.is_unknown() && !item_type.is_unknown())
                {
                    element = item_type;
                }
            }
            return resolved_type::array(element);
        }
        case ast::expr_type::TypedArrayLiteral:
        {
            const auto &literal = ast::as<ast::typed_array_literal>(expr);
            const resolved_type element = resolve_type_node(*literal.element_type);
            for (const auto &item : literal.elements)
            {
                const resolved_type item_type = check_expr(*item);
                require_compatible(expr.loc, element, item_type, "typed array element");
            }
            return resolved_type::array(element);
        }
        case ast::expr_type::TupleLiteral:
        {
            std::vector<resolved_type> fields;
            for (const auto &item : ast::as<ast::tuple_literal>(expr).elements)
            {
                fields.push_back(check_expr(*item));
            }
            return resolved_type::tuple(std::move(fields));
        }
        case ast::expr_type::MapLiteral:
            return check_map_literal(ast::as<ast::map_literal>(expr), expr.loc);
        case ast::expr_type::MapEntriesLiteral:
            return check_map_entries_literal(ast::as<ast::map_entries_literal>(expr),
                                             resolved_type::unknown(), expr.loc);
        case ast::expr_type::PipeInsert:
        {
            const auto &pipe = ast::as<ast::pipe_insert_expr>(expr);
            (void)check_expr(*pipe.value);
            return resolved_type::unknown();
        }
        case ast::expr_type::PipeExtract:
            return resolved_type::unknown();
        case ast::expr_type::Cast:
        {
            const auto &cast = ast::as<ast::cast_expr>(expr);
            (void)check_expr(*cast.operand);
            return resolve_type_node(*cast.target_type);
        }
        case ast::expr_type::Alloc:
        {
            const auto &alloc = ast::as<ast::alloc_expr>(expr);
            (void)check_expr(*alloc.capacity);
            for (const auto &item : alloc.initial_values)
            {
                (void)check_expr(*item);
            }
            return resolved_type::buffer();
        }
        case ast::expr_type::Free:
        {
            const auto &free_expr = ast::as<ast::free_expr>(expr);
            const resolved_type target = current_scope_->lookup_var(free_expr.buffer_name);
            if (!target.is_unknown() && target.tag != resolved_type::kind::Buffer)
            {
                fail(expr.loc, "`free`/`delete` expects a buffer, `" + free_expr.buffer_name +
                                 "` has type `" + target.stringify() + '`');
            }
            return resolved_type::make_primitive(ast::primitive_kind::Void);
        }
        case ast::expr_type::Simd:
            return check_simd_expr(ast::as<ast::simd_expr>(expr), expr.loc);
        case ast::expr_type::Lambda:
            return check_lambda(ast::as<ast::lambda_expr>(expr), expr.loc);
        }
        return resolved_type::unknown();
    }

    [[nodiscard]] resolved_type check_lambda(const ast::lambda_expr &expr,
                                             const ast::source_loc &loc)
    {
        const resolved_type ret = resolve_type_node(*expr.return_type);
        std::vector<resolved_type> params;
        params.reserve(expr.parameters.size());
        for (const ast::parameter &param : expr.parameters)
        {
            params.push_back(resolve_type_node(*param.type));
        }

        function_sig sig{};
        sig.loc = loc;
        sig.params = params;
        sig.ret = ret;

        scope_frame body{};
        body.parent = current_scope_;
        for (size_t i = 0; i < expr.parameters.size(); ++i)
        {
            body.vars.emplace(expr.parameters[i].name, params[i]);
        }

        scope_frame *previous_scope = current_scope_;
        const function_sig *previous_function = current_function_;
        current_scope_ = &body;
        current_function_ = &sig;
        saw_return_ = ret.tag == resolved_type::kind::Void;

        for (const auto &stmt : expr.body->statements)
        {
            check_stmt(*stmt);
        }

        current_scope_ = previous_scope;
        current_function_ = previous_function;

        return resolved_type::unknown();
    }
};

} // namespace munx
