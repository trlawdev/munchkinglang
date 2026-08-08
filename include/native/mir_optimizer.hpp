#pragma once

#include "mir.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace munx::native::mir
{

namespace detail
{

inline bool is_const(const instr &in)
{
    switch (in.op)
    {
    case opcode::const_i64:
    case opcode::const_f64:
    case opcode::const_bool:
    case opcode::const_null:
    case opcode::const_str:
        return true;
    default:
        return false;
    }
}

inline bool try_fold_binary(instr &out, opcode op, const instr &l, const instr &r)
{
    if (l.op == opcode::const_i64 && r.op == opcode::const_i64)
    {
        const int64_t a = l.i64;
        const int64_t b = r.i64;
        out.ty = type::value;
        switch (op)
        {
        case opcode::add:
            out.op = opcode::const_i64;
            out.i64 = a + b;
            return true;
        case opcode::sub:
            out.op = opcode::const_i64;
            out.i64 = a - b;
            return true;
        case opcode::mul:
            out.op = opcode::const_i64;
            out.i64 = a * b;
            return true;
        case opcode::div:
            if (b == 0)
            {
                return false;
            }
            out.op = opcode::const_i64;
            out.i64 = a / b;
            return true;
        case opcode::mod:
            if (b == 0)
            {
                return false;
            }
            out.op = opcode::const_i64;
            out.i64 = a % b;
            return true;
        case opcode::eq:
            out.op = opcode::const_bool;
            out.b = a == b;
            return true;
        case opcode::ne:
            out.op = opcode::const_bool;
            out.b = a != b;
            return true;
        case opcode::lt:
            out.op = opcode::const_bool;
            out.b = a < b;
            return true;
        case opcode::gt:
            out.op = opcode::const_bool;
            out.b = a > b;
            return true;
        case opcode::le:
            out.op = opcode::const_bool;
            out.b = a <= b;
            return true;
        case opcode::ge:
            out.op = opcode::const_bool;
            out.b = a >= b;
            return true;
        default:
            return false;
        }
    }
    if (l.op == opcode::const_bool && r.op == opcode::const_bool &&
        (op == opcode::eq || op == opcode::ne))
    {
        out.ty = type::value;
        out.op = opcode::const_bool;
        out.b = (op == opcode::eq) ? (l.b == r.b) : (l.b != r.b);
        return true;
    }
    return false;
}

inline void const_fold_function(function &fn)
{
    for (size_t i = 0; i < fn.code.size(); ++i)
    {
        instr &in = fn.code[i];
        switch (in.op)
        {
        case opcode::neg:
            if (in.args.size() == 1 && in.args[0] < fn.code.size() &&
                fn.code[in.args[0]].op == opcode::const_i64)
            {
                const int64_t v = fn.code[in.args[0]].i64;
                in = instr{};
                in.op = opcode::const_i64;
                in.ty = type::value;
                in.i64 = -v;
                in.result = static_cast<uint32_t>(i);
            }
            break;
        case opcode::not_:
            if (in.args.size() == 1 && in.args[0] < fn.code.size() &&
                fn.code[in.args[0]].op == opcode::const_bool)
            {
                const bool v = fn.code[in.args[0]].b;
                in = instr{};
                in.op = opcode::const_bool;
                in.ty = type::value;
                in.b = !v;
                in.result = static_cast<uint32_t>(i);
            }
            break;
        case opcode::add:
        case opcode::sub:
        case opcode::mul:
        case opcode::div:
        case opcode::mod:
        case opcode::eq:
        case opcode::ne:
        case opcode::lt:
        case opcode::gt:
        case opcode::le:
        case opcode::ge:
            if (in.args.size() == 2 && in.args[0] < fn.code.size() &&
                in.args[1] < fn.code.size())
            {
                instr folded;
                if (try_fold_binary(folded, in.op, fn.code[in.args[0]],
                                    fn.code[in.args[1]]))
                {
                    folded.result = static_cast<uint32_t>(i);
                    in = std::move(folded);
                }
            }
            break;
        case opcode::cbr:
            if (in.args.size() == 1 && in.args[0] < fn.code.size() &&
                fn.code[in.args[0]].op == opcode::const_bool)
            {
                const bool t = fn.code[in.args[0]].b;
                const uint32_t target =
                    t ? in.block_target : in.block_target_false;
                in = instr{};
                in.op = opcode::br;
                in.block_target = target;
                in.result = static_cast<uint32_t>(i);
            }
            break;
        default:
            break;
        }
    }
}

inline void jump_thread_function(function &fn)
{
    std::unordered_map<uint32_t, size_t> block_at;
    for (size_t i = 0; i < fn.code.size(); ++i)
    {
        if (fn.code[i].op == opcode::label)
        {
            block_at[fn.code[i].block_target] = i;
        }
    }

    auto resolve = [&](uint32_t block) {
        for (int guard = 0; guard < 32; ++guard)
        {
            const auto it = block_at.find(block);
            if (it == block_at.end())
            {
                return block;
            }
            size_t i = it->second + 1;
            while (i < fn.code.size() && fn.code[i].op == opcode::label)
            {
                ++i;
            }
            if (i < fn.code.size() && fn.code[i].op == opcode::br)
            {
                block = fn.code[i].block_target;
                continue;
            }
            return block;
        }
        return block;
    };

    for (instr &in : fn.code)
    {
        if (in.op == opcode::br)
        {
            in.block_target = resolve(in.block_target);
        }
        else if (in.op == opcode::cbr)
        {
            in.block_target = resolve(in.block_target);
            in.block_target_false = resolve(in.block_target_false);
        }
    }
}

inline bool has_side_effect(opcode op)
{
    switch (op)
    {
    case opcode::store_local:
    case opcode::print:
    case opcode::println:
    case opcode::call:
    case opcode::ret:
    case opcode::br:
    case opcode::cbr:
    case opcode::label:
        return true;
    default:
        return false;
    }
}

inline void dce_function(function &fn)
{
    std::unordered_set<uint32_t> used;
    for (const instr &in : fn.code)
    {
        if (has_side_effect(in.op) || in.op == opcode::load_local)
        {
            for (uint32_t a : in.args)
            {
                used.insert(a);
            }
        }
    }
    // Mark producers of used values transitively (simple fixed-point).
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (uint32_t id : std::vector<uint32_t>(used.begin(), used.end()))
        {
            if (id >= fn.code.size())
            {
                continue;
            }
            for (uint32_t a : fn.code[id].args)
            {
                if (used.insert(a).second)
                {
                    changed = true;
                }
            }
        }
    }

    for (size_t i = 0; i < fn.code.size(); ++i)
    {
        instr &in = fn.code[i];
        if (has_side_effect(in.op) || is_const(in))
        {
            continue;
        }
        if (!used.contains(static_cast<uint32_t>(i)) &&
            in.op != opcode::load_local)
        {
            // Replace pure unused ops with null const (keeps SSA indices stable).
            const uint32_t result = in.result;
            in = instr{};
            in.op = opcode::const_null;
            in.ty = type::value;
            in.result = result;
        }
    }
}

} // namespace detail

/// Run deterministic MIR opts used by the custom native backend.
inline module optimize_mir(module mod)
{
    for (function &fn : mod.functions)
    {
        detail::const_fold_function(fn);
        detail::jump_thread_function(fn);
        detail::dce_function(fn);
    }
    return mod;
}

} // namespace munx::native::mir
