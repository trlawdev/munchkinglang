#pragma once

#include "ast.hpp"
#include <cstdio>
#include <iostream>
#include <string>

namespace munx
{

    /// Pretty-prints an AST as indented S-expressions with source locations.
    class ast_printer
    {
        std::ostream &out_;  ///< Destination stream.
        int indent_{0};      ///< Current indent in spaces.

    public:
        /// Write output to @p out (defaults to @c std::cout).
        explicit ast_printer(std::ostream &out = std::cout) : out_(out) {}

        /// Print the full @p program tree.
        void print(const ast::program &program)
        {
            write_line("(program");
            indent();
            write_line("(package %s%s)", program.package_name.c_str(),
                       at(program.package_loc).c_str());
            for (const auto &import : program.imports)
            {
                print_load_package(import);
            }
            for (const auto &stmt : program.statements)
            {
                print_stmt(*stmt);
            }
            dedent();
            write_line(")");
        }

    private:
        /// Increase indentation by two spaces.
        void indent() { indent_ += 2; }
        /// Decrease indentation by two spaces.
        void dedent() { indent_ -= 2; }

        /// @return A string of spaces for the current indent level.
        std::string pad() const { return std::string(static_cast<std::size_t>(indent_), ' '); }

        /// Format @p loc as ` @file:line:col`, or empty if unset.
        static std::string at(const ast::source_loc &loc)
        {
            if (loc.line == 0 && loc.column == 0 && loc.file.empty())
            {
                return {};
            }
            std::string out{" @"};
            if (!loc.file.empty())
            {
                out += loc.file;
                out += ':';
            }
            out += std::to_string(loc.line);
            out += ':';
            out += std::to_string(loc.column);
            return out;
        }

        /// Write one indented line; @p fmt is printf-style when args are given.
        template <typename... Args>
        void write_line(const char *fmt, Args &&...args)
        {
            out_ << pad();
            if constexpr (sizeof...(Args) == 0)
            {
                out_ << fmt;
            }
            else
            {
                char buffer[1024];
                std::snprintf(buffer, sizeof(buffer), fmt, std::forward<Args>(args)...);
                out_ << buffer;
            }
            out_ << '\n';
        }

        /// @return Source spelling of primitive @p kind.
        static const char *primitive_name(ast::primitive_kind kind)
        {
            switch (kind)
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
        }

        /// @return Symbolic name of binary operator @p op.
        static const char *binary_op_name(ast::binary_op op)
        {
            switch (op)
            {
            case ast::binary_op::Add:
                return "+";
            case ast::binary_op::Sub:
                return "-";
            case ast::binary_op::Mul:
                return "*";
            case ast::binary_op::Div:
                return "/";
            case ast::binary_op::Mod:
                return "%";
            case ast::binary_op::Eq:
                return "==";
            case ast::binary_op::Ne:
                return "!=";
            case ast::binary_op::Lt:
                return "<";
            case ast::binary_op::Gt:
                return ">";
            case ast::binary_op::Le:
                return "<=";
            case ast::binary_op::Ge:
                return ">=";
            case ast::binary_op::And:
                return "&&";
            case ast::binary_op::Or:
                return "||";
            case ast::binary_op::BitwiseAnd:
                return "&";
            case ast::binary_op::BitwiseOr:
                return "|";
            case ast::binary_op::BitwiseXor:
                return "^";
            }
            return "?";
        }

        /// Print a type node.
        void print_type(const ast::type_node &type)
        {
            const auto loc = at(type.loc);
            switch (type.type)
            {
            case ast::type_kind::Primitive:
                write_line("(type primitive %s%s)",
                           primitive_name(std::get<ast::primitive_type>(type.value).kind),
                           loc.c_str());
                break;
            case ast::type_kind::Named:
                write_line("(type named %s%s)",
                           std::get<ast::named_type>(type.value).name.c_str(), loc.c_str());
                break;
            case ast::type_kind::Array:
                write_line("(type array%s", loc.c_str());
                indent();
                print_type(*std::get<ast::array_type>(type.value).element);
                dedent();
                write_line(")");
                break;
            case ast::type_kind::Tuple:
                write_line("(type tuple%s", loc.c_str());
                indent();
                for (const auto &element : std::get<ast::tuple_type>(type.value).elements)
                {
                    print_type(*element);
                }
                dedent();
                write_line(")");
                break;
            case ast::type_kind::Map:
                write_line("(type map%s", loc.c_str());
                indent();
                print_type(*std::get<ast::map_type>(type.value).key);
                print_type(*std::get<ast::map_type>(type.value).value);
                dedent();
                write_line(")");
                break;
            case ast::type_kind::Lambda:
                write_line("(type lambda%s", loc.c_str());
                indent();
                for (const auto &param :
                     std::get<ast::lambda_type>(type.value).params)
                {
                    print_type(*param);
                }
                print_type(*std::get<ast::lambda_type>(type.value).ret);
                dedent();
                write_line(")");
                break;
            }
        }

