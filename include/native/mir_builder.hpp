#pragma once

#include "../ast.hpp"
#include "../bytecode_compiler.hpp"
#include "../errors.hpp"
#include "../type_checker.hpp"
#include "mir.hpp"

#include <filesystem>
#include <unordered_map>
#include <variant>

namespace munx::native
{

inline void native_unsupported(const ast::source_loc &loc, const std::string &feature)
{
    fail_compile(loc.file + ':' + std::to_string(loc.line) + ':' +
                 std::to_string(loc.column) +
                 ": native: " + feature + " is not supported yet");
}

class mir_builder
{
    mir::module mod_;
    mir::function *fn_{nullptr};
    std::unordered_map<std::string, uint32_t> locals_;
    std::unordered_map<std::string, std::string> functions_; // name -> mangled
    std::unordered_map<std::string, bool> object_ctors_;
    uint32_t next_block_{0};
    std::vector<uint32_t> break_stack_;

public:
    mir::module build(const std::filesystem::path &source_dir, const ast::program &program)
    {
        package_resolver resolver{source_dir, program};
        resolver.resolve();
        if (!resolver.ok)
        {
            fail_compile("native: failed to resolve packages");
            return mod_;
        }

        type_checker::check_packages(source_dir, program,
                                     resolver.imports_package_programs);
        if (active_compile_context != nullptr && active_compile_context->failed())
        {
            return mod_;
        }

        mod_.entry_package = program.package_name;
        collect_functions(program);
        collect_objects(program);
        for (const ast::program &imported : resolver.imports_package_programs)
        {
            collect_functions(imported);
            collect_objects(imported);
        }
        for (const ast::program &imported : resolver.imports_package_programs)
        {
            emit_package(imported);
        }
        emit_package(program);
        return mod_;
    }

private:
    void collect_functions(const ast::program &program)
    {
        for (const auto &stmt : program.statements)
        {
            if (stmt->type == ast::stmt_type::FuncDecl)
            {
                const auto &fn = ast::as_stmt<ast::func_decl>(*stmt);
                functions_[fn.name] = "munx_fn_" + fn.name;
            }
        }
    }

    void collect_objects(const ast::program &program)
    {
        for (const auto &stmt : program.statements)
        {
            if (stmt->type == ast::stmt_type::ObjectDecl)
            {
                const auto &decl = ast::as_stmt<ast::object_decl>(*stmt);
                if (!decl.is_trait)
                {
                    object_ctors_[decl.name] = true;
                }
            }
        }
    }

    uint32_t new_block() { return next_block_++; }

    uint32_t emit(mir::instr in)
    {
        in.result = static_cast<uint32_t>(fn_->code.size());
        fn_->code.push_back(std::move(in));
        return in.result;
    }

    uint32_t local_slot(const std::string &name)
    {
        const auto found = locals_.find(name);
        if (found != locals_.end())
        {
            return found->second;
        }
        const uint32_t slot = fn_->local_count++;
        locals_[name] = slot;
        fn_->local_names.push_back(name);
        return slot;
    }

    void emit_package(const ast::program &program)
    {
        for (const auto &stmt : program.statements)
        {
            if (stmt->type == ast::stmt_type::FuncDecl)
            {
                emit_function(ast::as_stmt<ast::func_decl>(*stmt));
            }
        }

        mir::function init;
        init.name = "munx_init_" + program.package_name;
        init.is_entry_init = (program.package_name == mod_.entry_package);
        fn_ = &init;
        locals_.clear();
        next_block_ = 1;
        mir::instr entry;
        entry.op = mir::opcode::label;
        entry.block_target = 0;
        emit(std::move(entry));

        for (const auto &stmt : program.statements)
        {
            if (stmt->type == ast::stmt_type::FuncDecl ||
                stmt->type == ast::stmt_type::EnumDecl ||
                stmt->type == ast::stmt_type::ObjectDecl ||
                stmt->type == ast::stmt_type::LoadPackage)
            {
                continue;
            }
            emit_stmt(*stmt);
        }
        mir::instr ret;
        ret.op = mir::opcode::ret;
        ret.ty = mir::type::void_;
        emit(std::move(ret));
        mod_.functions.push_back(std::move(init));
        fn_ = nullptr;
    }

