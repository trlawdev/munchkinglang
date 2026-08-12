#pragma once

#include "../../errors.hpp"
#include "../mir.hpp"
#include "../toolchain.hpp"

#include <cstdio>
#include <sstream>
#include <string>

namespace munx::native::llvm_backend
{

#if MUNX_NATIVE_LLVM

namespace detail
{

inline std::string ll_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s)
    {
        if (c == '\\')
        {
            out += "\\\\";
        }
        else if (c == '"')
        {
            out += "\\22";
        }
        else if (c >= 32 && c < 127)
        {
            out.push_back(static_cast<char>(c));
        }
        else
        {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\%02X", c);
            out += buf;
        }
    }
    return out;
}

inline std::string vslot(uint32_t id) { return "%v" + std::to_string(id); }

inline const char *runtime_binop(mir::opcode op)
{
    switch (op)
    {
    case mir::opcode::add:
        return "munx_add";
    case mir::opcode::sub:
        return "munx_sub";
    case mir::opcode::mul:
        return "munx_mul";
    case mir::opcode::div:
        return "munx_div";
    case mir::opcode::mod:
        return "munx_mod";
    case mir::opcode::eq:
        return "munx_eq";
    case mir::opcode::ne:
        return "munx_ne";
    case mir::opcode::lt:
        return "munx_lt";
    case mir::opcode::gt:
        return "munx_gt";
    case mir::opcode::le:
        return "munx_le";
    case mir::opcode::ge:
        return "munx_ge";
    default:
        return nullptr;
    }
}

inline void load_mv_parts(std::ostringstream &out, uint32_t &tmp,
                          const std::string &ptr, std::string &tag,
                          std::string &payload)
{
    const uint32_t loaded = tmp++;
    const uint32_t t = tmp++;
    const uint32_t p = tmp++;
    out << "  %" << loaded << " = load %MV, %MV* " << ptr << "\n";
    out << "  %" << t << " = extractvalue %MV %" << loaded << ", 0\n";
    out << "  %" << p << " = extractvalue %MV %" << loaded << ", 1\n";
    tag = "%" + std::to_string(t);
    payload = "%" + std::to_string(p);
}

inline void store_call_result(std::ostringstream &out, uint32_t &tmp,
                              const std::string &dst_ptr,
                              const std::string &call_expr)
{
    const uint32_t id = tmp++;
    out << "  %" << id << " = " << call_expr << "\n";
    out << "  store %MV %" << id << ", %MV* " << dst_ptr << "\n";
}

inline bool is_terminator(mir::opcode op)
{
    return op == mir::opcode::br || op == mir::opcode::cbr ||
           op == mir::opcode::ret;
}

inline void emit_runtime_decls(std::ostringstream &out)
{
    out << "declare %MV @munx_null()\n";
    out << "declare %MV @munx_i64(i64)\n";
    out << "declare %MV @munx_f64(double)\n";
    out << "declare %MV @munx_bool(i1)\n";
    out << "declare %MV @munx_string(i8*)\n";
    out << "declare %MV @munx_add(i8, i64, i8, i64)\n";
    out << "declare %MV @munx_sub(i8, i64, i8, i64)\n";
    out << "declare %MV @munx_mul(i8, i64, i8, i64)\n";
    out << "declare %MV @munx_div(i8, i64, i8, i64)\n";
    out << "declare %MV @munx_mod(i8, i64, i8, i64)\n";
    out << "declare %MV @munx_neg(i8, i64)\n";
    out << "declare %MV @munx_not(i8, i64)\n";
    out << "declare %MV @munx_eq(i8, i64, i8, i64)\n";
    out << "declare %MV @munx_ne(i8, i64, i8, i64)\n";
    out << "declare %MV @munx_lt(i8, i64, i8, i64)\n";
    out << "declare %MV @munx_gt(i8, i64, i8, i64)\n";
    out << "declare %MV @munx_le(i8, i64, i8, i64)\n";
    out << "declare %MV @munx_ge(i8, i64, i8, i64)\n";
    out << "declare zeroext i1 @munx_truthy(i8, i64)\n";
    out << "declare void @munx_print(i8, i64)\n";
    out << "declare void @munx_println()\n";
    out << "declare void @munx_set_argv(i32, i8**)\n";
    out << "declare %MV @munx_argv_len()\n";
    out << "declare %MV @munx_argv_get(i8, i64)\n";
    out << "declare %MV @munx_pipe_open(i8, i64, i8, i64)\n";
    out << "declare void @munx_pipe_insert(i8, i64, i8, i64)\n";
    out << "declare %MV @munx_pipe_extract(i8, i64)\n";
    out << "declare %MV @munx_channel_open(i8, i64)\n";
    out << "declare void @munx_channel_insert(i8, i64, i8, i64)\n";
    out << "declare %MV @munx_channel_extract(i8, i64)\n";
    out << "declare %MV @munx_pipe_mode_out()\n";
    out << "declare %MV @munx_pipe_mode_in()\n";
    out << "declare %MV @munx_pipe_mode_subscribe()\n";
    out << "declare %MV @munx_concat(i8, i64, i8, i64)\n";
    out << "declare %MV @munx_to_string(i8, i64)\n";
    out << "declare void @munx_fail(i8, i64)\n";
    out << "declare void @munx_sleep(i8, i64)\n\n";
}