        /// Print an expression node.
        void print_expr(const ast::expr_node &expr)
        {
            const auto loc = at(expr.loc);
            switch (expr.type)
            {
            case ast::expr_type::IntLiteral:
                write_line("(int %lld%s)", ast::as<ast::int_literal>(expr).value, loc.c_str());
                break;
            case ast::expr_type::FloatLiteral:
                write_line("(float %Lf%s)", ast::as<ast::float_literal>(expr).value, loc.c_str());
                break;
            case ast::expr_type::StringLiteral:
                write_line("(string \"%s\"%s)", ast::as<ast::string_literal>(expr).value.c_str(),
                           loc.c_str());
                break;
            case ast::expr_type::CharLiteral:
                write_line("(char '%c'%s)", ast::as<ast::char_literal>(expr).value, loc.c_str());
                break;
            case ast::expr_type::BoolLiteral:
                write_line("(bool %s%s)",
                           ast::as<ast::bool_literal>(expr).value ? "true" : "false", loc.c_str());
                break;
            case ast::expr_type::NullLiteral:
                write_line("(null%s)", loc.c_str());
                break;
            case ast::expr_type::RegexLiteral:
                write_line("(regex \"%s\"%s)", ast::as<ast::regex_literal>(expr).pattern.c_str(),
                           loc.c_str());
                break;
            case ast::expr_type::Identifier:
                write_line("(ident %s%s)", ast::as<ast::identifier>(expr).name.c_str(),
                           loc.c_str());
                break;
            case ast::expr_type::Binary:
            {
                const auto &binary = ast::as<ast::binary_expr>(expr);
                write_line("(binary %s%s", binary_op_name(binary.op), loc.c_str());
                indent();
                print_expr(*binary.left);
                print_expr(*binary.right);
                dedent();
                write_line(")");
                break;
            }
            case ast::expr_type::Unary:
            {
                const auto &unary = ast::as<ast::unary_expr>(expr);
                write_line("(unary%s", loc.c_str());
                indent();
                print_expr(*unary.operand);
                dedent();
                write_line(")");
                break;
            }
            case ast::expr_type::Call:
            {
                const auto &call = ast::as<ast::call_expr>(expr);
                write_line("(call%s", loc.c_str());
                indent();
                print_expr(*call.callee);
                for (const auto &arg : call.arguments)
                {
                    print_expr(*arg);
                }
                dedent();
                write_line(")");
                break;
            }
            case ast::expr_type::Member:
            {
                const auto &member = ast::as<ast::member_expr>(expr);
                write_line("(member %s%s", member.member.c_str(), loc.c_str());
                indent();
                print_expr(*member.object);
                dedent();
                write_line(")");
                break;
            }
            case ast::expr_type::EnumAccess:
            {
                const auto &access = ast::as<ast::enum_access_expr>(expr);
                write_line("(enum-access %s::%s%s)", access.enum_name.c_str(),
                           access.member.c_str(), loc.c_str());
                break;
            }
            case ast::expr_type::Index:
            {
                const auto &index = ast::as<ast::index_expr>(expr);
                write_line("(index%s", loc.c_str());
                indent();
                print_expr(*index.object);
                print_expr(*index.index);
                dedent();
                write_line(")");
                break;
            }
            case ast::expr_type::ArrayLiteral:
            {
                const auto &array = ast::as<ast::array_literal>(expr);
                write_line("(array%s", loc.c_str());
                indent();
                for (const auto &element : array.elements)
                {
                    print_expr(*element);
                }
                dedent();
                write_line(")");
                break;
            }
            case ast::expr_type::TypedArrayLiteral:
            {
                const auto &typed_array = ast::as<ast::typed_array_literal>(expr);
                write_line("(typed-array%s", loc.c_str());
                indent();
                print_type(*typed_array.element_type);
                for (const auto &element : typed_array.elements)
                {
                    print_expr(*element);
                }
                dedent();
                write_line(")");
                break;
            }
            case ast::expr_type::TupleLiteral:
            {
                const auto &tuple = ast::as<ast::tuple_literal>(expr);
                write_line("(tuple%s", loc.c_str());
                indent();
                for (const auto &element : tuple.elements)
                {
                    print_expr(*element);
                }
                dedent();
                write_line(")");
                break;
            }
            case ast::expr_type::PipeInsert:
            {
                const auto &pipe_insert = ast::as<ast::pipe_insert_expr>(expr);
                write_line("(pipe-insert %s%s", pipe_insert.pipe_name.c_str(), loc.c_str());
                indent();
                print_expr(*pipe_insert.value);
                dedent();
                write_line(")");
                break;
            }
            case ast::expr_type::PipeExtract:
                write_line("(pipe-extract %s%s)",
                           ast::as<ast::pipe_extract_expr>(expr).pipe_name.c_str(), loc.c_str());
                break;
            case ast::expr_type::ChannelInsert:
            {
                const auto &channel_insert = ast::as<ast::channel_insert_expr>(expr);
                write_line("(channel-insert %s%s", channel_insert.channel_name.c_str(),
                           loc.c_str());
                indent();
                print_expr(*channel_insert.value);
                dedent();
                write_line(")");
                break;
            }
            case ast::expr_type::ChannelExtract:
                write_line("(channel-extract %s%s)",
                           ast::as<ast::channel_extract_expr>(expr).channel_name.c_str(),
                           loc.c_str());
                break;
            case ast::expr_type::Cast:
            {
                const auto &cast = ast::as<ast::cast_expr>(expr);
                write_line("(cast%s", loc.c_str());
                indent();
                print_type(*cast.target_type);
                print_expr(*cast.operand);
                dedent();
                write_line(")");
                break;
            }
            case ast::expr_type::Alloc:
            {
                const auto &alloc = ast::as<ast::alloc_expr>(expr);
                write_line("(alloc%s", loc.c_str());
                indent();
                print_expr(*alloc.capacity);
                for (const auto &init : alloc.initial_values)
                {
                    print_expr(*init);
                }
                dedent();
                write_line(")");
                break;
            }
            case ast::expr_type::Free:
                write_line("(free %s%s)", ast::as<ast::free_expr>(expr).buffer_name.c_str(),
                           loc.c_str());
                break;
            case ast::expr_type::Simd:
            {
                const auto &simd = ast::as<ast::simd_expr>(expr);
                write_line("(simd%s", loc.c_str());
                indent();
                print_expr(*simd.operand);
                dedent();
                write_line(")");
                break;
            }
            case ast::expr_type::Lambda:
            {
                const auto &lambda = ast::as<ast::lambda_expr>(expr);
                write_line("(lambda%s", loc.c_str());
                indent();
                for (const auto &param : lambda.parameters)
                {
                    write_line("(param %s%s", param.name.c_str(), at(param.loc).c_str());
                    indent();
                    print_type(*param.type);
                    dedent();
                    write_line(")");
                }
                print_type(*lambda.return_type);
                print_block(*lambda.body);
                dedent();
                write_line(")");
                break;
            }
            case ast::expr_type::MapLiteral:
            {
                const auto &literal = ast::as<ast::map_literal>(expr);
                write_line("(map-literal%s", loc.c_str());
                indent();
                print_type(*literal.key_type);
                print_type(*literal.value_type);
                for (const auto &entry : literal.entries)
                {
                    print_expr(*entry.key);
                    write_line(":");
                    print_expr(*entry.value);
                }
                dedent();
                write_line(")");
                break;
            }
            case ast::expr_type::MapEntriesLiteral:
            {
                const auto &entries = ast::as<ast::map_entries_literal>(expr);
                write_line("(map-entries%s", loc.c_str());
                indent();
                for (const auto &entry : entries.entries)
                {
                    print_expr(*entry.key);
                    write_line(":");
                    print_expr(*entry.value);
                }
                dedent();
                write_line(")");
                break;
            }
            }
        }