    void emit_function(const ast::func_decl &decl)
    {
        if (!decl.parameters.empty())
        {
            // Parameters supported as locals assigned from args — v1: max practical
        }
        mir::function f;
        f.name = functions_.at(decl.name);
        fn_ = &f;
        locals_.clear();
        next_block_ = 1;
        for (const auto &param : decl.parameters)
        {
            local_slot(param.name);
        }
        f.param_count = static_cast<uint32_t>(decl.parameters.size());
        mir::instr entry;
        entry.op = mir::opcode::label;
        entry.block_target = 0;
        emit(std::move(entry));
        // Load params from argument slots 0..n-1 (convention: first N locals are params)
        emit_block(*decl.body);
        bool ends_ret = !f.code.empty() && f.code.back().op == mir::opcode::ret;
        if (!ends_ret)
        {
            mir::instr ret;
            ret.op = mir::opcode::ret;
            ret.ty = mir::type::value;
            ret.args = {emit_null()};
            emit(std::move(ret));
        }
        mod_.functions.push_back(std::move(f));
        fn_ = nullptr;
    }

    void emit_block(const ast::block_stmt &block)
    {
        for (const auto &stmt : block.statements)
        {
            emit_stmt(*stmt);
        }
    }

    void emit_stmt(const ast::stmt_node &stmt)
    {
        switch (stmt.type)
        {
        case ast::stmt_type::Assignment:
            emit_assignment(ast::as_stmt<ast::assignment_stmt>(stmt), stmt.loc);
            break;
        case ast::stmt_type::Expr:
        {
            const uint32_t v = emit_expr(*ast::as_stmt<ast::expr_stmt>(stmt).expression);
            (void)v;
            break;
        }
        case ast::stmt_type::Return:
        {
            const auto &ret = ast::as_stmt<ast::return_stmt>(stmt);
            mir::instr in;
            in.op = mir::opcode::ret;
            in.ty = mir::type::value;
            if (ret.value.has_value())
            {
                in.args = {emit_expr(**ret.value)};
            }
            else
            {
                in.args = {emit_null()};
            }
            emit(std::move(in));
            break;
        }
        case ast::stmt_type::Break:
        {
            if (break_stack_.empty())
            {
                native_unsupported(stmt.loc, "`break` outside loop");
                break;
            }
            mir::instr in;
            in.op = mir::opcode::br;
            in.block_target = break_stack_.back();
            emit(std::move(in));
            break;
        }
        case ast::stmt_type::Block:
            emit_block(ast::as_stmt<ast::block_stmt>(stmt));
            break;
        case ast::stmt_type::If:
            emit_if(ast::as_stmt<ast::if_stmt>(stmt));
            break;
        case ast::stmt_type::Loop:
            emit_loop(ast::as_stmt<ast::loop_stmt>(stmt));
            break;
        case ast::stmt_type::FuncDecl:
        case ast::stmt_type::EnumDecl:
        case ast::stmt_type::ObjectDecl:
        case ast::stmt_type::LoadPackage:
            break;
        default:
            native_unsupported(stmt.loc, "statement kind");
            break;
        }
    }

    static bool is_tty_print_rebind(const std::string &name,
                                    const ast::expr_node &value)
    {
        if (name != "print" && name != "println")
        {
            return false;
        }
        if (value.type != ast::expr_type::Call)
        {
            return false;
        }
        const auto &call = ast::as<ast::call_expr>(value);
        if (call.callee->type != ast::expr_type::Identifier)
        {
            return false;
        }
        const std::string &callee = ast::as<ast::identifier>(*call.callee).name;
        return callee == "fix" || callee == "open";
    }

    void emit_assignment(const ast::assignment_stmt &assign, const ast::source_loc &loc)
    {
        if (assign.op != ast::assign_op::Assign && assign.op != ast::assign_op::AddAssign)
        {
            native_unsupported(loc, "assignment operator");
            return;
        }
        if (assign.targets.size() != 1 || assign.targets.front().is_discard ||
            assign.targets.front().name.empty())
        {
            native_unsupported(loc, "destructuring / complex bind");
            return;
        }
        const std::string &name = assign.targets.front().name;
        // Samples often rebind print/println to tty handles; native builtins cover that.
        if (is_tty_print_rebind(name, *assign.value))
        {
            return;
        }
        uint32_t value = emit_expr(*assign.value);
        if (assign.op == ast::assign_op::AddAssign)
        {
            mir::instr load;
            load.op = mir::opcode::load_local;
            load.local = local_slot(name);
            load.ty = mir::type::value;
            const uint32_t cur = emit(std::move(load));
            mir::instr add;
            add.op = mir::opcode::add;
            add.ty = mir::type::value;
            add.args = {cur, value};
            value = emit(std::move(add));
        }
        mir::instr store;
        store.op = mir::opcode::store_local;
        store.local = local_slot(name);
        store.args = {value};
        emit(std::move(store));
    }