inline void emit_function_body(std::ostringstream &out, const mir::function &fn)
{
    const bool is_init = fn.name.rfind("munx_init_", 0) == 0;
    uint32_t tmp = 0;

    if (is_init)
    {
        out << "define internal void @" << fn.name << "() {\n";
    }
    else
    {
        out << "define internal %MV @" << fn.name << "(";
        for (uint32_t i = 0; i < fn.param_count; ++i)
        {
            if (i)
            {
                out << ", ";
            }
            out << "i8 %p" << i << "t, i64 %p" << i << "p";
        }
        out << ") {\n";
    }

    out << "entry:\n";
    for (size_t i = 0; i < fn.code.size(); ++i)
    {
        out << "  " << vslot(static_cast<uint32_t>(i)) << " = alloca %MV\n";
    }
    if (fn.local_count > 0)
    {
        out << "  %locals = alloca %MV, i32 " << fn.local_count << "\n";
        for (uint32_t i = 0; i < fn.local_count; ++i)
        {
            const uint32_t ge = tmp++;
            const uint32_t n = tmp++;
            out << "  %" << ge << " = getelementptr %MV, %MV* %locals, i32 "
                << i << "\n";
            out << "  %" << n << " = call %MV @munx_null()\n";
            out << "  store %MV %" << n << ", %MV* %" << ge << "\n";
        }
        for (uint32_t i = 0; i < fn.param_count; ++i)
        {
            const uint32_t ge = tmp++;
            const uint32_t ins = tmp++;
            const uint32_t ins2 = tmp++;
            out << "  %" << ge << " = getelementptr %MV, %MV* %locals, i32 "
                << i << "\n";
            out << "  %" << ins << " = insertvalue %MV undef, i8 %p" << i
                << "t, 0\n";
            out << "  %" << ins2 << " = insertvalue %MV %" << ins << ", i64 %p"
                << i << "p, 1\n";
            out << "  store %MV %" << ins2 << ", %MV* %" << ge << "\n";
        }
    }

    const bool starts_with_label =
        !fn.code.empty() && fn.code.front().op == mir::opcode::label;
    if (starts_with_label)
    {
        out << "  br label %L" << fn.code.front().block_target << "\n";
    }
    else
    {
        out << "  br label %Lbody\n";
        out << "Lbody:\n";
    }

    bool need_block = false;
    for (size_t i = 0; i < fn.code.size(); ++i)
    {
        const mir::instr &in = fn.code[i];
        if (in.op == mir::opcode::label)
        {
            if (need_block)
            {
                // Previous block already terminated; this label starts a new one.
            }
            else if (i > 0 && !is_terminator(fn.code[i - 1].op))
            {
                out << "  br label %L" << in.block_target << "\n";
            }
            out << "L" << in.block_target << ":\n";
            need_block = false;
            continue;
        }

        if (need_block)
        {
            out << "Bunreach" << i << ":\n";
            need_block = false;
        }

        const std::string dst = vslot(static_cast<uint32_t>(i));
        switch (in.op)
        {
        case mir::opcode::const_i64:
            store_call_result(out, tmp, dst,
                              "call %MV @munx_i64(i64 " +
                                  std::to_string(in.i64) + ")");
            break;
        case mir::opcode::const_f64:
        {
            std::ostringstream expr;
            expr << "call %MV @munx_f64(double " << std::to_string(in.f64)
                 << ")";
            store_call_result(out, tmp, dst, expr.str());
            break;
        }
        case mir::opcode::const_bool:
            store_call_result(out, tmp, dst,
                              std::string("call %MV @munx_bool(i1 ") +
                                  (in.b ? "true" : "false") + ")");
            break;
        case mir::opcode::const_null:
            store_call_result(out, tmp, dst, "call %MV @munx_null()");
            break;
        case mir::opcode::const_str:
        {
            const uint32_t loaded = tmp++;
            out << "  %" << loaded << " = load %MV, %MV* @S" << in.str_index
                << "\n";
            out << "  store %MV %" << loaded << ", %MV* " << dst << "\n";
            break;
        }
        case mir::opcode::load_local:
        {
            const uint32_t ge = tmp++;
            const uint32_t loaded = tmp++;
            out << "  %" << ge << " = getelementptr %MV, %MV* %locals, i32 "
                << in.local << "\n";
            out << "  %" << loaded << " = load %MV, %MV* %" << ge << "\n";
            out << "  store %MV %" << loaded << ", %MV* " << dst << "\n";
            break;
        }
        case mir::opcode::store_local:
        {
            const uint32_t ge = tmp++;
            const uint32_t loaded = tmp++;
            out << "  %" << ge << " = getelementptr %MV, %MV* %locals, i32 "
                << in.local << "\n";
            out << "  %" << loaded << " = load %MV, %MV* " << vslot(in.args[0])
                << "\n";
            out << "  store %MV %" << loaded << ", %MV* %" << ge << "\n";
            break;
        }
        case mir::opcode::neg:
        case mir::opcode::not_:
        {
            std::string t;
            std::string p;
            load_mv_parts(out, tmp, vslot(in.args[0]), t, p);
            const char *name =
                (in.op == mir::opcode::neg) ? "munx_neg" : "munx_not";
            store_call_result(out, tmp, dst,
                              std::string("call %MV @") + name + "(i8 " + t +
                                  ", i64 " + p + ")");
            break;
        }
        case mir::opcode::add:
        case mir::opcode::sub:
        case mir::opcode::mul:
        case mir::opcode::div:
        case mir::opcode::mod:
        case mir::opcode::eq:
        case mir::opcode::ne:
        case mir::opcode::lt:
        case mir::opcode::gt:
        case mir::opcode::le:
        case mir::opcode::ge:
        {
            std::string t0;
            std::string p0;
            std::string t1;
            std::string p1;
            load_mv_parts(out, tmp, vslot(in.args[0]), t0, p0);
            load_mv_parts(out, tmp, vslot(in.args[1]), t1, p1);
            store_call_result(out, tmp, dst,
                              std::string("call %MV @") + runtime_binop(in.op) +
                                  "(i8 " + t0 + ", i64 " + p0 + ", i8 " + t1 +
                                  ", i64 " + p1 + ")");
            break;
        }
        case mir::opcode::print:
        {
            std::string t;
            std::string p;
            load_mv_parts(out, tmp, vslot(in.args[0]), t, p);
            out << "  call void @munx_print(i8 " << t << ", i64 " << p << ")\n";
            break;
        }
        case mir::opcode::println:
            out << "  call void @munx_println()\n";
            break;
        case mir::opcode::call:
            if (in.callee == "munx_argv_len")
            {
                store_call_result(out, tmp, dst, "call %MV @munx_argv_len()");
            }
            else if (in.callee == "munx_argv_get")
            {
                std::string t;
                std::string p;
                load_mv_parts(out, tmp, vslot(in.args[0]), t, p);
                store_call_result(out, tmp, dst,
                                  "call %MV @munx_argv_get(i8 " + t + ", i64 " +
                                      p + ")");
            }
            else if (in.ty == mir::type::void_ || in.callee == "munx_fail" ||
                     in.callee == "munx_sleep" ||
                     in.callee == "munx_pipe_insert" ||
                     in.callee == "munx_channel_insert")
            {
                std::ostringstream call;
                call << "call void @" << in.callee << "(";
                for (size_t a = 0; a < in.args.size(); ++a)
                {
                    if (a)
                    {
                        call << ", ";
                    }
                    std::string t;
                    std::string p;
                    load_mv_parts(out, tmp, vslot(in.args[a]), t, p);
                    call << "i8 " << t << ", i64 " << p;
                }
                call << ")";
                out << "  " << call.str() << "\n";
            }
            else
            {
                std::ostringstream call;
                call << "call %MV @" << in.callee << "(";
                for (size_t a = 0; a < in.args.size(); ++a)
                {
                    if (a)
                    {
                        call << ", ";
                    }
                    std::string t;
                    std::string p;
                    load_mv_parts(out, tmp, vslot(in.args[a]), t, p);
                    call << "i8 " << t << ", i64 " << p;
                }
                call << ")";
                store_call_result(out, tmp, dst, call.str());
            }
            break;
        case mir::opcode::ret:
            if (is_init || in.ty == mir::type::void_)
            {
                out << "  ret void\n";
            }
            else if (in.args.empty())
            {
                const uint32_t n = tmp++;
                out << "  %" << n << " = call %MV @munx_null()\n";
                out << "  ret %MV %" << n << "\n";
            }
            else
            {
                const uint32_t loaded = tmp++;
                out << "  %" << loaded << " = load %MV, %MV* "
                    << vslot(in.args[0]) << "\n";
                out << "  ret %MV %" << loaded << "\n";
            }
            need_block = true;
            break;
        case mir::opcode::br:
            out << "  br label %L" << in.block_target << "\n";
            need_block = true;
            break;
        case mir::opcode::cbr:
        {
            std::string t;
            std::string p;
            load_mv_parts(out, tmp, vslot(in.args[0]), t, p);
            const uint32_t cond = tmp++;
            out << "  %" << cond << " = call zeroext i1 @munx_truthy(i8 " << t
                << ", i64 " << p << ")\n";
            out << "  br i1 %" << cond << ", label %L" << in.block_target
                << ", label %L" << in.block_target_false;
            if (in.branch_hint == 1)
            {
                out << ", !prof !munx.likely";
            }
            else if (in.branch_hint == 2)
            {
                out << ", !prof !munx.unlikely";
            }
            out << "\n";
            need_block = true;
            break;
        }
        case mir::opcode::label:
            break;
        }
    }

    if (!fn.code.empty() && !is_terminator(fn.code.back().op))
    {
        if (is_init)
        {
            out << "  ret void\n";
        }
        else
        {
            const uint32_t n = tmp++;
            out << "  %" << n << " = call %MV @munx_null()\n";
            out << "  ret %MV %" << n << "\n";
        }
    }
    out << "}\n\n";
}

} // namespace detail

