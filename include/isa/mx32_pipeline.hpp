#pragma once

#include "../jit/ooo.hpp"
#include "../jit/vm_pipeline.hpp"
#include "mx32_cpu.hpp"

#include <cmath>
#include <cstdlib>

namespace munx::isa
{

using jit_uop = vm::jit::uop;

inline jit_uop decode_uop(uint32_t w, uint32_t pc)
{
    jit_uop u;
    u.op = dec_op(w);
    u.pc = pc;
    u.arch_rd = dec_rd(w);
    u.arch_rs1 = dec_rs1(w);
    u.arch_rs2 = dec_rs2(w);
    u.imm = dec_imm19_signed(w);
    u.imm_u = dec_imm14(w);
    switch (u.op)
    {
    case mx_op::MOV:
    case mx_op::LDC:
    case mx_op::LI:
    case mx_op::ADD:
    case mx_op::SUB:
    case mx_op::MUL:
    case mx_op::DIV:
    case mx_op::MOD:
    case mx_op::NEG:
    case mx_op::NOT:
    case mx_op::EQ:
    case mx_op::NE:
    case mx_op::LT:
    case mx_op::GT:
    case mx_op::LE:
    case mx_op::GE:
    case mx_op::LD_SLOT:
    case mx_op::ARGV_LEN:
    case mx_op::ARGV_GET:
    case mx_op::CALL:
        u.has_rd = true;
        break;
    default:
        u.has_rd = false;
        break;
    }
    switch (u.op)
    {
    case mx_op::PRINT:
    case mx_op::PRINTLN:
    case mx_op::CALL:
    case mx_op::RET:
    case mx_op::HALT:
        u.serialize = true;
        u.fu = vm::jit::fu_kind::runtime;
        break;
    case mx_op::JMP:
    case mx_op::BR_TRUE:
    case mx_op::BR_FALSE:
        u.fu = vm::jit::fu_kind::branch;
        break;
    case mx_op::LD_SLOT:
        u.fu = vm::jit::fu_kind::mem;
        u.is_load = true;
        u.mem_addr_known = true;
        u.mem_slot = u.imm_u;
        break;
    case mx_op::ST_SLOT:
        u.fu = vm::jit::fu_kind::mem;
        u.is_store = true;
        u.mem_addr_known = true;
        u.mem_slot = u.imm_u;
        break;
    default:
        u.fu = vm::jit::fu_kind::alu;
        break;
    }
    return u;
}

/// Shared execute for a decoded uop against a frame's register/slot state.
inline void exec_uop_on_frame(mx_cpu &cpu, mx_frame &f, const jit_uop &u, bool &branched,
                              bool &halt, uint32_t &next_pc)
{
    branched = false;
    halt = false;
    next_pc = u.pc + 1;
    const mx_op op = u.op;
    const uint32_t rd = u.arch_rd;
    const uint32_t rs1 = u.arch_rs1;
    const uint32_t rs2 = u.arch_rs2;

    switch (op)
    {
    case mx_op::NOP:
        break;
    case mx_op::HALT:
        halt = true;
        break;
    case mx_op::HINT:
        cpu.next_branch_hint = static_cast<int8_t>(u.imm != 0);
        break;
    case mx_op::MOV:
        f.regs[rd] = f.regs[rs1];
        break;
    case mx_op::LDC:
        f.regs[rd] = cpu.pool_value(u.imm_u);
        break;
    case mx_op::LI:
        f.regs[rd] = vm::value{static_cast<int64_t>(u.imm)};
        break;
    case mx_op::ADD:
    case mx_op::SUB:
    case mx_op::MUL:
    case mx_op::DIV:
    case mx_op::MOD:
        f.regs[rd] = cpu.bin_arith(op, f.regs[rs1], f.regs[rs2]);
        break;
    case mx_op::NEG:
        if (mx_cpu::is_int(f.regs[rs1]))
        {
            f.regs[rd] = vm::value{-mx_cpu::as_int(f.regs[rs1])};
        }
        else
        {
            f.regs[rd] = vm::value{-mx_cpu::as_num(f.regs[rs1])};
        }
        break;
    case mx_op::NOT:
        f.regs[rd] = vm::value{!mx_cpu::truthy(f.regs[rs1])};
        break;
    case mx_op::EQ:
    case mx_op::NE:
    case mx_op::LT:
    case mx_op::GT:
    case mx_op::LE:
    case mx_op::GE:
        f.regs[rd] = cpu.bin_cmp(op, f.regs[rs1], f.regs[rs2]);
        break;
    case mx_op::LD_SLOT:
        if (u.imm_u < f.slots.size())
        {
            f.regs[rd] = f.slots[u.imm_u];
        }
        break;
    case mx_op::ST_SLOT:
        if (u.imm_u >= f.slots.size())
        {
            f.slots.resize(u.imm_u + 1);
        }
        f.slots[u.imm_u] = f.regs[rs1];
        break;
    case mx_op::PRINT:
        std::cout << vm::to_display_string(f.regs[rs1]);
        break;
    case mx_op::PRINTLN:
        std::cout << '\n';
        break;
    case mx_op::JMP:
        next_pc = static_cast<uint32_t>(static_cast<int32_t>(u.pc) + u.imm);
        branched = true;
        break;
    case mx_op::BR_TRUE:
    {
        const bool taken = mx_cpu::truthy(f.regs[rd]);
        const uint32_t target =
            static_cast<uint32_t>(static_cast<int32_t>(u.pc) + u.imm);
        auto &pred = vm::jit::branch_predictor::instance();
        if (cpu.next_branch_hint >= 0)
        {
            pred.seed_static_hint(u.pc, cpu.next_branch_hint != 0);
            cpu.next_branch_hint = -1;
        }
        pred.train(u.pc, taken, target);
        if (taken)
        {
            next_pc = target;
            branched = true;
        }
        break;
    }
    case mx_op::BR_FALSE:
    {
        const bool taken = !mx_cpu::truthy(f.regs[rd]);
        const uint32_t target =
            static_cast<uint32_t>(static_cast<int32_t>(u.pc) + u.imm);
        auto &pred = vm::jit::branch_predictor::instance();
        if (cpu.next_branch_hint >= 0)
        {
            pred.seed_static_hint(u.pc, cpu.next_branch_hint != 0);
            cpu.next_branch_hint = -1;
        }
        pred.train(u.pc, taken, target);
        if (taken)
        {
            next_pc = target;
            branched = true;
        }
        break;
    }
    case mx_op::ARGV_LEN:
        f.regs[rd] = vm::value{static_cast<int64_t>(cpu.argv.size())};
        break;
    case mx_op::ARGV_GET:
    {
        const int64_t idx = mx_cpu::as_int(f.regs[rs1]);
        if (idx >= 0 && static_cast<size_t>(idx) < cpu.argv.size())
        {
            f.regs[rd] = vm::value{cpu.argv[static_cast<size_t>(idx)]};
        }
        else
        {
            f.regs[rd] = vm::value{};
        }
        break;
    }
    case mx_op::CALL:
    case mx_op::RET:
        // Complex control — fall back by setting pc and using scalar step path
        f.pc = u.pc;
        {
            // Use scalar for call/ret to keep stack discipline exact
            const uint32_t saved = f.pc;
            (void)saved;
        }
        break;
    default:
        break;
    }
}

inline bool module_has_call(const mx_module &m)
{
    for (uint32_t w : m.code)
    {
        const mx_op op = dec_op(w);
        if (op == mx_op::CALL || op == mx_op::RET)
        {
            return true;
        }
    }
    return false;
}

inline void mx_cpu::run_inorder()
{
    // Calls need a fully drained register file; until wide drain is complete,
    // modules with CALL/RET run scalar (ALU/branch programs use the 5-stage).
    if (module_has_call(*mod))
    {
        while (step_scalar())
        {
        }
        return;
    }

    vm::jit::inorder_pipeline pipe;
    pipe.reset();
    uint32_t fetch_pc = frame().pc;

    auto flush = [&](uint32_t pc) {
        pipe.reset();
        fetch_pc = pc;
        if (!call_stack.empty())
        {
            frame().pc = pc;
        }
    };

    while (!halted && !call_stack.empty())
    {
        mx_frame &f = frame();
        // Drain pipeline before CALL/RET/HALT so f.pc matches the control op.
        auto peek_at = [&](uint32_t pc) -> mx_op {
            return pc < mod->code.size() ? dec_op(mod->code[pc]) : mx_op::HALT;
        };
        const mx_op fetch_peek = peek_at(fetch_pc);
        const mx_op id_peek =
            pipe.if_id.valid ? dec_op(pipe.if_id.insn) : mx_op::NOP;
        if (fetch_peek == mx_op::CALL || fetch_peek == mx_op::RET ||
            fetch_peek == mx_op::HALT || id_peek == mx_op::CALL ||
            id_peek == mx_op::RET || id_peek == mx_op::HALT)
        {
            // Finish in-flight WB/MEM/EX
            if (pipe.mem_wb.valid && pipe.mem_wb.has_rd)
            {
                f.regs[pipe.mem_wb.op.arch_rd] = pipe.mem_wb.result;
            }
            if (pipe.ex_mem.valid)
            {
                if (pipe.ex_mem.is_store)
                {
                    if (pipe.ex_mem.store_slot >= f.slots.size())
                    {
                        f.slots.resize(pipe.ex_mem.store_slot + 1);
                    }
                    f.slots[pipe.ex_mem.store_slot] = pipe.ex_mem.store_val;
                }
                else if (pipe.ex_mem.op.has_rd)
                {
                    f.regs[pipe.ex_mem.op.arch_rd] = pipe.ex_mem.result;
                }
            }
            uint32_t ctrl_pc = fetch_pc;
            if (pipe.if_id.valid &&
                (id_peek == mx_op::CALL || id_peek == mx_op::RET ||
                 id_peek == mx_op::HALT))
            {
                ctrl_pc = pipe.if_id.pc;
            }
            pipe.reset();
            f.pc = ctrl_pc;
            if (!step_scalar())
            {
                break;
            }
            fetch_pc = call_stack.empty() ? 0 : frame().pc;
            continue;
        }

        // WB
        if (pipe.mem_wb.valid && pipe.mem_wb.has_rd)
        {
            f.regs[pipe.mem_wb.op.arch_rd] = pipe.mem_wb.result;
        }
        pipe.mem_wb = {};

        // MEM → WB
        if (pipe.ex_mem.valid)
        {
            if (pipe.ex_mem.mispredict)
            {
                flush(pipe.ex_mem.branch_target);
                continue;
            }
            if (pipe.ex_mem.is_store)
            {
                if (pipe.ex_mem.store_slot >= f.slots.size())
                {
                    f.slots.resize(pipe.ex_mem.store_slot + 1);
                }
                f.slots[pipe.ex_mem.store_slot] = pipe.ex_mem.store_val;
            }
            pipe.mem_wb.valid = true;
            pipe.mem_wb.op = pipe.ex_mem.op;
            pipe.mem_wb.result = pipe.ex_mem.result;
            pipe.mem_wb.has_rd = pipe.ex_mem.op.has_rd && !pipe.ex_mem.is_store;
            if (pipe.ex_mem.branch_taken)
            {
                fetch_pc = pipe.ex_mem.branch_target;
                pipe.if_id = {};
                pipe.id_ex = {};
            }
        }
        pipe.ex_mem = {};

        // EX
        if (pipe.id_ex.valid)
        {
            const jit_uop &u = pipe.id_ex.op;
            vm::jit::stage_ex_mem out{};
            out.valid = true;
            out.op = u;
            out.result = pipe.id_ex.v_rs1;

            // Forward from mem_wb
            vm::value a = pipe.id_ex.v_rs1;
            vm::value b = pipe.id_ex.v_rs2;
            if (pipe.mem_wb.valid && pipe.mem_wb.has_rd)
            {
                if (u.arch_rs1 == pipe.mem_wb.op.arch_rd)
                {
                    a = pipe.mem_wb.result;
                }
                if (u.arch_rs2 == pipe.mem_wb.op.arch_rd)
                {
                    b = pipe.mem_wb.result;
                }
            }

            switch (u.op)
            {
            case mx_op::MOV:
            case mx_op::LDC:
            case mx_op::LI:
            case mx_op::LD_SLOT:
            case mx_op::ARGV_LEN:
            case mx_op::ARGV_GET:
                // re-exec using frame (regs already hold sources for complex)
                {
                    bool br = false, halt = false;
                    uint32_t npc = 0;
                    // write sources into temps
                    f.regs[u.arch_rs1] = a;
                    f.regs[u.arch_rs2] = b;
                    exec_uop_on_frame(*this, f, u, br, halt, npc);
                    out.result = f.regs[u.arch_rd];
                    if (halt)
                    {
                        halted = true;
                    }
                    if (br)
                    {
                        out.branch_taken = true;
                        out.branch_target = npc;
                    }
                }
                break;
            case mx_op::ADD:
            case mx_op::SUB:
            case mx_op::MUL:
            case mx_op::DIV:
            case mx_op::MOD:
                out.result = bin_arith(u.op, a, b);
                break;
            case mx_op::EQ:
            case mx_op::NE:
            case mx_op::LT:
            case mx_op::GT:
            case mx_op::LE:
            case mx_op::GE:
                out.result = bin_cmp(u.op, a, b);
                break;
            case mx_op::NEG:
                out.result = mx_cpu::is_int(a) ? vm::value{-mx_cpu::as_int(a)}
                                               : vm::value{-mx_cpu::as_num(a)};
                break;
            case mx_op::NOT:
                out.result = vm::value{!truthy(a)};
                break;
            case mx_op::ST_SLOT:
                out.is_store = true;
                out.store_slot = u.imm_u;
                out.store_val = a;
                break;
            case mx_op::PRINT:
                std::cout << vm::to_display_string(a);
                break;
            case mx_op::PRINTLN:
                std::cout << '\n';
                break;
            case mx_op::JMP:
                out.branch_taken = true;
                out.branch_target =
                    static_cast<uint32_t>(static_cast<int32_t>(u.pc) + u.imm);
                break;
            case mx_op::BR_TRUE:
            case mx_op::BR_FALSE:
            {
                // Condition in arch_rd loaded as v_rs1 for branches we packed rd
                const bool cond = truthy(f.regs[u.arch_rd]);
                const bool taken = (u.op == mx_op::BR_TRUE) ? cond : !cond;
                const uint32_t target =
                    static_cast<uint32_t>(static_cast<int32_t>(u.pc) + u.imm);
                const uint32_t fall = u.pc + 1;
                auto &pred = vm::jit::branch_predictor::instance();
                if (next_branch_hint >= 0)
                {
                    pred.seed_static_hint(u.pc, next_branch_hint != 0);
                    next_branch_hint = -1;
                }
                const bool predicted = pred.predict(u.pc, target, fall);
                pred.train(u.pc, taken, target);
                const uint32_t actual = taken ? target : fall;
                if (predicted != taken)
                {
                    out.mispredict = true;
                    out.branch_target = actual;
                }
                else if (taken)
                {
                    out.branch_taken = true;
                    out.branch_target = target;
                }
                break;
            }
            case mx_op::HINT:
                next_branch_hint = static_cast<int8_t>(u.imm != 0);
                break;
            default:
                break;
            }
            pipe.ex_mem = out;
        }
        pipe.id_ex = {};

        // ID — stall on load-use if prior is LD_SLOT to same reg
        if (pipe.if_id.valid)
        {
            jit_uop u = decode_uop(pipe.if_id.insn, pipe.if_id.pc);
            // load-use stall
            if (pipe.ex_mem.valid && pipe.ex_mem.op.op == mx_op::LD_SLOT &&
                pipe.ex_mem.op.has_rd &&
                (u.arch_rs1 == pipe.ex_mem.op.arch_rd ||
                 u.arch_rs2 == pipe.ex_mem.op.arch_rd ||
                 ((u.op == mx_op::BR_TRUE || u.op == mx_op::BR_FALSE) &&
                  u.arch_rd == pipe.ex_mem.op.arch_rd)))
            {
                pipe.stall_fetch = true;
            }
            else
            {
                pipe.id_ex.valid = true;
                pipe.id_ex.op = u;
                pipe.id_ex.v_rs1 = f.regs[u.arch_rs1];
                pipe.id_ex.v_rs2 = f.regs[u.arch_rs2];
                // For BR_*, condition uses rd field
                if (u.op == mx_op::BR_TRUE || u.op == mx_op::BR_FALSE)
                {
                    pipe.id_ex.v_rs1 = f.regs[u.arch_rd];
                }
                if (u.op == mx_op::ST_SLOT)
                {
                    pipe.id_ex.v_rs1 = f.regs[u.arch_rs1];
                }
                if (u.op == mx_op::LDC)
                {
                    pipe.id_ex.v_rs1 = pool_value(u.imm_u);
                    f.regs[u.arch_rd] = pipe.id_ex.v_rs1; // early for simplicity
                }
                pipe.if_id = {};
                pipe.stall_fetch = false;
                f.pc = u.pc + 1;
            }
        }

        // IF
        if (!pipe.stall_fetch && !pipe.if_id.valid)
        {
            if (fetch_pc >= mod->code.size())
            {
                if (!pipe.id_ex.valid && !pipe.ex_mem.valid && !pipe.mem_wb.valid)
                {
                    halted = true;
                    break;
                }
            }
            else
            {
                pipe.if_id.valid = true;
                pipe.if_id.pc = fetch_pc;
                pipe.if_id.insn = mod->code[fetch_pc];
                // speculative PC
                const mx_op fop = dec_op(pipe.if_id.insn);
                if (fop == mx_op::JMP)
                {
                    fetch_pc = static_cast<uint32_t>(
                        static_cast<int32_t>(pipe.if_id.pc) +
                        dec_imm19_signed(pipe.if_id.insn));
                }
                else if (fop == mx_op::BR_TRUE || fop == mx_op::BR_FALSE)
                {
                    const uint32_t target = static_cast<uint32_t>(
                        static_cast<int32_t>(pipe.if_id.pc) +
                        dec_imm19_signed(pipe.if_id.insn));
                    const uint32_t fall = pipe.if_id.pc + 1;
                    const bool predicted =
                        vm::jit::branch_predictor::instance().predict(
                            pipe.if_id.pc, target, fall);
                    fetch_pc = predicted ? target : fall;
                }
                else
                {
                    fetch_pc = pipe.if_id.pc + 1;
                }
            }
        }
        else
        {
            pipe.stall_fetch = false;
        }
    }
}

inline void mx_cpu::run_ooo()
{
    // Full ROB path: enable with MUNX_VM_OOO_ROB=1. Default ooo uses scalar
    // retire for correctness while rename/mem-disambig land continues.
    const char *rob = std::getenv("MUNX_VM_OOO_ROB");
    if (rob == nullptr || rob[0] != '1')
    {
        while (step_scalar())
        {
        }
        return;
    }

    // OoO with rename/ROB: issue when operands ready; commit in order.
    // CALL/RET are serialize ops issued only at ROB head (precise frames).
    // Loads/stores use address-known slot disambiguation to skip false deps.
    vm::jit::ooo_state ooo;
    ooo.reset();
    for (uint32_t i = 0; i < k_arch_regs; ++i)
    {
        ooo.phys_rf[i] = frame().regs[i];
        ooo.phys_ready[i] = true;
    }

    uint32_t fetch_pc = frame().pc;
    bool fetch_hold = false; // parked on CALL/RET/HALT until it retires

    auto sync_arch_from_phys = [&]() {
        if (call_stack.empty())
        {
            return;
        }
        for (uint32_t i = 0; i < k_arch_regs; ++i)
        {
            frame().regs[i] = ooo.phys_rf[ooo.arch_rat[i]];
        }
    };

    auto reset_rename_from_frame = [&]() {
        ooo.reset();
        fetch_hold = false;
        if (call_stack.empty())
        {
            return;
        }
        for (uint32_t i = 0; i < k_arch_regs; ++i)
        {
            ooo.phys_rf[i] = frame().regs[i];
            ooo.phys_ready[i] = true;
        }
    };

    /// Older incomplete mem ops that may alias block issue.
    auto mem_dep_clear = [&](uint32_t rob_i, const jit_uop &cand) -> bool {
        if (!cand.is_load && !cand.is_store)
        {
            return true;
        }
        uint32_t idx = ooo.rob_head;
        for (uint32_t n = 0; n < ooo.rob_count; ++n)
        {
            if (idx == rob_i)
            {
                break;
            }
            const auto &older = ooo.rob[idx];
            if (older.valid && !older.complete)
            {
                const bool older_store = older.decoded.is_store;
                const bool older_load = older.decoded.is_load;
                // RAW: load after store; WAR: store after load; WAW: store after store
                const bool conflict =
                    (cand.is_load && older_store) ||
                    (cand.is_store && older_load) ||
                    (cand.is_store && older_store);
                if (conflict && vm::jit::may_alias_mem(cand, older.decoded))
                {
                    return false;
                }
            }
            idx = (idx + 1) % vm::jit::k_rob_size;
        }
        return true;
    };

    auto squash_younger_than = [&](uint32_t rob_i) {
        // Free everything after rob_i; leave rob_i as head to commit next.
        while (ooo.rob_count > 0)
        {
            const uint32_t last =
                (ooo.rob_head + ooo.rob_count - 1) % vm::jit::k_rob_size;
            if (last == rob_i)
            {
                break;
            }
            auto &e = ooo.rob[last];
            if (e.has_dest)
            {
                ooo.free_phys(e.dest_phys);
                ooo.rat[e.arch_rd] = e.old_phys;
            }
            e = {};
            --ooo.rob_count;
            ooo.rob_tail = last;
        }
        for (auto &r : ooo.rs)
        {
            if (r.valid && r.rob_id != rob_i)
            {
                // drop rs not for this rob
                bool keep = false;
                for (uint32_t n = 0, idx = ooo.rob_head; n < ooo.rob_count;
                     ++n, idx = (idx + 1) % vm::jit::k_rob_size)
                {
                    if (idx == r.rob_id)
                    {
                        keep = true;
                        break;
                    }
                }
                if (!keep)
                {
                    r = {};
                }
            }
        }
        ooo.rat = ooo.arch_rat;
    };

    int idle_cycles = 0;
    while (!halted && !call_stack.empty())
    {
        mx_frame &f = frame();
        bool progressed = false;

        // Commit ROB head
        while (ooo.rob_count > 0 && ooo.rob[ooo.rob_head].valid &&
               ooo.rob[ooo.rob_head].complete)
        {
            progressed = true;
            auto &h = ooo.rob[ooo.rob_head];
            if (h.has_dest)
            {
                ooo.arch_rat[h.arch_rd] = h.dest_phys;
                f.regs[h.arch_rd] = ooo.phys_rf[h.dest_phys];
                ooo.free_phys(h.old_phys);
            }
            if (h.decoded.op == mx_op::ST_SLOT)
            {
                const uint32_t slot = h.decoded.imm_u;
                if (slot >= f.slots.size())
                {
                    f.slots.resize(slot + 1);
                }
                f.slots[slot] = h.result;
            }
            if (h.decoded.op == mx_op::PRINT)
            {
                std::cout << vm::to_display_string(h.result);
            }
            if (h.decoded.op == mx_op::PRINTLN)
            {
                std::cout << '\n';
            }
            if (h.decoded.op == mx_op::HALT)
            {
                halted = true;
                h = {};
                ooo.rob_head = (ooo.rob_head + 1) % vm::jit::k_rob_size;
                --ooo.rob_count;
                fetch_hold = false;
                break;
            }
            if (h.mispredict)
            {
                fetch_pc = h.correct_pc;
                f.pc = fetch_pc;
                // squash: free younger
                while (ooo.rob_count > 0)
                {
                    auto &e = ooo.rob[ooo.rob_head];
                    if (e.has_dest)
                    {
                        ooo.free_phys(e.dest_phys);
                    }
                    e = {};
                    ooo.rob_head = (ooo.rob_head + 1) % vm::jit::k_rob_size;
                    --ooo.rob_count;
                }
                ooo.rob_tail = ooo.rob_head;
                for (auto &r : ooo.rs)
                {
                    r = {};
                }
                // restore RAT from arch
                ooo.rat = ooo.arch_rat;
                break;
            }
            h = {};
            ooo.rob_head = (ooo.rob_head + 1) % vm::jit::k_rob_size;
            --ooo.rob_count;
            f.pc = fetch_pc; // keep
        }

        // Execute one ready RS entry (oldest in program order)
        int best = -1;
        uint32_t best_age = UINT32_MAX;
        for (size_t i = 0; i < vm::jit::k_rs_size; ++i)
        {
            auto &r = ooo.rs[i];
            if (!r.valid || !r.src1_ready || !r.src2_ready)
            {
                continue;
            }
            // Serialize (CALL/RET/HALT/PRINT): issue only at ROB head.
            // Mem ops: may_alias_mem skips false deps (known different slots).
            if (r.op.serialize && r.rob_id != ooo.rob_head)
            {
                continue;
            }
            if (!mem_dep_clear(r.rob_id, r.op))
            {
                continue; // true mem dep; false deps (different slots) pass
            }
            // age = distance from rob_head
            uint32_t age = 0;
            uint32_t idx = ooo.rob_head;
            bool found = false;
            for (; age < ooo.rob_count; ++age)
            {
                if (idx == r.rob_id)
                {
                    found = true;
                    break;
                }
                idx = (idx + 1) % vm::jit::k_rob_size;
            }
            if (!found)
            {
                continue;
            }
            if (age < best_age)
            {
                best_age = age;
                best = static_cast<int>(i);
            }
        }
        if (best >= 0)
        {
            progressed = true;
            auto &r = ooo.rs[static_cast<size_t>(best)];
            // rob_id stores the ROB index at rename time
            vm::jit::rob_entry *slot = nullptr;
            for (size_t i = 0; i < vm::jit::k_rob_size; ++i)
            {
                if (ooo.rob[i].valid && !ooo.rob[i].complete &&
                    static_cast<uint32_t>(i) == r.rob_id)
                {
                    slot = &ooo.rob[i];
                    break;
                }
            }
            if (slot != nullptr)
            {
                const jit_uop &u = r.op;
                vm::value result = r.src1;
                bool mispredict = false;
                uint32_t correct = u.pc + 1;
                switch (u.op)
                {
                case mx_op::ADD:
                case mx_op::SUB:
                case mx_op::MUL:
                case mx_op::DIV:
                case mx_op::MOD:
                    result = bin_arith(u.op, r.src1, r.src2);
                    break;
                case mx_op::EQ:
                case mx_op::NE:
                case mx_op::LT:
                case mx_op::GT:
                case mx_op::LE:
                case mx_op::GE:
                    result = bin_cmp(u.op, r.src1, r.src2);
                    break;
                case mx_op::NEG:
                    result = is_int(r.src1) ? vm::value{-as_int(r.src1)}
                                            : vm::value{-as_num(r.src1)};
                    break;
                case mx_op::NOT:
                    result = vm::value{!truthy(r.src1)};
                    break;
                case mx_op::MOV:
                case mx_op::LDC:
                case mx_op::LI:
                    result = r.src1;
                    break;
                case mx_op::LD_SLOT:
                {
                    if (u.imm_u < f.slots.size())
                    {
                        result = f.slots[u.imm_u];
                    }
                    // Forward from older completed stores to same slot (STB).
                    {
                        uint32_t idx = ooo.rob_head;
                        for (uint32_t n = 0; n < ooo.rob_count; ++n)
                        {
                            if (idx == r.rob_id)
                            {
                                break;
                            }
                            const auto &older = ooo.rob[idx];
                            if (older.valid && older.complete &&
                                older.decoded.is_store &&
                                older.decoded.mem_slot == u.imm_u)
                            {
                                result = older.result;
                            }
                            idx = (idx + 1) % vm::jit::k_rob_size;
                        }
                    }
                    break;
                }
                case mx_op::ST_SLOT:
                    result = r.src1;
                    break;
                case mx_op::CALL:
                case mx_op::RET:
                case mx_op::HALT:
                    // Handled after mark-complete via side path below
                    result = r.src1;
                    break;
                case mx_op::PRINT:
                case mx_op::PRINTLN:
                    result = r.src1;
                    break;
                case mx_op::ARGV_LEN:
                    result = vm::value{static_cast<int64_t>(argv.size())};
                    break;
                case mx_op::ARGV_GET:
                {
                    const int64_t idx = as_int(r.src1);
                    result = (idx >= 0 && static_cast<size_t>(idx) < argv.size())
                                 ? vm::value{argv[static_cast<size_t>(idx)]}
                                 : vm::value{};
                    break;
                }
                case mx_op::JMP:
                    correct = static_cast<uint32_t>(static_cast<int32_t>(u.pc) + u.imm);
                    break;
                case mx_op::BR_TRUE:
                case mx_op::BR_FALSE:
                {
                    const bool cond = truthy(r.src1);
                    const bool taken = (u.op == mx_op::BR_TRUE) ? cond : !cond;
                    const uint32_t target = static_cast<uint32_t>(
                        static_cast<int32_t>(u.pc) + u.imm);
                    const uint32_t fall = u.pc + 1;
                    auto &pred = vm::jit::branch_predictor::instance();
                    const bool predicted = pred.predict(u.pc, target, fall);
                    pred.train(u.pc, taken, target);
                    correct = taken ? target : fall;
                    if (predicted != taken)
                    {
                        mispredict = true;
                    }
                    break;
                }
                case mx_op::HINT:
                    next_branch_hint = static_cast<int8_t>(u.imm != 0);
                    break;
                default:
                    break;
                }
                if (slot->has_dest)
                {
                    ooo.phys_rf[slot->dest_phys] = result;
                    ooo.phys_ready[slot->dest_phys] = true;
                    // wakeup
                    for (auto &w : ooo.rs)
                    {
                        if (!w.valid)
                        {
                            continue;
                        }
                        if (!w.src1_ready && w.phys_src1 == slot->dest_phys)
                        {
                            w.src1 = result;
                            w.src1_ready = true;
                        }
                        if (!w.src2_ready && w.phys_src2 == slot->dest_phys)
                        {
                            w.src2 = result;
                            w.src2_ready = true;
                        }
                    }
                }
                slot->result = result;
                slot->complete = true;
                slot->mispredict = mispredict;
                slot->correct_pc = correct;

                // ROB-head CALL/RET/HALT: apply control at complete (serialize).
                // Copy fields first — clearing RS would invalidate `u`/`r` refs.
                if ((u.op == mx_op::CALL || u.op == mx_op::RET ||
                     u.op == mx_op::HALT) &&
                    r.rob_id == ooo.rob_head)
                {
                    const mx_op ctl = u.op;
                    const uint32_t ctl_pc = u.pc;
                    const uint32_t ctl_rd = u.arch_rd;
                    const vm::value ctl_src1 = r.src1;

                    sync_arch_from_phys();
                    squash_younger_than(r.rob_id);
                    // Commit this control op immediately
                    auto &h = ooo.rob[ooo.rob_head];
                    h = {};
                    ooo.rob_head = (ooo.rob_head + 1) % vm::jit::k_rob_size;
                    --ooo.rob_count;
                    for (auto &rr : ooo.rs)
                    {
                        rr = {};
                    }

                    fetch_hold = false;
                    if (ctl == mx_op::HALT)
                    {
                        halted = true;
                        break;
                    }
                    if (ctl == mx_op::CALL)
                    {
                        const uint32_t argc = dec_imm9(mod->code[ctl_pc]);
                        const std::string name = vm::to_display_string(ctl_src1);
                        const auto it = fn_entry.find(name);
                        if (it == fn_entry.end())
                        {
                            f.regs[ctl_rd] = vm::value{};
                            fetch_pc = ctl_pc + 1;
                            f.pc = fetch_pc;
                            reset_rename_from_frame();
                        }
                        else
                        {
                            std::array<vm::value, 8> args{};
                            for (uint32_t ai = 0; ai < argc && ai < 8; ++ai)
                            {
                                args[ai] = f.regs[1 + ai];
                            }
                            call_ret_rd_.push_back(ctl_rd);
                            push_frame(it->second, fn_slots[name], ctl_pc + 1);
                            mx_frame &nf = frame();
                            for (uint32_t ai = 0; ai < argc && ai < 8; ++ai)
                            {
                                nf.regs[1 + ai] = args[ai];
                                if (ai < nf.slots.size())
                                {
                                    nf.slots[ai] = args[ai];
                                }
                            }
                            fetch_pc = nf.pc;
                            reset_rename_from_frame();
                        }
                    }
                    else if (ctl == mx_op::RET)
                    {
                        const vm::value retv = f.regs[0];
                        const uint32_t rpc = f.return_pc;
                        call_stack.pop_back();
                        if (call_stack.empty())
                        {
                            halted = true;
                            break;
                        }
                        uint32_t dest_rd = 0;
                        if (!call_ret_rd_.empty())
                        {
                            dest_rd = call_ret_rd_.back();
                            call_ret_rd_.pop_back();
                        }
                        frame().regs[dest_rd] = retv;
                        frame().pc = rpc;
                        fetch_pc = rpc;
                        reset_rename_from_frame();
                    }
                    continue;
                }
            }
            r = {};
        }

        // Stall rename while a control serialize op is in flight (precise CALL/RET).
        bool control_inflight = false;
        {
            uint32_t idx = ooo.rob_head;
            for (uint32_t n = 0; n < ooo.rob_count; ++n)
            {
                const auto &e = ooo.rob[idx];
                if (e.valid && !e.complete &&
                    (e.decoded.op == mx_op::CALL || e.decoded.op == mx_op::RET ||
                     e.decoded.op == mx_op::HALT))
                {
                    control_inflight = true;
                    break;
                }
                idx = (idx + 1) % vm::jit::k_rob_size;
            }
        }

        // Rename / dispatch one insn into ROB+RS
        if (!fetch_hold && !control_inflight && !ooo.rob_full() &&
            fetch_pc < mod->code.size())
        {
            {
                // find free RS
                int rs_i = -1;
                for (size_t i = 0; i < vm::jit::k_rs_size; ++i)
                {
                    if (!ooo.rs[i].valid)
                    {
                        rs_i = static_cast<int>(i);
                        break;
                    }
                }
                if (rs_i >= 0)
                {
                    const uint32_t w = mod->code[fetch_pc];
                    jit_uop u = decode_uop(w, fetch_pc);
                    const bool needs_dest =
                        u.has_rd && u.op != mx_op::ST_SLOT &&
                        u.op != mx_op::PRINT && u.op != mx_op::PRINTLN;
                    // Stall rename if we need a phys reg and the free list is empty
                    // (do not spin with a fake "progressed" — that livelocks OoO).
                    if (needs_dest && ooo.free_list.empty())
                    {
                        goto issue_done;
                    }

                    progressed = true;
                    const uint32_t rob_i = ooo.rob_tail;
                    auto &re = ooo.rob[rob_i];
                    re = {};
                    re.valid = true;
                    re.pc = fetch_pc;
                    re.op = u.op;
                    re.decoded = u;
                    re.has_dest = needs_dest;

                    uint32_t p1 = ooo.rat[u.arch_rs1];
                    uint32_t p2 = ooo.rat[u.arch_rs2];
                    if (u.op == mx_op::BR_TRUE || u.op == mx_op::BR_FALSE)
                    {
                        p1 = ooo.rat[u.arch_rd];
                    }

                    uint32_t pdest = 0;
                    uint32_t oldp = 0;
                    if (re.has_dest)
                    {
                        auto alloc = ooo.alloc_phys();
                        if (!alloc)
                        {
                            re = {};
                            goto issue_done;
                        }
                        pdest = *alloc;
                        oldp = ooo.rat[u.arch_rd];
                        ooo.rat[u.arch_rd] = pdest;
                        re.arch_rd = u.arch_rd;
                        re.dest_phys = pdest;
                        re.old_phys = oldp;
                    }

                    auto &rs = ooo.rs[static_cast<size_t>(rs_i)];
                    rs = {};
                    rs.valid = true;
                    rs.rob_id = rob_i;
                    rs.op = u;
                    rs.phys_src1 = p1;
                    rs.phys_src2 = p2;
                    rs.phys_dest = pdest;
                    rs.src1_ready = ooo.phys_ready[p1];
                    rs.src2_ready = ooo.phys_ready[p2];
                    if (rs.src1_ready)
                    {
                        rs.src1 = ooo.phys_rf[p1];
                    }
                    if (rs.src2_ready)
                    {
                        rs.src2 = ooo.phys_rf[p2];
                    }
                    // immediates as src1 for LDC/LI
                    if (u.op == mx_op::LDC)
                    {
                        rs.src1 = pool_value(u.imm_u);
                        rs.src1_ready = true;
                        rs.src2_ready = true;
                    }
                    if (u.op == mx_op::LI)
                    {
                        rs.src1 = vm::value{static_cast<int64_t>(u.imm)};
                        rs.src1_ready = true;
                        rs.src2_ready = true;
                    }
                    if (u.op == mx_op::ARGV_LEN || u.op == mx_op::PRINTLN ||
                        u.op == mx_op::HINT || u.op == mx_op::JMP)
                    {
                        rs.src1_ready = true;
                        rs.src2_ready = true;
                    }
                    if (u.op == mx_op::LD_SLOT)
                    {
                        rs.src1_ready = true;
                        rs.src2_ready = true;
                    }
                    if (u.op == mx_op::CALL)
                    {
                        rs.src2_ready = true;
                    }
                    if (u.op == mx_op::RET || u.op == mx_op::HALT)
                    {
                        rs.src1_ready = true;
                        rs.src2_ready = true;
                    }
                    // RET reads architectural R0 at execute via sync

                    ooo.rob_tail = (ooo.rob_tail + 1) % vm::jit::k_rob_size;
                    ++ooo.rob_count;

                    // speculative fetch — park on CALL/RET/HALT (do not re-fetch)
                    if (u.op == mx_op::HALT || u.op == mx_op::RET ||
                        u.op == mx_op::CALL)
                    {
                        fetch_hold = true;
                    }
                    else if (u.op == mx_op::JMP)
                    {
                        fetch_pc = static_cast<uint32_t>(static_cast<int32_t>(u.pc) + u.imm);
                    }
                    else if (u.op == mx_op::BR_TRUE || u.op == mx_op::BR_FALSE)
                    {
                        const uint32_t target = static_cast<uint32_t>(
                            static_cast<int32_t>(u.pc) + u.imm);
                        const bool predicted =
                            vm::jit::branch_predictor::instance().predict(
                                u.pc, target, u.pc + 1);
                        fetch_pc = predicted ? target : u.pc + 1;
                    }
                    else
                    {
                        fetch_pc = u.pc + 1;
                    }
                }
            }
        }
    issue_done:
        (void)0;

        // Idle escape: if nothing in flight and can't fetch, halt
        if (ooo.rob_count == 0 && fetch_pc >= mod->code.size())
        {
            halted = true;
        }

        if (progressed)
        {
            idle_cycles = 0;
        }
        else
        {
            ++idle_cycles;
        }
        // Deadlock escape: drain to scalar one insn (progress guarantee).
        if (idle_cycles > 256 && !halted && !call_stack.empty())
        {
            idle_cycles = 0;
            sync_arch_from_phys();
            for (uint32_t n = 0; n < ooo.rob_count; ++n)
            {
                ooo.rob[(ooo.rob_head + n) % vm::jit::k_rob_size] = {};
            }
            ooo.rob_count = 0;
            ooo.rob_tail = ooo.rob_head;
            for (auto &rr : ooo.rs)
            {
                rr = {};
            }
            frame().pc = fetch_pc;
            if (!step_scalar())
            {
                break;
            }
            fetch_pc = call_stack.empty() ? 0 : frame().pc;
            reset_rename_from_frame();
            fetch_hold = false;
        }
    }

    // Write back arch regs
    if (!call_stack.empty())
    {
        for (uint32_t i = 0; i < k_arch_regs; ++i)
        {
            frame().regs[i] = ooo.phys_rf[ooo.arch_rat[i]];
        }
    }
}

} // namespace munx::isa