    void emit_if(const ast::if_stmt &stmt)
    {
        const uint32_t then_b = new_block();
        const uint32_t else_b = new_block();
        const uint32_t join_b = new_block();

        const uint32_t cond = emit_expr(*stmt.then_branch->condition);
        mir::instr cbr;
        cbr.op = mir::opcode::cbr;
        cbr.args = {cond};
        cbr.block_target = then_b;
        cbr.block_target_false = else_b;
        cbr.branch_hint = static_cast<uint8_t>(stmt.then_branch->hint);
        emit(std::move(cbr));

        mir::instr lab_then;
        lab_then.op = mir::opcode::label;
        lab_then.block_target = then_b;
        emit(std::move(lab_then));
        emit_block(*stmt.then_branch->body);
        mir::instr br_join;
        br_join.op = mir::opcode::br;
        br_join.block_target = join_b;
        emit(std::move(br_join));

        mir::instr lab_else;
        lab_else.op = mir::opcode::label;
        lab_else.block_target = else_b;
        emit(std::move(lab_else));
        for (size_t i = 0; i < stmt.else_if_branches.size(); ++i)
        {
            const auto &br = *stmt.else_if_branches[i];
            const uint32_t t = new_block();
            const uint32_t f = new_block();
            const uint32_t c = emit_expr(*br.condition);
            mir::instr cb;
            cb.op = mir::opcode::cbr;
            cb.args = {c};
            cb.block_target = t;
            cb.block_target_false = f;
            cb.branch_hint = static_cast<uint8_t>(br.hint);
            emit(std::move(cb));
            mir::instr lt;
            lt.op = mir::opcode::label;
            lt.block_target = t;
            emit(std::move(lt));
            emit_block(*br.body);
            mir::instr bj;
            bj.op = mir::opcode::br;
            bj.block_target = join_b;
            emit(std::move(bj));
            mir::instr lf;
            lf.op = mir::opcode::label;
            lf.block_target = f;
            emit(std::move(lf));
        }
        if (stmt.else_branch.has_value())
        {
            emit_block(**stmt.else_branch);
        }
        mir::instr bj2;
        bj2.op = mir::opcode::br;
        bj2.block_target = join_b;
        emit(std::move(bj2));

        mir::instr lab_join;
        lab_join.op = mir::opcode::label;
        lab_join.block_target = join_b;
        emit(std::move(lab_join));
    }

    void emit_loop(const ast::loop_stmt &stmt)
    {
        const uint32_t head = new_block();
        const uint32_t body = new_block();
        const uint32_t exit = new_block();

        mir::instr br_head;
        br_head.op = mir::opcode::br;
        br_head.block_target = head;
        emit(std::move(br_head));

        mir::instr lab_head;
        lab_head.op = mir::opcode::label;
        lab_head.block_target = head;
        emit(std::move(lab_head));

        if (stmt.condition.has_value())
        {
            const uint32_t c = emit_expr(**stmt.condition);
            mir::instr cbr;
            cbr.op = mir::opcode::cbr;
            cbr.args = {c};
            cbr.block_target = body;
            cbr.block_target_false = exit;
            emit(std::move(cbr));
        }
        else
        {
            mir::instr br;
            br.op = mir::opcode::br;
            br.block_target = body;
            emit(std::move(br));
        }

        mir::instr lab_body;
        lab_body.op = mir::opcode::label;
        lab_body.block_target = body;
        emit(std::move(lab_body));
        break_stack_.push_back(exit);
        emit_block(*stmt.body);
        break_stack_.pop_back();
        mir::instr br_back;
        br_back.op = mir::opcode::br;
        br_back.block_target = head;
        emit(std::move(br_back));

        mir::instr lab_exit;
        lab_exit.op = mir::opcode::label;
        lab_exit.block_target = exit;
        emit(std::move(lab_exit));
    }

    uint32_t emit_null()
    {
        mir::instr in;
        in.op = mir::opcode::const_null;
        in.ty = mir::type::value;
        return emit(std::move(in));
    }