        /// Print a block statement.
        void print_block(const ast::block_stmt &block)
        {
            write_line("(block%s", at(block.loc).c_str());
            indent();
            for (const auto &stmt : block.statements)
            {
                print_stmt(*stmt);
            }
            dedent();
            write_line(")");
        }

        /// Print assignment / destructure bind targets.
        void print_bind_targets(const std::vector<ast::bind_target> &targets)
        {
            write_line("(targets");
            indent();
            for (const auto &target : targets)
            {
                if (target.is_discard)
                {
                    write_line("(discard _%s)", at(target.loc).c_str());
                }
                else
                {
                    write_line("(target %s%s)", target.name.c_str(), at(target.loc).c_str());
                }
            }
            dedent();
            write_line(")");
        }

        /// Print a load-package import.
        void print_load_package(const ast::load_package_stmt &stmt)
        {
            write_line("(load-package %s%s)", stmt.package.c_str(), at(stmt.loc).c_str());
        }

        /// Print a statement node.
        void print_stmt(const ast::stmt_node &stmt)
        {
            const auto loc = at(stmt.loc);
            switch (stmt.type)
            {
            case ast::stmt_type::Assignment:
            {
                const auto &assignment = ast::as_stmt<ast::assignment_stmt>(stmt);
                write_line("(assign %s%s",
                           assignment.op == ast::assign_op::AddAssign ? "+=" : "=", loc.c_str());
                indent();
                print_bind_targets(assignment.targets);
                print_expr(*assignment.value);
                dedent();
                write_line(")");
                break;
            }
            case ast::stmt_type::Expr:
            {
                const auto &expr = ast::as_stmt<ast::expr_stmt>(stmt);
                write_line("(expr%s", loc.c_str());
                indent();
                print_expr(*expr.expression);
                dedent();
                write_line(")");
                break;
            }
            case ast::stmt_type::Return:
            {
                const auto &ret = ast::as_stmt<ast::return_stmt>(stmt);
                write_line("(return%s", loc.c_str());
                if (ret.value)
                {
                    indent();
                    print_expr(**ret.value);
                    dedent();
                }
                write_line(")");
                break;
            }
            case ast::stmt_type::Break:
                write_line("(break%s)", loc.c_str());
                break;
            case ast::stmt_type::Block:
                print_block(ast::as_stmt<ast::block_stmt>(stmt));
                break;
            case ast::stmt_type::If:
            {
                const auto &if_stmt = ast::as_stmt<ast::if_stmt>(stmt);
                write_line("(if%s", loc.c_str());
                indent();
                write_line("(then");
                indent();
                if (if_stmt.then_branch->hint == ast::branch_hint::Likely)
                {
                    write_line("(likely)");
                }
                else if (if_stmt.then_branch->hint == ast::branch_hint::Unlikely)
                {
                    write_line("(unlikely)");
                }
                print_expr(*if_stmt.then_branch->condition);
                print_block(*if_stmt.then_branch->body);
                dedent();
                write_line(")");
                for (const auto &branch : if_stmt.else_if_branches)
                {
                    write_line("(else-if");
                    indent();
                    if (branch->hint == ast::branch_hint::Likely)
                    {
                        write_line("(likely)");
                    }
                    else if (branch->hint == ast::branch_hint::Unlikely)
                    {
                        write_line("(unlikely)");
                    }
                    print_expr(*branch->condition);
                    print_block(*branch->body);
                    dedent();
                    write_line(")");
                }
                if (if_stmt.else_branch)
                {
                    write_line("(else");
                    indent();
                    print_block(**if_stmt.else_branch);
                    dedent();
                    write_line(")");
                }
                dedent();
                write_line(")");
                break;
            }
            case ast::stmt_type::Loop:
            {
                const auto &loop = ast::as_stmt<ast::loop_stmt>(stmt);
                write_line("(loop%s", loc.c_str());
                indent();
                if (loop.condition)
                {
                    print_expr(**loop.condition);
                }
                print_block(*loop.body);
                dedent();
                write_line(")");
                break;
            }
            case ast::stmt_type::Match:
            {
                const auto &match = ast::as_stmt<ast::match_stmt>(stmt);
                write_line("(match%s", loc.c_str());
                indent();
                print_expr(*match.scrutinee);
                for (const auto &case_arm : match.cases)
                {
                    write_line("(case %s::%s%s", case_arm.enum_name.c_str(),
                               case_arm.member.c_str(), at(case_arm.loc).c_str());
                    indent();
                    print_block(*case_arm.body);
                    dedent();
                    write_line(")");
                }
                dedent();
                write_line(")");
                break;
            }
            case ast::stmt_type::FuncDecl:
            {
                const auto &func = ast::as_stmt<ast::func_decl>(stmt);
                write_line("(func %s%s", func.name.c_str(), loc.c_str());
                indent();
                for (const auto &param : func.parameters)
                {
                    write_line("(param %s%s", param.name.c_str(), at(param.loc).c_str());
                    indent();
                    print_type(*param.type);
                    dedent();
                    write_line(")");
                }
                print_type(*func.return_type);
                print_block(*func.body);
                dedent();
                write_line(")");
                break;
            }
            case ast::stmt_type::EnumDecl:
            {
                const auto &enum_decl = ast::as_stmt<ast::enum_decl>(stmt);
                write_line("(enum %s%s", enum_decl.name.c_str(), loc.c_str());
                indent();
                for (const auto &member : enum_decl.members)
                {
                    write_line("(member %s)", member.c_str());
                }
                dedent();
                write_line(")");
                break;
            }
            case ast::stmt_type::ObjectDecl:
            {
                const auto &object = ast::as_stmt<ast::object_decl>(stmt);
                write_line("(object %s%s", object.name.c_str(), loc.c_str());
                indent();
                for (const auto &field : object.fields)
                {
                    write_line("(field %s%s", field.name.c_str(), at(field.loc).c_str());
                    indent();
                    print_type(*field.type);
                    dedent();
                    write_line(")");
                }
                dedent();
                write_line(")");
                break;
            }
            case ast::stmt_type::Monitor:
            {
                const auto &monitor = ast::as_stmt<ast::monitor_stmt>(stmt);
                write_line("(monitor%s", loc.c_str());
                indent();
                print_block(*monitor.protected_block);
                write_line("(trap %s", monitor.trap_name.c_str());
                indent();
                print_type(*monitor.trap_type);
                print_block(*monitor.handler);
                dedent();
                write_line(")");
                dedent();
                write_line(")");
                break;
            }
            case ast::stmt_type::Lock:
                write_line("(lock %s%s)", ast::as_stmt<ast::lock_stmt>(stmt).lock_name.c_str(),
                           loc.c_str());
                break;
            case ast::stmt_type::Acquire:
                write_line("(acquire %s%s)",
                           ast::as_stmt<ast::acquire_stmt>(stmt).lock_name.c_str(), loc.c_str());
                break;
            case ast::stmt_type::Release:
                write_line("(release %s%s)",
                           ast::as_stmt<ast::release_stmt>(stmt).lock_name.c_str(), loc.c_str());
                break;
            case ast::stmt_type::LoadPackage:
                print_load_package(ast::as_stmt<ast::load_package_stmt>(stmt));
                break;
            case ast::stmt_type::Insert:
            {
                const auto &insert = ast::as_stmt<ast::insert_stmt>(stmt);
                write_line("(insert %s%s", insert.receiver.c_str(), loc.c_str());
                indent();
                print_expr(*insert.map_expr);
                print_expr(*insert.entries);
                dedent();
                write_line(")");
                break;
            }
            case ast::stmt_type::ReflectFor:
            {
                const auto &rf = ast::as_stmt<ast::reflect_for_stmt>(stmt);
                write_line("(reflect-for %s%s", rf.item_name.c_str(), loc.c_str());
                indent();
                print_expr(*rf.collection);
                print_block(*rf.body);
                dedent();
                write_line(")");
                break;
            }
            case ast::stmt_type::TypeidMatch:
            {
                const auto &tm = ast::as_stmt<ast::typeid_match_stmt>(stmt);
                write_line("(typeid-match%s", loc.c_str());
                indent();
                print_expr(*tm.scrutinee);
                for (const auto &arm : tm.cases)
                {
                    if (arm.is_default)
                    {
                        write_line("(default%s", at(arm.loc).c_str());
                    }
                    else
                    {
                        write_line("(case typeid(%s)%s", arm.type_kind_name.c_str(),
                                   at(arm.loc).c_str());
                    }
                    indent();
                    if (arm.body)
                    {
                        print_expr(*arm.body);
                    }
                    dedent();
                    write_line(")");
                }
                dedent();
                write_line(")");
                break;
            }
            }
        }
    };

} // namespace munx
