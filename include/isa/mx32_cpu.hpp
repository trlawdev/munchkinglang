#pragma once

#include "mx32.hpp"
#include "../jit/branch_predictor.hpp"
#include "../vm_value.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace munx::isa
{

enum class pipeline_mode
{
    scalar,  ///< one insn at a time (debugger / --interp style)
    inorder, ///< 5-stage in-order
    ooo,     ///< rename / issue / ROB
};

inline pipeline_mode pipeline_mode_from_env()
{
    const char *env = std::getenv("MUNX_VM_PIPELINE");
    if (env == nullptr || env[0] == '\0')
    {
        return pipeline_mode::inorder;
    }
    const std::string s{env};
    if (s == "scalar" || s == "interp")
    {
        return pipeline_mode::scalar;
    }
    if (s == "inorder" || s == "in-order")
    {
        return pipeline_mode::inorder;
    }
    return pipeline_mode::ooo;
}

struct mx_frame
{
    uint32_t pc{0};
    uint32_t return_pc{0};
    std::array<vm::value, k_arch_regs> regs{};
    std::vector<vm::value> slots;
    uint32_t local_slots{0};
};

struct mx_cpu
{
    const mx_module *mod{nullptr};
    std::vector<std::string> argv;
    std::unordered_map<std::string, uint32_t> fn_entry;
    std::unordered_map<std::string, uint32_t> fn_params;
    std::unordered_map<std::string, uint32_t> fn_slots;
    std::vector<mx_frame> call_stack;
    bool halted{false};
    int8_t next_branch_hint{-1}; // -1 none, 0/1 for HINT
    pipeline_mode mode{pipeline_mode::scalar};

    void bind(const mx_module &m)
    {
        mod = &m;
        fn_entry.clear();
        fn_params.clear();
        fn_slots.clear();
        for (const auto &f : m.functions)
        {
            fn_entry[f.name] = f.entry;
            fn_params[f.name] = f.param_count;
            fn_slots[f.name] = f.local_slots;
        }
    }

    mx_frame &frame() { return call_stack.back(); }

    static bool truthy(const vm::value &v) { return vm::is_truthy(v); }

    vm::value pool_value(uint32_t idx) const
    {
        if (idx >= mod->pool.size())
        {
            return vm::value{};
        }
        const pool_entry &e = mod->pool[idx];
        switch (e.tag)
        {
        case pool_tag::i64:
            return vm::value{e.i64};
        case pool_tag::f64:
            return vm::value{e.f64};
        case pool_tag::boolean:
            return vm::value{e.b};
        case pool_tag::string:
            return vm::value{e.str};
        case pool_tag::null:
            return vm::value{};
        }
        return vm::value{};
    }

    static bool is_int(const vm::value &v)
    {
        return std::holds_alternative<int64_t>(v.data);
    }
    static bool is_num(const vm::value &v)
    {
        return std::holds_alternative<int64_t>(v.data) ||
               std::holds_alternative<double>(v.data) ||
               std::holds_alternative<bool>(v.data);
    }
    static double as_num(const vm::value &v)
    {
        if (const auto *i = std::get_if<int64_t>(&v.data))
        {
            return static_cast<double>(*i);
        }
        if (const auto *d = std::get_if<double>(&v.data))
        {
            return *d;
        }
        if (const auto *b = std::get_if<bool>(&v.data))
        {
            return *b ? 1.0 : 0.0;
        }
        return 0.0;
    }
    static int64_t as_int(const vm::value &v)
    {
        if (const auto *i = std::get_if<int64_t>(&v.data))
        {
            return *i;
        }
        return static_cast<int64_t>(as_num(v));
    }

    vm::value bin_arith(mx_op op, const vm::value &a, const vm::value &b) const
    {
        if (op == mx_op::ADD)
        {
            if (vm::try_string(a) != nullptr || vm::try_string(b) != nullptr)
            {
                return vm::value{vm::to_display_string(a) + vm::to_display_string(b)};
            }
        }
        if (is_int(a) && is_int(b) && op != mx_op::DIV)
        {
            const int64_t x = as_int(a);
            const int64_t y = as_int(b);
            switch (op)
            {
            case mx_op::ADD:
                return vm::value{x + y};
            case mx_op::SUB:
                return vm::value{x - y};
            case mx_op::MUL:
                return vm::value{x * y};
            case mx_op::MOD:
                return y == 0 ? vm::value{int64_t{0}} : vm::value{x % y};
            default:
                break;
            }
        }
        const double x = as_num(a);
        const double y = as_num(b);
        switch (op)
        {
        case mx_op::ADD:
            return vm::value{x + y};
        case mx_op::SUB:
            return vm::value{x - y};
        case mx_op::MUL:
            return vm::value{x * y};
        case mx_op::DIV:
            return vm::value{y == 0.0 ? 0.0 : x / y};
        case mx_op::MOD:
            return vm::value{y == 0.0 ? 0.0 : std::fmod(x, y)};
        default:
            break;
        }
        return vm::value{};
    }

    vm::value bin_cmp(mx_op op, const vm::value &a, const vm::value &b) const
    {
        int ord = 0;
        if (is_num(a) && is_num(b))
        {
            const double x = as_num(a);
            const double y = as_num(b);
            ord = x < y ? -1 : (x > y ? 1 : 0);
        }
        else
        {
            const std::string x = vm::to_display_string(a);
            const std::string y = vm::to_display_string(b);
            ord = x < y ? -1 : (x > y ? 1 : 0);
        }
        switch (op)
        {
        case mx_op::EQ:
            return vm::value{ord == 0};
        case mx_op::NE:
            return vm::value{ord != 0};
        case mx_op::LT:
            return vm::value{ord < 0};
        case mx_op::GT:
            return vm::value{ord > 0};
        case mx_op::LE:
            return vm::value{ord <= 0};
        case mx_op::GE:
            return vm::value{ord >= 0};
        default:
            return vm::value{false};
        }
    }

    void push_frame(uint32_t entry, uint32_t slots, uint32_t ret_pc)
    {
        mx_frame f;
        f.pc = entry;
        f.return_pc = ret_pc;
        f.local_slots = slots;
        f.slots.assign(slots + 8, vm::value{});
        // copy arg regs from caller if any
        if (!call_stack.empty())
        {
            for (uint32_t i = 1; i < k_arch_regs; ++i)
            {
                f.regs[i] = call_stack.back().regs[i];
            }
            // params into slots 0..
            // (caller placed args in R1..)
        }
        call_stack.push_back(std::move(f));
    }

    /// Execute a single instruction from the current frame. Returns false if halted/returned from entry.
    bool step_scalar()
    {
        if (halted || call_stack.empty())
        {
            return false;
        }
        mx_frame &f = frame();
        if (f.pc >= mod->code.size())
        {
            halted = true;
            return false;
        }
        const uint32_t w = mod->code[f.pc];
        const uint32_t pc0 = f.pc;
        const mx_op op = dec_op(w);
        const uint32_t rd = dec_rd(w);
        const uint32_t rs1 = dec_rs1(w);
        const uint32_t rs2 = dec_rs2(w);
        bool branched = false;

        auto do_branch = [&](bool taken, int32_t imm) {
            const uint32_t fall = pc0 + 1;
            const uint32_t target =
                static_cast<uint32_t>(static_cast<int32_t>(pc0) + imm);
            bool predicted = taken;
            if (op == mx_op::BR_TRUE || op == mx_op::BR_FALSE || op == mx_op::JMP)
            {
                auto &pred = vm::jit::branch_predictor::instance();
                if (op != mx_op::JMP)
                {
                    if (next_branch_hint >= 0)
                    {
                        pred.seed_static_hint(pc0, next_branch_hint != 0);
                        next_branch_hint = -1;
                    }
                    predicted = pred.predict(pc0, target, fall);
                    pred.train(pc0, taken, target);
                }
                (void)predicted; // scalar always takes correct path
            }
            if (taken)
            {
                f.pc = target;
                branched = true;
            }
        };

        switch (op)
        {
        case mx_op::NOP:
            break;
        case mx_op::HALT:
            halted = true;
            return false;
        case mx_op::HINT:
            next_branch_hint = static_cast<int8_t>(dec_imm19_signed(w) != 0);
            break;
        case mx_op::MOV:
            f.regs[rd] = f.regs[rs1];
            break;
        case mx_op::LDC:
            f.regs[rd] = pool_value(dec_imm14(w));
            break;
        case mx_op::LI:
            f.regs[rd] = vm::value{static_cast<int64_t>(dec_imm19_signed(w))};
            break;
        case mx_op::ADD:
        case mx_op::SUB:
        case mx_op::MUL:
        case mx_op::DIV:
        case mx_op::MOD:
            f.regs[rd] = bin_arith(op, f.regs[rs1], f.regs[rs2]);
            break;
        case mx_op::NEG:
        {
            if (is_int(f.regs[rs1]))
            {
                f.regs[rd] = vm::value{-as_int(f.regs[rs1])};
            }
            else
            {
                f.regs[rd] = vm::value{-as_num(f.regs[rs1])};
            }
            break;
        }
        case mx_op::NOT:
            f.regs[rd] = vm::value{!truthy(f.regs[rs1])};
            break;
        case mx_op::EQ:
        case mx_op::NE:
        case mx_op::LT:
        case mx_op::GT:
        case mx_op::LE:
        case mx_op::GE:
            f.regs[rd] = bin_cmp(op, f.regs[rs1], f.regs[rs2]);
            break;
        case mx_op::LD_SLOT:
        {
            const uint32_t slot = dec_imm14(w);
            if (slot < f.slots.size())
            {
                f.regs[rd] = f.slots[slot];
            }
            break;
        }
        case mx_op::ST_SLOT:
        {
            const uint32_t slot = dec_imm14(w);
            if (slot >= f.slots.size())
            {
                f.slots.resize(slot + 1);
            }
            f.slots[slot] = f.regs[rs1];
            break;
        }
        case mx_op::PRINT:
            std::cout << vm::to_display_string(f.regs[rs1]);
            break;
        case mx_op::PRINTLN:
            std::cout << '\n';
            break;
        case mx_op::JMP:
            do_branch(true, dec_imm19_signed(w));
            break;
        case mx_op::BR_TRUE:
            do_branch(truthy(f.regs[rd]), dec_imm19_signed(w));
            break;
        case mx_op::BR_FALSE:
            do_branch(!truthy(f.regs[rd]), dec_imm19_signed(w));
            break;
        case mx_op::ARGV_LEN:
            f.regs[rd] = vm::value{static_cast<int64_t>(argv.size())};
            break;
        case mx_op::ARGV_GET:
        {
            const int64_t idx = as_int(f.regs[rs1]);
            if (idx >= 0 && static_cast<size_t>(idx) < argv.size())
            {
                f.regs[rd] = vm::value{argv[static_cast<size_t>(idx)]};
            }
            else
            {
                f.regs[rd] = vm::value{};
            }
            break;
        }
        case mx_op::CALL:
        {
            const uint32_t argc = dec_imm9(w);
            std::string name = vm::to_display_string(f.regs[rs1]);
            // Strip if pool stored raw — names are strings
            const auto it = fn_entry.find(name);
            if (it == fn_entry.end())
            {
                // unknown: null result
                f.regs[rd] = vm::value{};
                break;
            }
            const uint32_t slots = fn_slots[name];
            const uint32_t ret = pc0 + 1;
            // Save args from R1.. before push
            std::array<vm::value, 8> args{};
            for (uint32_t i = 0; i < argc && i < 8; ++i)
            {
                args[i] = f.regs[1 + i];
            }
            push_frame(it->second, slots, ret);
            mx_frame &nf = frame();
            for (uint32_t i = 0; i < argc && i < 8; ++i)
            {
                nf.regs[1 + i] = args[i];
                if (i < nf.slots.size())
                {
                    nf.slots[i] = args[i];
                }
            }
            nf.regs[0] = vm::value{}; // return will fill
            // Stash destination arch reg in unused way: use slots.back marker
            // Store rd in return_pc high? Use a side stack.
            call_ret_rd_.push_back(rd);
            branched = true; // pc already set in new frame
            break;
        }
        case mx_op::RET:
        {
            const vm::value ret = f.regs[0];
            const uint32_t rpc = f.return_pc;
            call_stack.pop_back();
            if (call_stack.empty())
            {
                halted = true;
                return false;
            }
            uint32_t dest_rd = 0;
            if (!call_ret_rd_.empty())
            {
                dest_rd = call_ret_rd_.back();
                call_ret_rd_.pop_back();
            }
            frame().regs[dest_rd] = ret;
            frame().pc = rpc;
            branched = true;
            break;
        }
        default:
            break;
        }

        if (!branched && !halted && !call_stack.empty())
        {
            frame().pc = pc0 + 1;
        }
        return !halted;
    }

    std::vector<uint32_t> call_ret_rd_;

    int run(pipeline_mode m)
    {
        mode = m;
        halted = false;
        call_stack.clear();
        call_ret_rd_.clear();
        push_frame(mod->entry_pc, 32, 0);
        switch (m)
        {
        case pipeline_mode::scalar:
            while (step_scalar())
            {
            }
            break;
        case pipeline_mode::inorder:
            run_inorder();
            break;
        case pipeline_mode::ooo:
            run_ooo();
            break;
        }
        return halted ? 0 : 0;
    }

    // Forward decls implemented below after pipeline types
    void run_inorder();
    void run_ooo();
};

} // namespace munx::isa