    uint32_t emit_expr(const ast::expr_node &expr)
    {
        switch (expr.type)
        {
        case ast::expr_type::IntLiteral:
        {
            mir::instr in;
            in.op = mir::opcode::const_i64;
            in.ty = mir::type::value;
            in.i64 = ast::as<ast::int_literal>(expr).value;
            return emit(std::move(in));
        }
        case ast::expr_type::FloatLiteral:
        {
            mir::instr in;
            in.op = mir::opcode::const_f64;
            in.ty = mir::type::value;
            in.f64 = static_cast<double>(ast::as<ast::float_literal>(expr).value);
            return emit(std::move(in));
        }
        case ast::expr_type::BoolLiteral:
        {
            mir::instr in;
            in.op = mir::opcode::const_bool;
            in.ty = mir::type::value;
            in.b = ast::as<ast::bool_literal>(expr).value;
            return emit(std::move(in));
        }
        case ast::expr_type::NullLiteral:
            return emit_null();
        case ast::expr_type::StringLiteral:
        {
            mir::instr in;
            in.op = mir::opcode::const_str;
            in.ty = mir::type::value;
            in.str_index = mod_.intern(ast::as<ast::string_literal>(expr).value);
            return emit(std::move(in));
        }
        case ast::expr_type::CharLiteral:
        {
            mir::instr in;
            in.op = mir::opcode::const_i64;
            in.ty = mir::type::value;
            in.i64 = static_cast<unsigned char>(ast::as<ast::char_literal>(expr).value);
            return emit(std::move(in));
        }
        case ast::expr_type::Identifier:
        {
            const std::string &name = ast::as<ast::identifier>(expr).name;
            if (name == "out")
            {
                mir::instr in;
                in.op = mir::opcode::call;
                in.callee = "munx_pipe_mode_out";
                in.ty = mir::type::value;
                return emit(std::move(in));
            }
            if (name == "in")
            {
                mir::instr in;
                in.op = mir::opcode::call;
                in.callee = "munx_pipe_mode_in";
                in.ty = mir::type::value;
                return emit(std::move(in));
            }
            if (name == "subscribe")
            {
                mir::instr in;
                in.op = mir::opcode::call;
                in.callee = "munx_pipe_mode_subscribe";
                in.ty = mir::type::value;
                return emit(std::move(in));
            }
            mir::instr in;
            in.op = mir::opcode::load_local;
            in.local = local_slot(name);
            in.ty = mir::type::value;
            return emit(std::move(in));
        }
        case ast::expr_type::Binary:
            return emit_binary(ast::as<ast::binary_expr>(expr), expr.loc);
        case ast::expr_type::Unary:
            return emit_unary(ast::as<ast::unary_expr>(expr), expr.loc);
        case ast::expr_type::Call:
            return emit_call(ast::as<ast::call_expr>(expr), expr.loc);
        case ast::expr_type::PipeInsert:
        {
            const auto &ins = ast::as<ast::pipe_insert_expr>(expr);
            const uint32_t val = emit_expr(*ins.value);
            mir::instr load;
            load.op = mir::opcode::load_local;
            load.local = local_slot(ins.pipe_name);
            load.ty = mir::type::value;
            const uint32_t pipe = emit(std::move(load));
            mir::instr in;
            in.op = mir::opcode::call;
            in.callee = "munx_pipe_insert";
            in.args = {pipe, val};
            in.ty = mir::type::void_;
            emit(std::move(in));
            return emit_null();
        }
        case ast::expr_type::PipeExtract:
        {
            const auto &ex = ast::as<ast::pipe_extract_expr>(expr);
            mir::instr load;
            load.op = mir::opcode::load_local;
            load.local = local_slot(ex.pipe_name);
            load.ty = mir::type::value;
            const uint32_t pipe = emit(std::move(load));
            mir::instr in;
            in.op = mir::opcode::call;
            in.callee = "munx_pipe_extract";
            in.args = {pipe};
            in.ty = mir::type::value;
            return emit(std::move(in));
        }
        case ast::expr_type::ChannelInsert:
        {
            const auto &ins = ast::as<ast::channel_insert_expr>(expr);
            const uint32_t val = emit_expr(*ins.value);
            mir::instr load;
            load.op = mir::opcode::load_local;
            load.local = local_slot(ins.channel_name);
            load.ty = mir::type::value;
            const uint32_t ch = emit(std::move(load));
            mir::instr in;
            in.op = mir::opcode::call;
            in.callee = "munx_channel_insert";
            in.args = {ch, val};
            in.ty = mir::type::void_;
            emit(std::move(in));
            return emit_null();
        }
        case ast::expr_type::ChannelExtract:
        {
            const auto &ex = ast::as<ast::channel_extract_expr>(expr);
            mir::instr load;
            load.op = mir::opcode::load_local;
            load.local = local_slot(ex.channel_name);
            load.ty = mir::type::value;
            const uint32_t ch = emit(std::move(load));
            mir::instr in;
            in.op = mir::opcode::call;
            in.callee = "munx_channel_extract";
            in.args = {ch};
            in.ty = mir::type::value;
            return emit(std::move(in));
        }
        case ast::expr_type::Cast:
        {
            const auto &cast = ast::as<ast::cast_expr>(expr);
            const uint32_t operand = emit_expr(*cast.operand);
            if (cast.target_type->type == ast::type_kind::Primitive &&
                std::get<ast::primitive_type>(cast.target_type->value).kind ==
                    ast::primitive_kind::String)
            {
                mir::instr in;
                in.op = mir::opcode::call;
                in.callee = "munx_to_string";
                in.args = {operand};
                in.ty = mir::type::value;
                return emit(std::move(in));
            }
            native_unsupported(expr.loc, "cast target");
            return emit_null();
        }
        case ast::expr_type::Member:
        {
            const auto &mem = ast::as<ast::member_expr>(expr);
            if (mem.object->type == ast::expr_type::Identifier &&
                ast::as<ast::identifier>(*mem.object).name == "argv" && mem.member == "len")
            {
                mir::instr in;
                in.op = mir::opcode::call;
                in.callee = "munx_argv_len";
                in.ty = mir::type::value;
                return emit(std::move(in));
            }
            native_unsupported(expr.loc, "member access");
            return emit_null();
        }
        case ast::expr_type::Index:
        {
            const auto &idx = ast::as<ast::index_expr>(expr);
            if (idx.object->type == ast::expr_type::Identifier &&
                ast::as<ast::identifier>(*idx.object).name == "argv")
            {
                const uint32_t i = emit_expr(*idx.index);
                mir::instr in;
                in.op = mir::opcode::call;
                in.callee = "munx_argv_get";
                in.args = {i};
                in.ty = mir::type::value;
                return emit(std::move(in));
            }
            native_unsupported(expr.loc, "index");
            return emit_null();
        }
        default:
            native_unsupported(expr.loc, "expression kind");
            return emit_null();
        }
    }

