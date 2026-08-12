#pragma once

/// Compile-time generics monomorphization + reflexpr lowering.
/// Rewrites a parsed program so later stages never see ReflectFor / TypeidMatch
/// or generic func bodies.

#include "ast.hpp"
#include "errors.hpp"
#include "keywords.hpp"
#include "lexer.hpp"
#include "parser.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace munx
{

namespace detail
{

struct field_info
{
    std::string name;
    std::string kind; ///< "object" or primitive name ("int", "string", …)
    std::string type_name; ///< Named type if object, else kind
};

struct object_info
{
    std::vector<field_info> fields;
};

inline std::string type_kind_name(const ast::type_node &type)
{
    switch (type.type)
    {
    case ast::type_kind::Primitive:
        switch (std::get<ast::primitive_type>(type.value).kind)
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
        return "unknown";
    case ast::type_kind::Named:
        return "object";
    case ast::type_kind::Array:
        return "array";
    case ast::type_kind::Tuple:
        return "tuple";
    case ast::type_kind::Map:
        return "map";
    case ast::type_kind::Lambda:
        return "lambda";
    }
    return "unknown";
}

inline std::string named_type_name(const ast::type_node &type)
{
    if (type.type == ast::type_kind::Named)
    {
        return std::get<ast::named_type>(type.value).name;
    }
    return type_kind_name(type);
}

inline std::unique_ptr<ast::type_node> clone_type(const ast::type_node &type)
{
    auto out = std::make_unique<ast::type_node>();
    out->loc = type.loc;
    out->type = type.type;
    switch (type.type)
    {
    case ast::type_kind::Primitive:
        out->value = std::get<ast::primitive_type>(type.value);
        break;
    case ast::type_kind::Named:
        out->value = std::get<ast::named_type>(type.value);
        break;
    case ast::type_kind::Array:
    {
        ast::array_type arr;
        arr.element = clone_type(*std::get<ast::array_type>(type.value).element);
        out->value = std::move(arr);
        break;
    }
    case ast::type_kind::Tuple:
    {
        ast::tuple_type tup;
        for (const auto &el : std::get<ast::tuple_type>(type.value).elements)
        {
            tup.elements.push_back(clone_type(*el));
        }
        out->value = std::move(tup);
        break;
    }
    case ast::type_kind::Map:
    {
        const auto &m = std::get<ast::map_type>(type.value);
        ast::map_type map;
        map.key = clone_type(*m.key);
        map.value = clone_type(*m.value);
        out->value = std::move(map);
        break;
    }
    case ast::type_kind::Lambda:
    {
        const auto &l = std::get<ast::lambda_type>(type.value);
        ast::lambda_type lam;
        for (const auto &p : l.params)
        {
            lam.params.push_back(clone_type(*p));
        }
        lam.ret = clone_type(*l.ret);
        out->value = std::move(lam);
        break;
    }
    }
    return out;
}

inline void subst_type(ast::type_node &type,
                       const std::unordered_map<std::string, std::string> &bind)
{
    if (type.type == ast::type_kind::Named)
    {
        auto &named = std::get<ast::named_type>(type.value);
        const auto found = bind.find(named.name);
        if (found != bind.end())
        {
            named.name = found->second;
        }
        return;
    }
    if (type.type == ast::type_kind::Array)
    {
        subst_type(*std::get<ast::array_type>(type.value).element, bind);
    }
    else if (type.type == ast::type_kind::Tuple)
    {
        for (auto &el : std::get<ast::tuple_type>(type.value).elements)
        {
            subst_type(*el, bind);
        }
    }
    else if (type.type == ast::type_kind::Map)
    {
        auto &m = std::get<ast::map_type>(type.value);
        subst_type(*m.key, bind);
        subst_type(*m.value, bind);
    }
    else if (type.type == ast::type_kind::Lambda)
    {
        auto &l = std::get<ast::lambda_type>(type.value);
        for (auto &p : l.params)
        {
            subst_type(*p, bind);
        }
        subst_type(*l.ret, bind);
    }
}

inline std::string callee_name(const ast::expr_node &expr)
{
    if (expr.type == ast::expr_type::Identifier)
    {
        return ast::as<ast::identifier>(expr).name;
    }
    return {};
}

inline bool is_compiler_call(const ast::expr_node &expr, const char *name)
{
    if (expr.type != ast::expr_type::Call)
    {
        return false;
    }
    return callee_name(*ast::as<ast::call_expr>(expr).callee) == name;
}

inline std::unique_ptr<ast::expr_node> make_string(const std::string &s,
                                                   const ast::source_loc &loc)
{
    return ast::make_expr_ptr(ast::string_literal{s}, loc);
}

inline std::unique_ptr<ast::expr_node> make_ident(const std::string &s,
                                                  const ast::source_loc &loc)
{
    return ast::make_expr_ptr(ast::identifier{s}, loc);
}

inline std::unique_ptr<ast::stmt_node>
make_println2(const std::string &a, const std::string &b, const ast::source_loc &loc)
{
    ast::call_expr call;
    call.callee = make_ident("println", loc);
    call.arguments.push_back(make_string(a, loc));
    call.arguments.push_back(make_string(b, loc));
    return ast::make_stmt_ptr(
        ast::expr_stmt{ast::make_expr_ptr(std::move(call), loc)}, loc);
}

struct param_info
{
    std::string name;
    std::string type_name; ///< Display name (e.g. T, int, Person)
    std::string kind;      ///< typeid kind string
};

struct func_info
{
    std::string name;
    std::string package_name;
    std::string return_type_name;
    std::vector<std::string> type_params;
    std::vector<param_info> params;
    bool is_meta{false};
};

struct package_info
{
    std::string name;
    std::vector<std::string> function_names;
};

struct ct_value
{
    enum class kind
    {
        None,
        ObjectArtifact,  ///< ObjectReflectionArtifact
        FuncArtifact,    ///< FunctionReflectionArtifact
        PackageArtifact, ///< PackageReflectionArtifact
        Members,         ///< Object field list
        FuncMembers,     ///< Package function list
        MetaParams,      ///< Generic type-param list
        FuncParams,      ///< Function parameter list
        Field,           ///< Object field
        MetaParam,       ///< One generic type param
        FuncParam,       ///< One function parameter
        TypeName,        ///< Bound local's static object type name
        Bool,            ///< Compile-time bool
    } tag{kind::None};
    std::string type_name;
    std::string func_name;
    std::string package_name;
    bool flag{false};
    std::vector<field_info> fields;
    std::vector<param_info> params;
    std::vector<std::string> meta_names;
    std::vector<std::string> func_names;
    field_info field{};
    param_info param{};
};

class expander
{
public:
    explicit expander(ast::program &program) : program_(program)
    {
        collect_objects();
        collect_functions();
        collect_imported_packages();
    }

    void run()
    {
        collect_generics();
        scan_local_types(program_.statements);
        specialize_calls(program_.statements);
        rebuild_with_toplevel_lowering();
    }

private:
    ast::program &program_;
    std::unordered_map<std::string, object_info> objects_;
    std::unordered_map<std::string, func_info> functions_;
    std::unordered_map<std::string, package_info> packages_;
    std::unordered_set<std::string> imported_packages_;
    std::unordered_map<std::string, const ast::func_decl *> generics_;
    std::unordered_map<std::string, std::string> local_types_;
    std::unordered_map<std::string, bool> specialized_done_;
    std::vector<std::unique_ptr<ast::stmt_node>> specialized_;
    /// Field being unrolled by `::reflect_for` (for nested generic inference).
    const field_info *current_field_{nullptr};

    static func_info make_func_info(const ast::func_decl &fn,
                                    const std::string &package_name)
    {
        func_info info;
        info.name = fn.name;
        info.package_name = package_name;
        info.return_type_name = named_type_name(*fn.return_type);
        info.type_params = fn.type_params;
        info.is_meta = !fn.type_params.empty();
        for (const auto &p : fn.parameters)
        {
            param_info pi;
            pi.name = p.name;
            pi.type_name = named_type_name(*p.type);
            pi.kind = type_kind_name(*p.type);
            info.params.push_back(std::move(pi));
        }
        return info;
    }

    void collect_objects()
    {
        for (const auto &stmt : program_.statements)
        {
            if (stmt->type != ast::stmt_type::ObjectDecl)
            {
                continue;
            }
            const auto &decl = ast::as_stmt<ast::object_decl>(*stmt);
            object_info info;
            for (const auto &field : decl.fields)
            {
                field_info f;
                f.name = field.name;
                f.kind = type_kind_name(*field.type);
                f.type_name = named_type_name(*field.type);
                info.fields.push_back(std::move(f));
            }
            objects_.emplace(decl.name, std::move(info));
        }
    }

    void collect_functions()
    {
        package_info pkg;
        pkg.name = program_.package_name;
        for (const auto &stmt : program_.statements)
        {
            if (stmt->type != ast::stmt_type::FuncDecl)
            {
                continue;
            }
            const auto &fn = ast::as_stmt<ast::func_decl>(*stmt);
            func_info info = make_func_info(fn, program_.package_name);
            pkg.function_names.push_back(info.name);
            functions_.emplace(fn.name, std::move(info));
        }
        packages_.emplace(program_.package_name, std::move(pkg));
    }

    static std::string read_source_file(const std::filesystem::path &path)
    {
        std::ifstream in(path);
        if (!in)
        {
            return {};
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        return buf.str();
    }

    void ingest_package_program(const ast::program &pkg)
    {
        package_info info;
        info.name = pkg.package_name;
        for (const auto &stmt : pkg.statements)
        {
            if (stmt->type != ast::stmt_type::FuncDecl)
            {
                continue;
            }
            const auto &fn = ast::as_stmt<ast::func_decl>(*stmt);
            func_info fi = make_func_info(fn, pkg.package_name);
            // Qualify foreign function keys so they do not collide with locals.
            const std::string key = pkg.package_name + "::" + fi.name;
            info.function_names.push_back(fi.name);
            functions_.emplace(key, std::move(fi));
        }
        packages_.emplace(pkg.package_name, std::move(info));
    }

    void collect_imported_packages()
    {
        for (const auto &imp : program_.imports)
        {
            imported_packages_.insert(imp.package);
        }
        if (program_.imports.empty() || program_.package_loc.file.empty())
        {
            return;
        }
        const auto dir =
            std::filesystem::path{program_.package_loc.file}.parent_path();
        for (const auto &imp : program_.imports)
        {
            if (imp.package == program_.package_name ||
                packages_.contains(imp.package))
            {
                continue;
            }
            const auto path = dir / (imp.package + ".mx");
            const std::string source = read_source_file(path);
            if (source.empty())
            {
                continue;
            }
            lexer lex{source, keywords(), path};
            parser parse{lex.read_tokens(), path};
            ast::program imported = parse.parse_program();
            // Do not expand imports recursively here; only harvest declarations.
            ingest_package_program(imported);
        }
    }

    const func_info *lookup_func(const ct_value &art) const
    {
        if (art.tag != ct_value::kind::FuncArtifact)
        {
            return nullptr;
        }
        if (!art.package_name.empty() &&
            art.package_name != program_.package_name)
        {
            const auto key = art.package_name + "::" + art.func_name;
            const auto found = functions_.find(key);
            if (found != functions_.end())
            {
                return &found->second;
            }
        }
        const auto found = functions_.find(art.func_name);
        if (found == functions_.end())
        {
            return nullptr;
        }
        return &found->second;
    }

    static std::string stringify_params(const std::vector<param_info> &params)
    {
        std::string out = "[";
        for (size_t i = 0; i < params.size(); ++i)
        {
            if (i != 0)
            {
                out += ", ";
            }
            out += params[i].name;
            out += ": ";
            out += params[i].type_name;
        }
        out += "]";
        return out;
    }

    static std::string stringify_meta_params(const std::vector<std::string> &names)
    {
        std::string out = "[";
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (i != 0)
            {
                out += ", ";
            }
            out += names[i];
        }
        out += "]";
        return out;
    }

    void collect_generics()
    {
        for (const auto &stmt : program_.statements)
        {
            if (stmt->type != ast::stmt_type::FuncDecl)
            {
                continue;
            }
            const auto &fn = ast::as_stmt<ast::func_decl>(*stmt);
            if (!fn.type_params.empty())
            {
                generics_.emplace(fn.name, &fn);
            }
        }
    }

    void rebuild_with_toplevel_lowering()
    {
        std::vector<std::unique_ptr<ast::stmt_node>> out;
        out.reserve(program_.statements.size() + specialized_.size());
        std::unordered_map<std::string, ct_value> env;
        std::unordered_map<std::string, std::string> bind;

        for (auto &stmt : program_.statements)
        {
            if (stmt->type == ast::stmt_type::ObjectDecl ||
                stmt->type == ast::stmt_type::EnumDecl ||
                stmt->type == ast::stmt_type::Lock ||
                stmt->type == ast::stmt_type::LoadPackage)
            {
                out.push_back(std::move(stmt));
                continue;
            }
            if (stmt->type == ast::stmt_type::FuncDecl)
            {
                const auto &fn = ast::as_stmt<ast::func_decl>(*stmt);
                if (!fn.type_params.empty())
                {
                    continue; // drop generic templates after specialization/reflection
                }
                out.push_back(std::move(stmt));
                continue;
            }

            ast::block_stmt tmp;
            tmp.loc = stmt->loc;
            lower_stmt(*stmt, env, bind, tmp, stmt->loc);
            for (auto &emitted : tmp.statements)
            {
                out.push_back(std::move(emitted));
            }
        }
        for (auto &fn : specialized_)
        {
            out.push_back(std::move(fn));
        }
        program_.statements = std::move(out);
    }

    void scan_local_types(const std::vector<std::unique_ptr<ast::stmt_node>> &stmts)
    {
        for (const auto &stmt : stmts)
        {
            if (stmt->type != ast::stmt_type::Assignment)
            {
                continue;
            }
            const auto &assign = ast::as_stmt<ast::assignment_stmt>(*stmt);
            if (assign.targets.size() != 1 || assign.targets.front().name.empty())
            {
                continue;
            }
            if (assign.value->type != ast::expr_type::Call)
            {
                continue;
            }
            const auto &call = ast::as<ast::call_expr>(*assign.value);
            const std::string ctor = callee_name(*call.callee);
            if (objects_.contains(ctor))
            {
                local_types_[assign.targets.front().name] = ctor;
            }
        }
    }

    void specialize_calls(std::vector<std::unique_ptr<ast::stmt_node>> &stmts)
    {
        for (auto &stmt : stmts)
        {
            rewrite_stmt(*stmt);
        }
    }

    void rewrite_stmt(ast::stmt_node &stmt)
    {
        switch (stmt.type)
        {
        case ast::stmt_type::Assignment:
            rewrite_expr(*ast::as_stmt<ast::assignment_stmt>(stmt).value);
            break;
        case ast::stmt_type::Expr:
            rewrite_expr(*ast::as_stmt<ast::expr_stmt>(stmt).expression);
            break;
        case ast::stmt_type::Return:
        {
            auto &ret = ast::as_stmt<ast::return_stmt>(stmt);
            if (ret.value.has_value())
            {
                rewrite_expr(**ret.value);
            }
            break;
        }
        case ast::stmt_type::Block:
            for (auto &inner : ast::as_stmt<ast::block_stmt>(stmt).statements)
            {
                rewrite_stmt(*inner);
            }
            break;
        case ast::stmt_type::If:
        {
            auto &ifs = ast::as_stmt<ast::if_stmt>(stmt);
            rewrite_expr(*ifs.then_branch->condition);
            for (auto &inner : ifs.then_branch->body->statements)
            {
                rewrite_stmt(*inner);
            }
            for (auto &br : ifs.else_if_branches)
            {
                rewrite_expr(*br->condition);
                for (auto &inner : br->body->statements)
                {
                    rewrite_stmt(*inner);
                }
            }
            if (ifs.else_branch)
            {
                for (auto &inner : (*ifs.else_branch)->statements)
                {
                    rewrite_stmt(*inner);
                }
            }
            break;
        }
        case ast::stmt_type::Loop:
        {
            auto &loop = ast::as_stmt<ast::loop_stmt>(stmt);
            if (loop.condition.has_value())
            {
                rewrite_expr(**loop.condition);
            }
            for (auto &inner : loop.body->statements)
            {
                rewrite_stmt(*inner);
            }
            break;
        }
        case ast::stmt_type::FuncDecl:
        {
            auto &fn = ast::as_stmt<ast::func_decl>(stmt);
            if (fn.type_params.empty())
            {
                for (auto &inner : fn.body->statements)
                {
                    rewrite_stmt(*inner);
                }
            }
            break;
        }
        case ast::stmt_type::Monitor:
        {
            auto &mon = ast::as_stmt<ast::monitor_stmt>(stmt);
            for (auto &inner : mon.protected_block->statements)
            {
                rewrite_stmt(*inner);
            }
            for (auto &inner : mon.handler->statements)
            {
                rewrite_stmt(*inner);
            }
            break;
        }
        default:
            break;
        }
    }

    void rewrite_expr(ast::expr_node &expr)
    {
        if (expr.type != ast::expr_type::Call)
        {
            if (expr.type == ast::expr_type::Binary)
            {
                auto &bin = ast::as<ast::binary_expr>(expr);
                rewrite_expr(*bin.left);
                rewrite_expr(*bin.right);
            }
            else if (expr.type == ast::expr_type::Unary)
            {
                rewrite_expr(*ast::as<ast::unary_expr>(expr).operand);
            }
            else if (expr.type == ast::expr_type::Member)
            {
                rewrite_expr(*ast::as<ast::member_expr>(expr).object);
            }
            return;
        }
        auto &call = ast::as<ast::call_expr>(expr);
        rewrite_expr(*call.callee);
        for (auto &arg : call.arguments)
        {
            rewrite_expr(*arg);
        }
        specialize_generic_call(call, expr.loc, nullptr);
    }

    /// Bind type args / infer, monomorphize, and rewrite @p call to the concrete name.
    /// @p outer_bind substitutes type-argument names when lowering inside a specialization.
    void specialize_generic_call(
        ast::call_expr &call, const ast::source_loc &loc,
        const std::unordered_map<std::string, std::string> *outer_bind)
    {
        const std::string name = callee_name(*call.callee);
        auto g = generics_.find(name);
        if (g == generics_.end())
        {
            return;
        }
        const ast::func_decl &tmpl = *g->second;
        std::unordered_map<std::string, std::string> bind;
        if (!call.type_arguments.empty())
        {
            if (call.type_arguments.size() != tmpl.type_params.size())
            {
                fail_compile(loc.file + ':' + std::to_string(loc.line) +
                             ": error: wrong number of type arguments for `" + name +
                             "`");
            }
            for (size_t i = 0; i < tmpl.type_params.size(); ++i)
            {
                std::string concrete = named_type_name(*call.type_arguments[i]);
                if (outer_bind != nullptr)
                {
                    const auto found = outer_bind->find(concrete);
                    if (found != outer_bind->end())
                    {
                        concrete = found->second;
                    }
                }
                bind[tmpl.type_params[i]] = concrete;
            }
        }
        else
        {
            if (call.arguments.size() != tmpl.parameters.size())
            {
                fail_compile(loc.file + ':' + std::to_string(loc.line) +
                             ": error: wrong number of arguments for `" + name + "`");
            }
            for (size_t i = 0; i < tmpl.parameters.size(); ++i)
            {
                const ast::type_node &ptype = *tmpl.parameters[i].type;
                if (ptype.type != ast::type_kind::Named)
                {
                    continue;
                }
                const std::string &pname = std::get<ast::named_type>(ptype.value).name;
                if (std::find(tmpl.type_params.begin(), tmpl.type_params.end(),
                              pname) == tmpl.type_params.end())
                {
                    continue;
                }
                std::string arg_type = infer_expr_type(*call.arguments[i]);
                if (arg_type.empty())
                {
                    continue; // may fill from reflect_for field / return type below
                }
                const auto existing = bind.find(pname);
                if (existing != bind.end() && existing->second != arg_type)
                {
                    fail_compile(loc.file + ':' + std::to_string(loc.line) +
                                 ": error: conflicting inference for `" + pname + "`");
                }
                bind[pname] = arg_type;
            }
            // Inside `::reflect_for` over object fields: nested `from_json(value)`
            // infers T from the current field's object type.
            if (current_field_ != nullptr && current_field_->kind == "object" &&
                tmpl.return_type && tmpl.return_type->type == ast::type_kind::Named)
            {
                const std::string &ret_name =
                    std::get<ast::named_type>(tmpl.return_type->value).name;
                if (std::find(tmpl.type_params.begin(), tmpl.type_params.end(),
                              ret_name) != tmpl.type_params.end() &&
                    !bind.contains(ret_name))
                {
                    bind[ret_name] = current_field_->type_name;
                }
            }
            for (const auto &tp : tmpl.type_params)
            {
                if (!bind.contains(tp))
                {
                    fail_compile(loc.file + ':' + std::to_string(loc.line) +
                                 ": error: cannot infer type parameter `" + tp +
                                 "` for `" + name + "`");
                    return;
                }
            }
        }

        std::string mangled = name;
        for (const auto &tp : tmpl.type_params)
        {
            mangled += "__" + bind.at(tp);
        }
        ensure_specialization(tmpl, bind, mangled, loc);
        call.callee = make_ident(mangled, loc);
        call.type_arguments.clear();
    }

    std::string infer_expr_type(const ast::expr_node &expr) const
    {
        if (expr.type == ast::expr_type::Identifier)
        {
            const auto found = local_types_.find(ast::as<ast::identifier>(expr).name);
            if (found != local_types_.end())
            {
                return found->second;
            }
        }
        if (expr.type == ast::expr_type::Call)
        {
            const std::string ctor = callee_name(*ast::as<ast::call_expr>(expr).callee);
            if (objects_.contains(ctor))
            {
                return ctor;
            }
        }
        return {};
    }

    void ensure_specialization(const ast::func_decl &tmpl,
                               const std::unordered_map<std::string, std::string> &bind,
                               const std::string &mangled, const ast::source_loc &loc)
    {
        if (specialized_done_.contains(mangled))
        {
            return;
        }
        specialized_done_[mangled] = true;

        ast::func_decl spec;
        spec.name = mangled;
        for (const auto &param : tmpl.parameters)
        {
            ast::parameter p;
            p.loc = param.loc;
            p.name = param.name;
            p.type = clone_type(*param.type);
            subst_type(*p.type, bind);
            spec.parameters.push_back(std::move(p));
        }
        spec.return_type = clone_type(*tmpl.return_type);
        subst_type(*spec.return_type, bind);

        // Seed CT env with parameter types and bound type parameters.
        std::unordered_map<std::string, ct_value> env;
        for (const auto &[tp, concrete] : bind)
        {
            ct_value v;
            v.tag = ct_value::kind::TypeName;
            v.type_name = concrete;
            env.emplace(tp, std::move(v));
        }
        for (const auto &param : spec.parameters)
        {
            ct_value v;
            v.tag = ct_value::kind::TypeName;
            v.type_name = named_type_name(*param.type);
            env.emplace(param.name, std::move(v));
        }

        spec.body = std::make_unique<ast::block_stmt>();
        spec.body->loc = tmpl.body->loc;
        lower_block(*tmpl.body, env, bind, *spec.body, loc);

        specialized_.push_back(ast::make_stmt_ptr(std::move(spec), loc));
    }

    void lower_block(const ast::block_stmt &src,
                     std::unordered_map<std::string, ct_value> env,
                     const std::unordered_map<std::string, std::string> &bind,
                     ast::block_stmt &dst, const ast::source_loc &loc)
    {
        for (const auto &stmt : src.statements)
        {
            lower_stmt(*stmt, env, bind, dst, loc);
        }
    }

    void lower_stmt(const ast::stmt_node &stmt,
                    std::unordered_map<std::string, ct_value> &env,
                    const std::unordered_map<std::string, std::string> &bind,
                    ast::block_stmt &dst, const ast::source_loc &loc)
    {
        if (stmt.type == ast::stmt_type::Assignment)
        {
            const auto &assign = ast::as_stmt<ast::assignment_stmt>(stmt);
            if (assign.targets.size() == 1 && !assign.targets.front().name.empty() &&
                assign.value->type == ast::expr_type::Call)
            {
                const auto &call = ast::as<ast::call_expr>(*assign.value);
                const std::string cal = callee_name(*call.callee);
                if (cal == "::reflexpr" && call.arguments.size() == 1)
                {
                    ct_value art = make_reflexpr_value(*call.arguments[0], env, stmt.loc);
                    env[assign.targets.front().name] = std::move(art);
                    return;
                }
                if (cal == "::members" && call.arguments.size() == 1)
                {
                    ct_value mem;
                    mem.tag = ct_value::kind::Members;
                    const ct_value *art = eval_ct(*call.arguments[0], env);
                    if (art == nullptr || art->tag != ct_value::kind::ObjectArtifact)
                    {
                        fail_compile(stmt.loc.file + ':' + std::to_string(stmt.loc.line) +
                                     ": error: ::members expects an ObjectReflectionArtifact");
                    }
                    mem.fields = objects_.at(art->type_name).fields;
                    mem.type_name = art->type_name;
                    env[assign.targets.front().name] = std::move(mem);
                    return;
                }
                if (cal == "::meta_params" && call.arguments.size() == 1)
                {
                    const ct_value *fn = eval_ct(*call.arguments[0], env);
                    if (fn == nullptr || fn->tag != ct_value::kind::FuncArtifact)
                    {
                        fail_compile(stmt.loc.file + ':' + std::to_string(stmt.loc.line) +
                                     ": error: ::meta_params expects a FunctionReflectionArtifact");
                    }
                    const func_info *info = lookup_func(*fn);
                    if (info == nullptr)
                    {
                        fail_compile(stmt.loc.file + ':' + std::to_string(stmt.loc.line) +
                                     ": error: ::meta_params: unknown function artifact");
                    }
                    ct_value meta;
                    meta.tag = ct_value::kind::MetaParams;
                    meta.meta_names = info->type_params;
                    env[assign.targets.front().name] = std::move(meta);
                    return;
                }
                if (cal == "::params" && call.arguments.size() == 1)
                {
                    const ct_value *fn = eval_ct(*call.arguments[0], env);
                    if (fn == nullptr || fn->tag != ct_value::kind::FuncArtifact)
                    {
                        fail_compile(stmt.loc.file + ':' + std::to_string(stmt.loc.line) +
                                     ": error: ::params expects a FunctionReflectionArtifact");
                    }
                    const func_info *info = lookup_func(*fn);
                    if (info == nullptr)
                    {
                        fail_compile(stmt.loc.file + ':' + std::to_string(stmt.loc.line) +
                                     ": error: ::params: unknown function artifact");
                    }
                    ct_value ps;
                    ps.tag = ct_value::kind::FuncParams;
                    ps.params = info->params;
                    env[assign.targets.front().name] = std::move(ps);
                    return;
                }
                if (cal == "::function_members" && call.arguments.size() == 1)
                {
                    const ct_value *pkg = eval_ct(*call.arguments[0], env);
                    if (pkg == nullptr || pkg->tag != ct_value::kind::PackageArtifact)
                    {
                        fail_compile(
                            stmt.loc.file + ':' + std::to_string(stmt.loc.line) +
                            ": error: ::function_members expects a PackageReflectionArtifact");
                    }
                    const auto found = packages_.find(pkg->package_name);
                    if (found == packages_.end())
                    {
                        fail_compile(stmt.loc.file + ':' + std::to_string(stmt.loc.line) +
                                     ": error: unknown package `" + pkg->package_name +
                                     "`");
                    }
                    ct_value mem;
                    mem.tag = ct_value::kind::FuncMembers;
                    mem.package_name = pkg->package_name;
                    mem.func_names = found->second.function_names;
                    env[assign.targets.front().name] = std::move(mem);
                    return;
                }
            }
            // Runtime assignment (e.g. p = Person(...)): keep as-is.
            ast::assignment_stmt copy;
            copy.op = assign.op;
            copy.targets = assign.targets;
            copy.value = lower_ct_expr(*assign.value, env, bind, loc);
            dst.statements.push_back(ast::make_stmt_ptr(std::move(copy), stmt.loc));
            return;
        }
        if (stmt.type == ast::stmt_type::If)
        {
            const auto &ifs = ast::as_stmt<ast::if_stmt>(stmt);
            const ct_value *cond = eval_ct_bool(*ifs.then_branch->condition, env);
            if (cond != nullptr && cond->tag == ct_value::kind::Bool)
            {
                if (cond->flag)
                {
                    lower_block(*ifs.then_branch->body, env, bind, dst, loc);
                }
                else if (ifs.else_branch)
                {
                    lower_block(**ifs.else_branch, env, bind, dst, loc);
                }
                return;
            }
            // Runtime `if`: keep structure, lower nested statements/expressions.
            ast::if_stmt copy;
            copy.then_branch = std::make_unique<ast::if_branch>();
            copy.then_branch->hint = ifs.then_branch->hint;
            copy.then_branch->condition =
                lower_ct_expr(*ifs.then_branch->condition, env, bind, loc);
            copy.then_branch->body = std::make_unique<ast::block_stmt>();
            copy.then_branch->body->loc = ifs.then_branch->body->loc;
            lower_block(*ifs.then_branch->body, env, bind, *copy.then_branch->body,
                        loc);
            for (const auto &br : ifs.else_if_branches)
            {
                auto out_br = std::make_unique<ast::if_branch>();
                out_br->hint = br->hint;
                out_br->condition = lower_ct_expr(*br->condition, env, bind, loc);
                out_br->body = std::make_unique<ast::block_stmt>();
                out_br->body->loc = br->body->loc;
                lower_block(*br->body, env, bind, *out_br->body, loc);
                copy.else_if_branches.push_back(std::move(out_br));
            }
            if (ifs.else_branch)
            {
                auto else_body = std::make_unique<ast::block_stmt>();
                else_body->loc = (*ifs.else_branch)->loc;
                lower_block(**ifs.else_branch, env, bind, *else_body, loc);
                copy.else_branch = std::move(else_body);
            }
            dst.statements.push_back(ast::make_stmt_ptr(std::move(copy), stmt.loc));
            return;
        }
        if (stmt.type == ast::stmt_type::ReflectFor)
        {
            const auto &rf = ast::as_stmt<ast::reflect_for_stmt>(stmt);
            const ct_value *coll = eval_ct(*rf.collection, env);
            if (coll == nullptr)
            {
                fail_compile(stmt.loc.file + ':' + std::to_string(stmt.loc.line) +
                             ": error: ::reflect_for expects a reflection list");
            }
            if (coll->tag == ct_value::kind::Members)
            {
                for (const auto &field : coll->fields)
                {
                    auto nested = env;
                    ct_value item;
                    item.tag = ct_value::kind::Field;
                    item.field = field;
                    nested[rf.item_name] = std::move(item);
                    const field_info *previous_field = current_field_;
                    current_field_ = &field;
                    lower_block(*rf.body, nested, bind, dst, loc);
                    current_field_ = previous_field;
                }
                return;
            }
            if (coll->tag == ct_value::kind::MetaParams)
            {
                for (const auto &name : coll->meta_names)
                {
                    auto nested = env;
                    ct_value item;
                    item.tag = ct_value::kind::MetaParam;
                    item.param.name = name;
                    item.param.type_name = name;
                    nested[rf.item_name] = std::move(item);
                    lower_block(*rf.body, nested, bind, dst, loc);
                }
                return;
            }
            if (coll->tag == ct_value::kind::FuncParams)
            {
                for (const auto &param : coll->params)
                {
                    auto nested = env;
                    ct_value item;
                    item.tag = ct_value::kind::FuncParam;
                    item.param = param;
                    nested[rf.item_name] = std::move(item);
                    lower_block(*rf.body, nested, bind, dst, loc);
                }
                return;
            }
            if (coll->tag == ct_value::kind::FuncMembers)
            {
                for (const auto &fname : coll->func_names)
                {
                    auto nested = env;
                    ct_value item;
                    item.tag = ct_value::kind::FuncArtifact;
                    item.func_name = fname;
                    item.package_name = coll->package_name;
                    const func_info *info = lookup_func(item);
                    if (info != nullptr)
                    {
                        item.flag = info->is_meta;
                        item.package_name = info->package_name;
                    }
                    nested[rf.item_name] = std::move(item);
                    lower_block(*rf.body, nested, bind, dst, loc);
                }
                return;
            }
            fail_compile(
                stmt.loc.file + ':' + std::to_string(stmt.loc.line) +
                ": error: ::reflect_for expects fields, function_members, "
                "meta_params, or params");
            return;
        }
        if (stmt.type == ast::stmt_type::TypeidMatch)
        {
            const auto &tm = ast::as_stmt<ast::typeid_match_stmt>(stmt);
            const ct_value *scrut = eval_ct(*tm.scrutinee, env);
            std::string kind;
            if (scrut != nullptr && scrut->tag == ct_value::kind::Field)
            {
                kind = scrut->field.kind;
            }
            else
            {
                fail_compile(stmt.loc.file + ':' + std::to_string(stmt.loc.line) +
                             ": error: ::match scrutinee must be a field .type");
            }
            const ast::typeid_match_case *chosen = nullptr;
            const ast::typeid_match_case *fallback = nullptr;
            for (const auto &arm : tm.cases)
            {
                if (arm.is_default)
                {
                    fallback = &arm;
                    continue;
                }
                if (arm.type_kind_name == kind)
                {
                    chosen = &arm;
                    break;
                }
            }
            if (chosen == nullptr)
            {
                chosen = fallback;
            }
            if (chosen == nullptr || !chosen->body)
            {
                return;
            }
            auto body = lower_ct_expr(*chosen->body, env, bind, loc);
            dst.statements.push_back(ast::make_stmt_ptr(
                ast::expr_stmt{std::move(body)}, stmt.loc));
            return;
        }
        if (stmt.type == ast::stmt_type::Expr)
        {
            auto body = lower_ct_expr(
                *ast::as_stmt<ast::expr_stmt>(stmt).expression, env, bind, loc);
            dst.statements.push_back(ast::make_stmt_ptr(
                ast::expr_stmt{std::move(body)}, stmt.loc));
            return;
        }
        if (stmt.type == ast::stmt_type::Block)
        {
            lower_block(ast::as_stmt<ast::block_stmt>(stmt), env, bind, dst, loc);
            return;
        }
        if (stmt.type == ast::stmt_type::Return)
        {
            const auto &ret = ast::as_stmt<ast::return_stmt>(stmt);
            ast::return_stmt copy;
            if (ret.value.has_value())
            {
                copy.value = lower_ct_expr(**ret.value, env, bind, loc);
            }
            dst.statements.push_back(ast::make_stmt_ptr(std::move(copy), stmt.loc));
            return;
        }
        if (stmt.type == ast::stmt_type::Monitor)
        {
            const auto &mon = ast::as_stmt<ast::monitor_stmt>(stmt);
            ast::monitor_stmt copy;
            copy.trap_name = mon.trap_name;
            copy.trap_type = clone_type(*mon.trap_type);
            copy.protected_block = std::make_unique<ast::block_stmt>();
            copy.protected_block->loc = mon.protected_block->loc;
            lower_block(*mon.protected_block, env, bind, *copy.protected_block, loc);
            copy.handler = std::make_unique<ast::block_stmt>();
            copy.handler->loc = mon.handler->loc;
            lower_block(*mon.handler, env, bind, *copy.handler, loc);
            dst.statements.push_back(ast::make_stmt_ptr(std::move(copy), stmt.loc));
            return;
        }
        if (stmt.type == ast::stmt_type::Loop)
        {
            const auto &loop = ast::as_stmt<ast::loop_stmt>(stmt);
            ast::loop_stmt copy;
            if (loop.condition.has_value())
            {
                copy.condition = lower_ct_expr(**loop.condition, env, bind, loc);
            }
            copy.body = std::make_unique<ast::block_stmt>();
            copy.body->loc = loop.body->loc;
            lower_block(*loop.body, env, bind, *copy.body, loc);
            dst.statements.push_back(ast::make_stmt_ptr(std::move(copy), stmt.loc));
            return;
        }
        if (stmt.type == ast::stmt_type::Break)
        {
            dst.statements.push_back(
                ast::make_stmt_ptr(ast::break_stmt{}, stmt.loc));
            return;
        }
        if (stmt.type == ast::stmt_type::Insert)
        {
            const auto &insert = ast::as_stmt<ast::insert_stmt>(stmt);
            ast::insert_stmt copy;
            copy.receiver = insert.receiver;
            copy.map_expr = lower_ct_expr(*insert.map_expr, env, bind, loc);
            copy.entries = lower_ct_expr(*insert.entries, env, bind, loc);
            dst.statements.push_back(ast::make_stmt_ptr(std::move(copy), stmt.loc));
            return;
        }
        if (stmt.type == ast::stmt_type::Match)
        {
            const auto &match = ast::as_stmt<ast::match_stmt>(stmt);
            ast::match_stmt copy;
            copy.scrutinee = lower_ct_expr(*match.scrutinee, env, bind, loc);
            for (const auto &arm : match.cases)
            {
                ast::match_case out_arm;
                out_arm.loc = arm.loc;
                out_arm.enum_name = arm.enum_name;
                out_arm.member = arm.member;
                out_arm.body = std::make_unique<ast::block_stmt>();
                out_arm.body->loc = arm.body->loc;
                lower_block(*arm.body, env, bind, *out_arm.body, loc);
                copy.cases.push_back(std::move(out_arm));
            }
            dst.statements.push_back(ast::make_stmt_ptr(std::move(copy), stmt.loc));
            return;
        }
        if (stmt.type == ast::stmt_type::Acquire)
        {
            dst.statements.push_back(ast::make_stmt_ptr(
                ast::acquire_stmt{ast::as_stmt<ast::acquire_stmt>(stmt).lock_name},
                stmt.loc));
            return;
        }
        if (stmt.type == ast::stmt_type::Release)
        {
            dst.statements.push_back(ast::make_stmt_ptr(
                ast::release_stmt{ast::as_stmt<ast::release_stmt>(stmt).lock_name},
                stmt.loc));
            return;
        }
        fail_compile(stmt.loc.file + ':' + std::to_string(stmt.loc.line) +
                     ": error: unsupported statement inside generic/reflexpr body");
    }

    ct_value make_reflexpr_value(const ast::expr_node &arg,
                                 const std::unordered_map<std::string, ct_value> &env,
                                 const ast::source_loc &loc) const
    {
        if (arg.type == ast::expr_type::Identifier)
        {
            const std::string &n = ast::as<ast::identifier>(arg).name;
            if (n == "this_package")
            {
                ct_value art;
                art.tag = ct_value::kind::PackageArtifact;
                art.package_name = program_.package_name;
                return art;
            }
            if (n == program_.package_name)
            {
                fail_compile(loc.file + ':' + std::to_string(loc.line) +
                             ": error: ::reflexpr on the current package requires "
                             "`this_package`, not the package name");
            }
            if (imported_packages_.contains(n) || packages_.contains(n))
            {
                if (!packages_.contains(n))
                {
                    fail_compile(loc.file + ':' + std::to_string(loc.line) +
                                 ": error: imported package `" + n +
                                 "` could not be loaded for reflection");
                }
                ct_value art;
                art.tag = ct_value::kind::PackageArtifact;
                art.package_name = n;
                return art;
            }
            if (functions_.contains(n))
            {
                const func_info &fn = functions_.at(n);
                ct_value art;
                art.tag = ct_value::kind::FuncArtifact;
                art.func_name = fn.name;
                art.package_name = fn.package_name;
                art.flag = fn.is_meta;
                return art;
            }
            const auto ct = env.find(n);
            if (ct != env.end() && ct->second.tag == ct_value::kind::TypeName)
            {
                ct_value art;
                art.tag = ct_value::kind::ObjectArtifact;
                art.type_name = ct->second.type_name;
                return art;
            }
            const auto lt = local_types_.find(n);
            if (lt != local_types_.end())
            {
                ct_value art;
                art.tag = ct_value::kind::ObjectArtifact;
                art.type_name = lt->second;
                return art;
            }
            if (objects_.contains(n))
            {
                ct_value art;
                art.tag = ct_value::kind::ObjectArtifact;
                art.type_name = n;
                return art;
            }
        }
        const std::string obj = infer_expr_type(arg);
        if (!obj.empty() && objects_.contains(obj))
        {
            ct_value art;
            art.tag = ct_value::kind::ObjectArtifact;
            art.type_name = obj;
            return art;
        }
        fail_compile(loc.file + ':' + std::to_string(loc.line) +
                     ": error: ::reflexpr requires an object, function, "
                     "`this_package`, or imported package");
        return {};
    }

    const ct_value *eval_ct(const ast::expr_node &expr,
                            const std::unordered_map<std::string, ct_value> &env) const
    {
        if (expr.type == ast::expr_type::Identifier)
        {
            const auto found = env.find(ast::as<ast::identifier>(expr).name);
            if (found != env.end())
            {
                return &found->second;
            }
            return nullptr;
        }
        if (expr.type == ast::expr_type::Member)
        {
            const auto &mem = ast::as<ast::member_expr>(expr);
            const ct_value *base = eval_ct(*mem.object, env);
            if (base == nullptr)
            {
                return nullptr;
            }
            if (mem.member == "type" && base->tag == ct_value::kind::Field)
            {
                return base;
            }
            if (base->tag == ct_value::kind::FuncArtifact)
            {
                if (mem.member == "is_meta_function")
                {
                    // Materialize into thread-local storage for pointer stability.
                    static thread_local ct_value bool_scratch;
                    bool_scratch.tag = ct_value::kind::Bool;
                    bool_scratch.flag = base->flag;
                    return &bool_scratch;
                }
                if (mem.member == "params" || mem.member == "meta_params")
                {
                    const func_info *info = lookup_func(*base);
                    if (info == nullptr)
                    {
                        return nullptr;
                    }
                    static thread_local ct_value list_scratch;
                    if (mem.member == "params")
                    {
                        list_scratch.tag = ct_value::kind::FuncParams;
                        list_scratch.params = info->params;
                        list_scratch.meta_names.clear();
                    }
                    else
                    {
                        list_scratch.tag = ct_value::kind::MetaParams;
                        list_scratch.meta_names = info->type_params;
                        list_scratch.params.clear();
                    }
                    return &list_scratch;
                }
            }
            return base;
        }
        return nullptr;
    }

    const ct_value *eval_ct_bool(const ast::expr_node &expr,
                                 const std::unordered_map<std::string, ct_value> &env) const
    {
        if (expr.type == ast::expr_type::Member)
        {
            const auto &mem = ast::as<ast::member_expr>(expr);
            if (mem.member == "is_meta_function")
            {
                const ct_value *base = eval_ct(*mem.object, env);
                if (base != nullptr && base->tag == ct_value::kind::FuncArtifact)
                {
                    static thread_local ct_value bool_scratch;
                    bool_scratch.tag = ct_value::kind::Bool;
                    bool_scratch.flag = base->flag;
                    return &bool_scratch;
                }
            }
        }
        return eval_ct(expr, env);
    }

    static std::unique_ptr<ast::type_node> make_named_type(const std::string &name,
                                                           const ast::source_loc &loc)
    {
        auto type = std::make_unique<ast::type_node>();
        *type = ast::type_node::make_named(name, loc);
        return type;
    }

    static std::unique_ptr<ast::expr_node>
    make_call(const std::string &name,
              std::vector<std::unique_ptr<ast::expr_node>> args,
              const ast::source_loc &loc)
    {
        ast::call_expr call;
        call.callee = make_ident(name, loc);
        call.arguments = std::move(args);
        return ast::make_expr_ptr(std::move(call), loc);
    }

    std::string resolve_type_arg_name(
        const ast::expr_node &type_arg,
        const std::unordered_map<std::string, ct_value> &env,
        const std::unordered_map<std::string, std::string> &bind,
        const ast::source_loc &loc) const
    {
        if (type_arg.type == ast::expr_type::Identifier)
        {
            const std::string &n = ast::as<ast::identifier>(type_arg).name;
            const auto bound = bind.find(n);
            if (bound != bind.end())
            {
                return bound->second;
            }
            const auto ct = env.find(n);
            if (ct != env.end() &&
                (ct->second.tag == ct_value::kind::TypeName ||
                 ct->second.tag == ct_value::kind::ObjectArtifact))
            {
                return ct->second.type_name;
            }
            if (objects_.contains(n))
            {
                return n;
            }
        }
        fail_compile(loc.file + ':' + std::to_string(loc.line) +
                     ": error: expected an object type name");
        return {};
    }

    std::unique_ptr<ast::expr_node>
    lower_ct_expr(const ast::expr_node &expr,
                  const std::unordered_map<std::string, ct_value> &env,
                  const std::unordered_map<std::string, std::string> &bind,
                  const ast::source_loc &loc)
    {
        if (expr.type == ast::expr_type::Member)
        {
            const auto &mem = ast::as<ast::member_expr>(expr);
            const ct_value *base = eval_ct(*mem.object, env);
            if (base != nullptr)
            {
                if (base->tag == ct_value::kind::Field)
                {
                    if (mem.member == "name")
                    {
                        return make_string(base->field.name, expr.loc);
                    }
                    if (mem.member == "type_name")
                    {
                        return make_string(base->field.type_name, expr.loc);
                    }
                }
                if (base->tag == ct_value::kind::FuncArtifact)
                {
                    if (mem.member == "name")
                    {
                        return make_string(base->func_name, expr.loc);
                    }
                    if (mem.member == "package_name")
                    {
                        return make_string(base->package_name, expr.loc);
                    }
                    if (mem.member == "is_meta_function")
                    {
                        return ast::make_expr_ptr(ast::bool_literal{base->flag}, expr.loc);
                    }
                    if (mem.member == "return_type")
                    {
                        const func_info *info = lookup_func(*base);
                        return make_string(
                            info != nullptr ? info->return_type_name : std::string{},
                            expr.loc);
                    }
                    if (mem.member == "params")
                    {
                        const func_info *info = lookup_func(*base);
                        return make_string(
                            info != nullptr ? stringify_params(info->params)
                                            : std::string{"[]"},
                            expr.loc);
                    }
                    if (mem.member == "meta_params")
                    {
                        const func_info *info = lookup_func(*base);
                        return make_string(
                            info != nullptr ? stringify_meta_params(info->type_params)
                                            : std::string{"[]"},
                            expr.loc);
                    }
                }
                if (base->tag == ct_value::kind::MetaParam ||
                    base->tag == ct_value::kind::FuncParam)
                {
                    if (mem.member == "name")
                    {
                        return make_string(base->param.name, expr.loc);
                    }
                    if (mem.member == "type")
                    {
                        return make_string(base->param.type_name, expr.loc);
                    }
                }
            }
        }
        if (expr.type == ast::expr_type::Call)
        {
            const auto &call = ast::as<ast::call_expr>(expr);
            const std::string cal = callee_name(*call.callee);
            if (cal == "::construct")
            {
                if (call.arguments.size() != 2)
                {
                    fail_compile(expr.loc.file + ':' + std::to_string(expr.loc.line) +
                                 ": error: ::construct expects (Type, args_array)");
                }
                const std::string type_name =
                    resolve_type_arg_name(*call.arguments[0], env, bind, expr.loc);
                if (!objects_.contains(type_name))
                {
                    fail_compile(expr.loc.file + ':' + std::to_string(expr.loc.line) +
                                 ": error: ::construct requires an object type");
                }
                if (call.arguments[1]->type != ast::expr_type::Identifier)
                {
                    fail_compile(expr.loc.file + ':' + std::to_string(expr.loc.line) +
                                 ": error: ::construct args must be a local array name");
                }
                const std::string args_name =
                    ast::as<ast::identifier>(*call.arguments[1]).name;
                const size_t n = objects_.at(type_name).fields.size();
                ast::call_expr ctor;
                ctor.callee = make_ident(type_name, expr.loc);
                for (size_t i = 0; i < n; ++i)
                {
                    ast::index_expr idx;
                    idx.object = make_ident(args_name, expr.loc);
                    idx.index = ast::make_expr_ptr(
                        ast::int_literal{static_cast<long long>(i)}, expr.loc);
                    ctor.arguments.push_back(
                        ast::make_expr_ptr(std::move(idx), expr.loc));
                }
                return ast::make_expr_ptr(std::move(ctor), expr.loc);
            }
            ast::call_expr out;
            out.callee = lower_ct_expr(*call.callee, env, bind, loc);
            for (const auto &ta : call.type_arguments)
            {
                out.type_arguments.push_back(clone_type(*ta));
            }
            for (const auto &arg : call.arguments)
            {
                out.arguments.push_back(lower_ct_expr(*arg, env, bind, loc));
            }
            // Nested generics (e.g. from_json<T>(…) or from_json(value) in reflect_for).
            if (generics_.contains(cal))
            {
                out.callee = make_ident(cal, expr.loc);
                specialize_generic_call(out, expr.loc, &bind);
            }
            return ast::make_expr_ptr(std::move(out), expr.loc);
        }
        if (expr.type == ast::expr_type::Member)
        {
            const auto &mem = ast::as<ast::member_expr>(expr);
            // Non-CT member access (e.g. p.name after decode): keep as runtime.
            ast::member_expr out;
            out.object = lower_ct_expr(*mem.object, env, bind, loc);
            out.member = mem.member;
            return ast::make_expr_ptr(std::move(out), expr.loc);
        }
        if (expr.type == ast::expr_type::Binary)
        {
            const auto &bin = ast::as<ast::binary_expr>(expr);
            ast::binary_expr out;
            out.op = bin.op;
            out.left = lower_ct_expr(*bin.left, env, bind, loc);
            out.right = lower_ct_expr(*bin.right, env, bind, loc);
            return ast::make_expr_ptr(std::move(out), expr.loc);
        }
        if (expr.type == ast::expr_type::Unary)
        {
            const auto &un = ast::as<ast::unary_expr>(expr);
            ast::unary_expr out;
            out.op = un.op;
            out.operand = lower_ct_expr(*un.operand, env, bind, loc);
            return ast::make_expr_ptr(std::move(out), expr.loc);
        }
        if (expr.type == ast::expr_type::Index)
        {
            const auto &idx = ast::as<ast::index_expr>(expr);
            ast::index_expr out;
            out.object = lower_ct_expr(*idx.object, env, bind, loc);
            out.index = lower_ct_expr(*idx.index, env, bind, loc);
            return ast::make_expr_ptr(std::move(out), expr.loc);
        }
        if (expr.type == ast::expr_type::Cast)
        {
            const auto &cast = ast::as<ast::cast_expr>(expr);
            ast::cast_expr out;
            out.target_type = clone_type(*cast.target_type);
            out.operand = lower_ct_expr(*cast.operand, env, bind, loc);
            return ast::make_expr_ptr(std::move(out), expr.loc);
        }
        if (expr.type == ast::expr_type::EnumAccess)
        {
            const auto &access = ast::as<ast::enum_access_expr>(expr);
            return ast::make_expr_ptr(
                ast::enum_access_expr{access.enum_name, access.member}, expr.loc);
        }
        if (expr.type == ast::expr_type::Identifier)
        {
            return make_ident(ast::as<ast::identifier>(expr).name, expr.loc);
        }
        if (expr.type == ast::expr_type::StringLiteral)
        {
            return make_string(ast::as<ast::string_literal>(expr).value, expr.loc);
        }
        if (expr.type == ast::expr_type::IntLiteral)
        {
            return ast::make_expr_ptr(
                ast::int_literal{ast::as<ast::int_literal>(expr).value}, expr.loc);
        }
        if (expr.type == ast::expr_type::FloatLiteral)
        {
            return ast::make_expr_ptr(
                ast::float_literal{ast::as<ast::float_literal>(expr).value}, expr.loc);
        }
        if (expr.type == ast::expr_type::BoolLiteral)
        {
            return ast::make_expr_ptr(
                ast::bool_literal{ast::as<ast::bool_literal>(expr).value}, expr.loc);
        }
        if (expr.type == ast::expr_type::NullLiteral)
        {
            return ast::make_expr_ptr(ast::null_literal{}, expr.loc);
        }
        if (expr.type == ast::expr_type::CharLiteral)
        {
            return ast::make_expr_ptr(
                ast::char_literal{ast::as<ast::char_literal>(expr).value}, expr.loc);
        }
        if (expr.type == ast::expr_type::RegexLiteral)
        {
            return ast::make_expr_ptr(
                ast::regex_literal{ast::as<ast::regex_literal>(expr).pattern},
                expr.loc);
        }
        if (expr.type == ast::expr_type::ArrayLiteral)
        {
            ast::array_literal out;
            for (const auto &el : ast::as<ast::array_literal>(expr).elements)
            {
                out.elements.push_back(lower_ct_expr(*el, env, bind, loc));
            }
            return ast::make_expr_ptr(std::move(out), expr.loc);
        }
        if (expr.type == ast::expr_type::TypedArrayLiteral)
        {
            const auto &typed = ast::as<ast::typed_array_literal>(expr);
            ast::typed_array_literal out;
            out.element_type = clone_type(*typed.element_type);
            for (const auto &el : typed.elements)
            {
                out.elements.push_back(lower_ct_expr(*el, env, bind, loc));
            }
            return ast::make_expr_ptr(std::move(out), expr.loc);
        }
        if (expr.type == ast::expr_type::TupleLiteral)
        {
            ast::tuple_literal out;
            for (const auto &el : ast::as<ast::tuple_literal>(expr).elements)
            {
                out.elements.push_back(lower_ct_expr(*el, env, bind, loc));
            }
            return ast::make_expr_ptr(std::move(out), expr.loc);
        }
        if (expr.type == ast::expr_type::PipeInsert)
        {
            const auto &ins = ast::as<ast::pipe_insert_expr>(expr);
            return ast::make_expr_ptr(
                ast::pipe_insert_expr{lower_ct_expr(*ins.value, env, bind, loc),
                                      ins.pipe_name},
                expr.loc);
        }
        if (expr.type == ast::expr_type::PipeExtract)
        {
            return ast::make_expr_ptr(
                ast::pipe_extract_expr{
                    ast::as<ast::pipe_extract_expr>(expr).pipe_name},
                expr.loc);
        }
        if (expr.type == ast::expr_type::ChannelInsert)
        {
            const auto &ins = ast::as<ast::channel_insert_expr>(expr);
            return ast::make_expr_ptr(
                ast::channel_insert_expr{
                    lower_ct_expr(*ins.value, env, bind, loc), ins.channel_name},
                expr.loc);
        }
        if (expr.type == ast::expr_type::ChannelExtract)
        {
            return ast::make_expr_ptr(
                ast::channel_extract_expr{
                    ast::as<ast::channel_extract_expr>(expr).channel_name},
                expr.loc);
        }
        if (expr.type == ast::expr_type::Alloc)
        {
            const auto &alloc = ast::as<ast::alloc_expr>(expr);
            ast::alloc_expr out;
            out.capacity = lower_ct_expr(*alloc.capacity, env, bind, loc);
            for (const auto &init : alloc.initial_values)
            {
                out.initial_values.push_back(lower_ct_expr(*init, env, bind, loc));
            }
            return ast::make_expr_ptr(std::move(out), expr.loc);
        }
        if (expr.type == ast::expr_type::Free)
        {
            return ast::make_expr_ptr(
                ast::free_expr{ast::as<ast::free_expr>(expr).buffer_name},
                expr.loc);
        }
        if (expr.type == ast::expr_type::Simd)
        {
            const auto &simd = ast::as<ast::simd_expr>(expr);
            return ast::make_expr_ptr(
                ast::simd_expr{lower_ct_expr(*simd.operand, env, bind, loc)},
                expr.loc);
        }
        if (expr.type == ast::expr_type::Lambda)
        {
            const auto &lambda = ast::as<ast::lambda_expr>(expr);
            ast::lambda_expr out;
            for (const auto &param : lambda.parameters)
            {
                ast::parameter p;
                p.loc = param.loc;
                p.name = param.name;
                p.type = clone_type(*param.type);
                out.parameters.push_back(std::move(p));
            }
            out.return_type = clone_type(*lambda.return_type);
            out.body = std::make_unique<ast::block_stmt>();
            out.body->loc = lambda.body->loc;
            lower_block(*lambda.body, env, bind, *out.body, loc);
            return ast::make_expr_ptr(std::move(out), expr.loc);
        }
        if (expr.type == ast::expr_type::MapLiteral)
        {
            const auto &map = ast::as<ast::map_literal>(expr);
            ast::map_literal out;
            out.key_type = clone_type(*map.key_type);
            out.value_type = clone_type(*map.value_type);
            for (const auto &entry : map.entries)
            {
                ast::map_entry e;
                e.key = lower_ct_expr(*entry.key, env, bind, loc);
                e.value = lower_ct_expr(*entry.value, env, bind, loc);
                out.entries.push_back(std::move(e));
            }
            return ast::make_expr_ptr(std::move(out), expr.loc);
        }
        if (expr.type == ast::expr_type::MapEntriesLiteral)
        {
            const auto &map = ast::as<ast::map_entries_literal>(expr);
            ast::map_entries_literal out;
            for (const auto &entry : map.entries)
            {
                ast::map_entry e;
                e.key = lower_ct_expr(*entry.key, env, bind, loc);
                e.value = lower_ct_expr(*entry.value, env, bind, loc);
                out.entries.push_back(std::move(e));
            }
            return ast::make_expr_ptr(std::move(out), expr.loc);
        }
        fail_compile(loc.file + ':' + std::to_string(loc.line) +
                     ": error: unsupported expression in reflexpr lowering");
        return make_ident("null", loc);
    }
};

} // namespace detail

/// Monomorphize generic calls and lower compile-time reflexpr constructs in @p program.
inline void expand_generics_and_reflexpr(ast::program &program)
{
    detail::expander{program}.run();
}

} // namespace munx
