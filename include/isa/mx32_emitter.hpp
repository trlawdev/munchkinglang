#pragma once

#include "../errors.hpp"
#include "../native/mir.hpp"
#include "mx32.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace munx::isa
{

namespace mir = munx::native::mir;

namespace detail
{

inline uint32_t pool_i64(mx_module &m, int64_t v)
{
    for (uint32_t i = 0; i < m.pool.size(); ++i)
    {
        if (m.pool[i].tag == pool_tag::i64 && m.pool[i].i64 == v)
        {
            return i;
        }
    }
    pool_entry e;
    e.tag = pool_tag::i64;
    e.i64 = v;
    m.pool.push_back(std::move(e));
    return static_cast<uint32_t>(m.pool.size() - 1);
}

inline uint32_t pool_f64(mx_module &m, double v)
{
    pool_entry e;
    e.tag = pool_tag::f64;
    e.f64 = v;
    m.pool.push_back(std::move(e));
    return static_cast<uint32_t>(m.pool.size() - 1);
}

inline uint32_t pool_str(mx_module &m, const std::string &s)
{
    for (uint32_t i = 0; i < m.pool.size(); ++i)
    {
        if (m.pool[i].tag == pool_tag::string && m.pool[i].str == s)
        {
            return i;
        }
    }
    pool_entry e;
    e.tag = pool_tag::string;
    e.str = s;
    m.pool.push_back(std::move(e));
    return static_cast<uint32_t>(m.pool.size() - 1);
}

inline uint32_t pool_bool(mx_module &m, bool b)
{
    for (uint32_t i = 0; i < m.pool.size(); ++i)
    {
        if (m.pool[i].tag == pool_tag::boolean && m.pool[i].b == b)
        {
            return i;
        }
    }
    pool_entry e;
    e.tag = pool_tag::boolean;
    e.b = b;
    m.pool.push_back(std::move(e));
    return static_cast<uint32_t>(m.pool.size() - 1);
}

inline uint32_t pool_null(mx_module &m)
{
    for (uint32_t i = 0; i < m.pool.size(); ++i)
    {
        if (m.pool[i].tag == pool_tag::null)
        {
            return i;
        }
    }
    pool_entry e;
    e.tag = pool_tag::null;
    m.pool.push_back(std::move(e));
    return static_cast<uint32_t>(m.pool.size() - 1);
}

struct emit_fn
{
    mx_module *mod{nullptr};
    uint32_t n_locals{0};
    uint32_t n_vals{0};
    std::vector<uint32_t> code;
    std::unordered_map<uint32_t, uint32_t> label_insn; // block → insn index
    struct patch
    {
        uint32_t at;
        uint32_t block;
        mx_op op;
        uint32_t reg;
    };
    std::vector<patch> patches;

    uint32_t slot_local(uint32_t i) const { return i; }
    uint32_t slot_val(uint32_t i) const { return n_locals + i; }

    // Scratch regs
    static constexpr uint32_t R_A = 8;
    static constexpr uint32_t R_B = 9;
    static constexpr uint32_t R_C = 10;
    static constexpr uint32_t R_T = 11;

    void emit(uint32_t w) { code.push_back(w); }

    void load_slot(uint32_t rd, uint32_t slot)
    {
        // LD_SLOT rd, r0, slot  — base r0=0, imm = slot (imm14)
        emit(enc_i(mx_op::LD_SLOT, rd, k_r0, slot & 0x3fff));
    }

    void store_slot(uint32_t slot, uint32_t rs)
    {
        // ST_SLOT: base in rd field, value in rs1, imm14 = slot
        emit(enc_i(mx_op::ST_SLOT, k_r0, rs, slot & 0x3fff));
    }

    void load_val(uint32_t rd, uint32_t vid) { load_slot(rd, slot_val(vid)); }
    void store_val(uint32_t vid, uint32_t rs) { store_slot(slot_val(vid), rs); }

    mx_op bin_op(mir::opcode op)
    {
        switch (op)
        {
        case mir::opcode::add:
            return mx_op::ADD;
        case mir::opcode::sub:
            return mx_op::SUB;
        case mir::opcode::mul:
            return mx_op::MUL;
        case mir::opcode::div:
            return mx_op::DIV;
        case mir::opcode::mod:
            return mx_op::MOD;
        case mir::opcode::eq:
            return mx_op::EQ;
        case mir::opcode::ne:
            return mx_op::NE;
        case mir::opcode::lt:
            return mx_op::LT;
        case mir::opcode::gt:
            return mx_op::GT;
        case mir::opcode::le:
            return mx_op::LE;
        case mir::opcode::ge:
            return mx_op::GE;
        default:
            return mx_op::NOP;
        }
    }

    void lower(const mir::function &fn, const mir::module &mir_mod)
    {
        n_locals = fn.local_count;
        n_vals = static_cast<uint32_t>(fn.code.size());

        // params already in slots 0..param_count-1 (caller convention)
        for (size_t ii = 0; ii < fn.code.size(); ++ii)
        {
            const mir::instr &in = fn.code[ii];
            const uint32_t dst = static_cast<uint32_t>(ii);
            switch (in.op)
            {
            case mir::opcode::label:
                label_insn[in.block_target] = static_cast<uint32_t>(code.size());
                break;
            case mir::opcode::const_i64:
            {
                const uint32_t idx = pool_i64(*mod, in.i64);
                emit(enc_i(mx_op::LDC, R_T, 0, idx));
                store_val(dst, R_T);
                break;
            }
            case mir::opcode::const_f64:
            {
                const uint32_t idx = pool_f64(*mod, in.f64);
                emit(enc_i(mx_op::LDC, R_T, 0, idx));
                store_val(dst, R_T);
                break;
            }
            case mir::opcode::const_bool:
            {
                const uint32_t idx = pool_bool(*mod, in.b);
                emit(enc_i(mx_op::LDC, R_T, 0, idx));
                store_val(dst, R_T);
                break;
            }
            case mir::opcode::const_null:
            {
                const uint32_t idx = pool_null(*mod);
                emit(enc_i(mx_op::LDC, R_T, 0, idx));
                store_val(dst, R_T);
                break;
            }
            case mir::opcode::const_str:
            {
                const uint32_t idx =
                    pool_str(*mod, mir_mod.strings[in.str_index]);
                emit(enc_i(mx_op::LDC, R_T, 0, idx));
                store_val(dst, R_T);
                break;
            }
            case mir::opcode::load_local:
                load_slot(R_T, slot_local(in.local));
                store_val(dst, R_T);
                break;
            case mir::opcode::store_local:
                load_val(R_T, in.args[0]);
                store_slot(slot_local(in.local), R_T);
                break;
            case mir::opcode::neg:
                load_val(R_A, in.args[0]);
                emit(enc_r(mx_op::NEG, R_T, R_A, 0));
                store_val(dst, R_T);
                break;
            case mir::opcode::not_:
                load_val(R_A, in.args[0]);
                emit(enc_r(mx_op::NOT, R_T, R_A, 0));
                store_val(dst, R_T);
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
                load_val(R_A, in.args[0]);
                load_val(R_B, in.args[1]);
                emit(enc_r(bin_op(in.op), R_T, R_A, R_B));
                store_val(dst, R_T);
                break;
            case mir::opcode::print:
                load_val(R_A, in.args[0]);
                emit(enc_r(mx_op::PRINT, 0, R_A, 0));
                break;
            case mir::opcode::println:
                emit(enc_r(mx_op::PRINTLN, 0, 0, 0));
                break;
            case mir::opcode::call:
            {
                if (in.callee == "munx_argv_len")
                {
                    emit(enc_r(mx_op::ARGV_LEN, R_T, 0, 0));
                    store_val(dst, R_T);
                    break;
                }
                if (in.callee == "munx_argv_get")
                {
                    load_val(R_A, in.args[0]);
                    emit(enc_r(mx_op::ARGV_GET, R_T, R_A, 0));
                    store_val(dst, R_T);
                    break;
                }
                // Move args into R1..Rk, call by name index in pool as string
                if (in.args.size() > 8)
                {
                    fail_compile("mx32: too many call args");
                    return;
                }
                for (size_t a = 0; a < in.args.size(); ++a)
                {
                    load_val(static_cast<uint32_t>(1 + a), in.args[a]);
                }
                const uint32_t name_idx = pool_str(*mod, in.callee);
                emit(enc_i(mx_op::LDC, R_C, 0, name_idx)); // callee name as string value
                emit(enc_r(mx_op::CALL, R_T, R_C, 0, static_cast<uint32_t>(in.args.size())));
                if (in.ty != mir::type::void_)
                {
                    store_val(dst, R_T);
                }
                break;
            }
            case mir::opcode::ret:
                if (fn.name.rfind("munx_init_", 0) == 0)
                {
                    // Init frames are entered by JMP, not CALL — end with HALT.
                    emit(enc_r(mx_op::HALT, 0, 0, 0));
                }
                else if (!in.args.empty())
                {
                    load_val(k_r0, in.args[0]);
                    emit(enc_r(mx_op::RET, 0, 0, 0));
                }
                else
                {
                    const uint32_t idx = pool_null(*mod);
                    emit(enc_i(mx_op::LDC, k_r0, 0, idx));
                    emit(enc_r(mx_op::RET, 0, 0, 0));
                }
                break;
            case mir::opcode::br:
                patches.push_back({static_cast<uint32_t>(code.size()), in.block_target,
                                   mx_op::JMP, 0});
                emit(enc_b(mx_op::JMP, 0, 0));
                break;
            case mir::opcode::cbr:
                if (in.branch_hint == 1)
                {
                    emit(enc_u(mx_op::HINT, 0, 1));
                }
                else if (in.branch_hint == 2)
                {
                    emit(enc_u(mx_op::HINT, 0, 0));
                }
                load_val(R_A, in.args[0]);
                patches.push_back({static_cast<uint32_t>(code.size()), in.block_target,
                                   mx_op::BR_TRUE, R_A});
                emit(enc_b(mx_op::BR_TRUE, R_A, 0));
                patches.push_back({static_cast<uint32_t>(code.size()),
                                   in.block_target_false, mx_op::JMP, 0});
                emit(enc_b(mx_op::JMP, 0, 0));
                break;
            }
        }

        // fallthrough
        if (fn.name.rfind("munx_init_", 0) == 0)
        {
            emit(enc_r(mx_op::HALT, 0, 0, 0));
        }
        else
        {
            const uint32_t idx = pool_null(*mod);
            emit(enc_i(mx_op::LDC, k_r0, 0, idx));
            emit(enc_r(mx_op::RET, 0, 0, 0));
        }

        for (const auto &p : patches)
        {
            const auto it = label_insn.find(p.block);
            if (it == label_insn.end())
            {
                fail_compile("mx32: missing label");
                return;
            }
            const int32_t delta =
                static_cast<int32_t>(it->second) - static_cast<int32_t>(p.at);
            code[p.at] = enc_b(p.op, p.reg, delta);
        }
    }
};

} // namespace detail

/// Lower optimized MIR module to an in-memory mx32 module.
inline mx_module emit_mx32(const mir::module &mir_mod)
{
    mx_module out;
    std::unordered_map<std::string, uint32_t> fn_entry;

    // Trampoline at word 0 so fetch never falls into it after HALT.
    out.entry_pc = 0;
    out.code.push_back(enc_b(mx_op::JMP, 0, 0));

    for (const mir::function &fn : mir_mod.functions)
    {
        detail::emit_fn e;
        e.mod = &out;
        const uint32_t entry = static_cast<uint32_t>(out.code.size());
        e.lower(fn, mir_mod);
        out.code.insert(out.code.end(), e.code.begin(), e.code.end());

        mx_module::fn fdesc;
        fdesc.name = fn.name;
        fdesc.entry = entry;
        fdesc.param_count = fn.param_count;
        fdesc.local_slots = fn.local_count + static_cast<uint32_t>(fn.code.size());
        fn_entry[fn.name] = entry;
        out.functions.push_back(std::move(fdesc));
    }

    std::vector<uint32_t> init_entries;
    for (const mir::function &fn : mir_mod.functions)
    {
        if (fn.name.rfind("munx_init_", 0) == 0 && !fn.is_entry_init)
        {
            init_entries.push_back(fn_entry[fn.name]);
        }
    }
    for (const mir::function &fn : mir_mod.functions)
    {
        if (fn.is_entry_init)
        {
            init_entries.push_back(fn_entry[fn.name]);
        }
    }

    if (init_entries.empty())
    {
        out.code[0] = enc_r(mx_op::HALT, 0, 0, 0);
        return out;
    }

    for (size_t i = 0; i + 1 < init_entries.size(); ++i)
    {
        const uint32_t from = init_entries[i];
        const uint32_t to = init_entries[i + 1];
        const uint32_t next_start = init_entries[i + 1];
        uint32_t halt_at = next_start;
        for (uint32_t pc = next_start; pc > from;)
        {
            --pc;
            if (dec_op(out.code[pc]) == mx_op::HALT)
            {
                halt_at = pc;
                break;
            }
        }
        out.code[halt_at] = enc_b(
            mx_op::JMP, 0,
            static_cast<int32_t>(to) - static_cast<int32_t>(halt_at));
    }

    out.code[0] =
        enc_b(mx_op::JMP, 0, static_cast<int32_t>(init_entries.front()));
    return out;
}

} // namespace munx::isa