    uint32_t emit_binary(const ast::binary_expr &bin, const ast::source_loc &loc)
    {
        const uint32_t l = emit_expr(*bin.left);
        const uint32_t r = emit_expr(*bin.right);
        mir::opcode op = mir::opcode::add;
        switch (bin.op)
        {
        case ast::binary_op::Add:
            op = mir::opcode::add;
            break;
        case ast::binary_op::Sub:
            op = mir::opcode::sub;
            break;
        case ast::binary_op::Mul:
            op = mir::opcode::mul;
            break;
        case ast::binary_op::Div:
            op = mir::opcode::div;
            break;
        case ast::binary_op::Mod:
            op = mir::opcode::mod;
            break;
        case ast::binary_op::Eq:
            op = mir::opcode::eq;
            break;
        case ast::binary_op::Ne:
            op = mir::opcode::ne;
            break;
        case ast::binary_op::Lt:
            op = mir::opcode::lt;
            break;
        case ast::binary_op::Gt:
            op = mir::opcode::gt;
            break;
        case ast::binary_op::Le:
            op = mir::opcode::le;
            break;
        case ast::binary_op::Ge:
            op = mir::opcode::ge;
            break;
        case ast::binary_op::And:
        case ast::binary_op::Or:
            native_unsupported(loc, "short-circuit && / || (use nested if)");
            return emit_null();
        default:
            native_unsupported(loc, "binary operator");
            return emit_null();
        }
        mir::instr in;
        in.op = op;
        in.ty = mir::type::value;
        in.args = {l, r};
        return emit(std::move(in));
    }