/// Lower MIR to LLVM IR text (host MunxValue ABI coerced to { i8, i64 }).
inline std::string emit_ll(const mir::module &mod)
{
    std::ostringstream out;
    out << "; generated by munxc --native --backend llvm\n";
    out << "%MV = type { i8, i64 }\n";
    out << "!munx.likely = !{!\"branch_weights\", i32 2000, i32 1}\n";
    out << "!munx.unlikely = !{!\"branch_weights\", i32 1, i32 2000}\n\n";
    detail::emit_runtime_decls(out);

    for (uint32_t i = 0; i < mod.strings.size(); ++i)
    {
        const std::string &s = mod.strings[i];
        out << "@str." << i << " = private unnamed_addr constant ["
            << (s.size() + 1) << " x i8] c\"" << detail::ll_escape(s)
            << "\\00\"\n";
        out << "@S" << i << " = internal global %MV zeroinitializer\n";
    }
    out << "\n";

    out << "define internal void @munx_init_strings() {\nentry:\n";
    uint32_t tmp = 0;
    for (uint32_t i = 0; i < mod.strings.size(); ++i)
    {
        const uint32_t ge = tmp++;
        const uint32_t call = tmp++;
        out << "  %" << ge << " = getelementptr [" << (mod.strings[i].size() + 1)
            << " x i8], [" << (mod.strings[i].size() + 1) << " x i8]* @str."
            << i << ", i64 0, i64 0\n";
        out << "  %" << call << " = call %MV @munx_string(i8* %" << ge << ")\n";
        out << "  store %MV %" << call << ", %MV* @S" << i << "\n";
    }
    out << "  ret void\n}\n\n";

    for (const mir::function &fn : mod.functions)
    {
        detail::emit_function_body(out, fn);
    }

    out << "declare void @munx_pipe_session_begin()\n\n";
    out << "define i32 @main(i32 %argc, i8** %argv) {\nentry:\n";
    out << "  %argc1 = icmp sgt i32 %argc, 0\n";
    out << "  %argc_adj = select i1 %argc1, i32 1, i32 0\n";
    out << "  %argc_user = sub i32 %argc, %argc_adj\n";
    out << "  %argv_user = getelementptr i8*, i8** %argv, i32 %argc_adj\n";
    out << "  call void @munx_set_argv(i32 %argc_user, i8** %argv_user)\n";
    out << "  call void @munx_pipe_session_begin()\n";
    out << "  call void @munx_init_strings()\n";
    for (const mir::function &fn : mod.functions)
    {
        if (fn.name.rfind("munx_init_", 0) == 0 && !fn.is_entry_init)
        {
            out << "  call void @" << fn.name << "()\n";
        }
    }
    for (const mir::function &fn : mod.functions)
    {
        if (fn.is_entry_init)
        {
            out << "  call void @" << fn.name << "()\n";
        }
    }
    out << "  ret i32 0\n}\n";
    return out.str();
}

inline void emit_and_link(const mir::module &mod,
                          const std::filesystem::path &output)
{
    link_generated_ll(emit_ll(mod), output);
}

#else

inline void emit_and_link(const mir::module &,
                          const std::filesystem::path &)
{
    fail_compile("native: LLVM backend was not compiled in (build with "
        "MUNX_NATIVE_BACKEND=llvm or both)");
}

#endif

} // namespace munx::native::llvm_backend
