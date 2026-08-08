#pragma once

#include "../mir.hpp"

#include <cstdio>
#include <sstream>
#include <string>

namespace munx::native::custom
{

namespace detail
{

inline std::string c_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 32 || c > 126)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\x%02x", c);
                out += buf;
            }
            else
            {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

inline std::string vref(uint32_t id) { return "v" + std::to_string(id); }

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

inline void emit_function(std::ostringstream &out, const mir::function &fn)
{
    const bool is_init = fn.name.rfind("munx_init_", 0) == 0;
    if (is_init)
    {
        out << "static void " << fn.name << "(void)\n{\n";
    }
    else
    {
        out << "static MunxValue " << fn.name << "(";
        for (uint32_t i = 0; i < fn.param_count; ++i)
        {
            if (i)
            {
                out << ", ";
            }
            out << "MunxValue p" << i;
        }
        out << ")\n{\n";
    }

    if (fn.local_count > 0)
    {
        out << "  MunxValue locals[" << fn.local_count << "];\n";
        out << "  for (uint32_t i = 0; i < " << fn.local_count
            << "; ++i) locals[i] = munx_null();\n";
        for (uint32_t i = 0; i < fn.param_count; ++i)
        {
            out << "  locals[" << i << "] = p" << i << ";\n";
        }
    }

    for (size_t i = 0; i < fn.code.size(); ++i)
    {
        out << "  MunxValue " << vref(static_cast<uint32_t>(i))
            << " = munx_null();\n";
    }

    for (size_t i = 0; i < fn.code.size(); ++i)
    {
        const mir::instr &in = fn.code[i];
        const std::string dst = vref(static_cast<uint32_t>(i));
        switch (in.op)
        {
        case mir::opcode::label:
            out << "L" << in.block_target << ": ;\n";
            break;
        case mir::opcode::const_i64:
            out << "  " << dst << " = munx_i64(" << in.i64 << "LL);\n";
            break;
        case mir::opcode::const_f64:
            out << "  " << dst << " = munx_f64(" << std::to_string(in.f64)
                << ");\n";
            break;
        case mir::opcode::const_bool:
            out << "  " << dst << " = munx_bool(" << (in.b ? "true" : "false")
                << ");\n";
            break;
        case mir::opcode::const_null:
            out << "  " << dst << " = munx_null();\n";
            break;
        case mir::opcode::const_str:
            out << "  " << dst << " = S" << in.str_index << ";\n";
            break;
        case mir::opcode::load_local:
            out << "  " << dst << " = locals[" << in.local << "];\n";
            break;
        case mir::opcode::store_local:
            out << "  locals[" << in.local << "] = " << vref(in.args[0])
                << ";\n";
            break;
        case mir::opcode::neg:
            out << "  " << dst << " = munx_neg(" << vref(in.args[0]) << ");\n";
            break;
        case mir::opcode::not_:
            out << "  " << dst << " = munx_not(" << vref(in.args[0]) << ");\n";
            break;
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
            out << "  " << dst << " = " << runtime_binop(in.op) << "("
                << vref(in.args[0]) << ", " << vref(in.args[1]) << ");\n";
            break;
        case mir::opcode::print:
            out << "  munx_print(" << vref(in.args[0]) << ");\n";
            break;
        case mir::opcode::println:
            out << "  munx_println();\n";
            break;
        case mir::opcode::call:
            if (in.callee == "munx_argv_len")
            {
                out << "  " << dst << " = munx_argv_len();\n";
            }
            else if (in.callee == "munx_argv_get")
            {
                out << "  " << dst << " = munx_argv_get(" << vref(in.args[0])
                    << ");\n";
            }
            else if (in.ty == mir::type::void_ || in.callee == "munx_fail" ||
                     in.callee == "munx_sleep" ||
                     in.callee == "munx_pipe_insert" ||
                     in.callee == "munx_channel_insert")
            {
                out << "  " << in.callee << "(";
                for (size_t a = 0; a < in.args.size(); ++a)
                {
                    if (a)
                    {
                        out << ", ";
                    }
                    out << vref(in.args[a]);
                }
                out << ");\n";
            }
            else
            {
                out << "  " << dst << " = " << in.callee << "(";
                for (size_t a = 0; a < in.args.size(); ++a)
                {
                    if (a)
                    {
                        out << ", ";
                    }
                    out << vref(in.args[a]);
                }
                out << ");\n";
            }
            break;
        case mir::opcode::ret:
            if (is_init || in.ty == mir::type::void_)
            {
                out << "  return;\n";
            }
            else if (in.args.empty())
            {
                out << "  return munx_null();\n";
            }
            else
            {
                out << "  return " << vref(in.args[0]) << ";\n";
            }
            break;
        case mir::opcode::br:
            out << "  goto L" << in.block_target << ";\n";
            break;
        case mir::opcode::cbr:
            out << "  if (munx_truthy(" << vref(in.args[0]) << ")) goto L"
                << in.block_target << "; else goto L" << in.block_target_false
                << ";\n";
            break;
        }
    }
    if (is_init)
    {
        out << "}\n\n";
    }
    else
    {
        out << "  return munx_null();\n}\n\n";
    }
}

} // namespace detail

/// Lower optimized MIR to a single C translation unit linked with munx_rt.
inline std::string emit_c(const mir::module &mod)
{
    std::ostringstream out;
    out << "/* generated by munxc --native (custom backend) */\n";
    out << "#include \"munx_rt.h\"\n\n";

    for (uint32_t i = 0; i < mod.strings.size(); ++i)
    {
        out << "static MunxValue S" << i << ";\n";
    }
    if (!mod.strings.empty())
    {
        out << "\nstatic void munx_init_strings(void)\n{\n";
        for (uint32_t i = 0; i < mod.strings.size(); ++i)
        {
            out << "  S" << i << " = munx_string(\""
                << detail::c_escape(mod.strings[i]) << "\");\n";
        }
        out << "}\n\n";
    }
    else
    {
        out << "static void munx_init_strings(void) {}\n\n";
    }

    for (const mir::function &fn : mod.functions)
    {
        if (fn.name.rfind("munx_init_", 0) == 0)
        {
            out << "static void " << fn.name << "(void);\n";
        }
        else
        {
            out << "static MunxValue " << fn.name << "(";
            for (uint32_t i = 0; i < fn.param_count; ++i)
            {
                if (i)
                {
                    out << ", ";
                }
                out << "MunxValue";
            }
            out << ");\n";
        }
    }
    out << "\n";

    for (const mir::function &fn : mod.functions)
    {
        detail::emit_function(out, fn);
    }

    out << "int main(int argc, char **argv)\n{\n";
    // Match VM semantics: argv[0] is the first program argument, not the exe name.
    out << "  munx_set_argv(argc > 0 ? argc - 1 : 0, argc > 0 ? argv + 1 : argv);\n";
    out << "  munx_pipe_session_begin();\n";
    out << "  munx_init_strings();\n";
    for (const mir::function &fn : mod.functions)
    {
        if (fn.name.rfind("munx_init_", 0) == 0 && !fn.is_entry_init)
        {
            out << "  " << fn.name << "();\n";
        }
    }
    for (const mir::function &fn : mod.functions)
    {
        if (fn.is_entry_init)
        {
            out << "  " << fn.name << "();\n";
        }
    }
    out << "  return 0;\n}\n";
    return out.str();
}

} // namespace munx::native::custom