    uint32_t emit_unary(const ast::unary_expr &un, const ast::source_loc &loc)
    {
        const uint32_t a = emit_expr(*un.operand);
        mir::instr in;
        in.ty = mir::type::value;
        in.args = {a};
        switch (un.op)
        {
        case ast::unary_op::Neg:
            in.op = mir::opcode::neg;
            break;
        case ast::unary_op::Not:
            in.op = mir::opcode::not_;
            break;
        default:
            native_unsupported(loc, "unary operator");
            return emit_null();
        }
        return emit(std::move(in));
    }

    uint32_t emit_call(const ast::call_expr &call, const ast::source_loc &loc)
    {
        if (call.callee->type != ast::expr_type::Identifier)
        {
            native_unsupported(loc, "indirect call");
            return emit_null();
        }
        const std::string &name = ast::as<ast::identifier>(*call.callee).name;
        if ((name == "likely" || name == "unlikely") && call.arguments.size() == 1)
        {
            return emit_expr(*call.arguments[0]);
        }
        if (name == "pipe")
        {
            if (call.arguments.size() != 2)
            {
                native_unsupported(loc, "pipe() arity");
                return emit_null();
            }
            mir::instr in;
            in.op = mir::opcode::call;
            in.callee = "munx_pipe_open";
            in.args = {emit_expr(*call.arguments[0]),
                       emit_expr(*call.arguments[1])};
            in.ty = mir::type::value;
            return emit(std::move(in));
        }
        if (name == "channel")
        {
            if (call.arguments.size() != 1)
            {
                native_unsupported(loc, "channel() arity");
                return emit_null();
            }
            mir::instr in;
            in.op = mir::opcode::call;
            in.callee = "munx_channel_open";
            in.args = {emit_expr(*call.arguments[0])};
            in.ty = mir::type::value;
            return emit(std::move(in));
        }
        if (name == "concat")
        {
            if (call.arguments.size() != 2)
            {
                native_unsupported(loc, "concat() arity");
                return emit_null();
            }
            mir::instr in;
            in.op = mir::opcode::call;
            in.callee = "munx_concat";
            in.args = {emit_expr(*call.arguments[0]),
                       emit_expr(*call.arguments[1])};
            in.ty = mir::type::value;
            return emit(std::move(in));
        }
        if (name == "sleep")
        {
            if (call.arguments.size() != 1)
            {
                native_unsupported(loc, "sleep() arity");
                return emit_null();
            }
            mir::instr in;
            in.op = mir::opcode::call;
            in.callee = "munx_sleep";
            in.args = {emit_expr(*call.arguments[0])};
            in.ty = mir::type::void_;
            emit(std::move(in));
            return emit_null();
        }
        if (name == "fail")
        {
            mir::instr in;
            in.op = mir::opcode::call;
            in.callee = "munx_fail";
            if (!call.arguments.empty())
            {
                in.args = {emit_expr(*call.arguments[0])};
            }
            else
            {
                mir::instr msg;
                msg.op = mir::opcode::const_str;
                msg.str_index = mod_.intern("fail");
                msg.ty = mir::type::value;
                in.args = {emit(std::move(msg))};
            }
            in.ty = mir::type::void_;
            emit(std::move(in));
            return emit_null();
        }
        if (name == "print")
        {
            for (const auto &arg : call.arguments)
            {
                const uint32_t v = emit_expr(*arg);
                mir::instr in;
                in.op = mir::opcode::print;
                in.args = {v};
                emit(std::move(in));
            }
            return emit_null();
        }
        if (name == "println")
        {
            for (const auto &arg : call.arguments)
            {
                const uint32_t v = emit_expr(*arg);
                mir::instr in;
                in.op = mir::opcode::print;
                in.args = {v};
                emit(std::move(in));
            }
            mir::instr nl;
            nl.op = mir::opcode::println;
            emit(std::move(nl));
            return emit_null();
        }
        if (object_ctors_.contains(name))
        {
            // Minimal object support: evaluate constructor args for side effects,
            // return a null stub (fields are unused after compile-time reflexpr).
            for (const auto &arg : call.arguments)
            {
                (void)emit_expr(*arg);
            }
            return emit_null();
        }
        const auto found = functions_.find(name);
        if (found == functions_.end())
        {
            native_unsupported(loc, "call to unknown or unsupported function `" + name + "`");
            return emit_null();
        }
        mir::instr in;
        in.op = mir::opcode::call;
        in.callee = found->second;
        in.ty = mir::type::value;
        for (const auto &arg : call.arguments)
        {
            in.args.push_back(emit_expr(*arg));
        }
        return emit(std::move(in));
    }
};

} // namespace munx::native
