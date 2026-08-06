#pragma once

#include "bytecode_utils.hpp"
#include <optional>
#include <unordered_map>

namespace munx::vm::jit
{

/// Aggressive bytecode optimizer: constant folding, peephole cleanup, jump threading.
inline std::vector<std::byte> optimize_bytecode(std::span<const std::byte> source,
                                                std::string_view strings)
{
    std::vector<std::byte> out;
    out.reserve(source.size());

    bytecode_cursor cursor{source, strings};
    std::unordered_map<size_t, size_t> pc_remap;
    std::optional<size_t> pending_pc;

    auto record_pc = [&](size_t source_pc) { pc_remap[source_pc] = out.size(); };

    auto remap_target = [&](uint32_t old_target) -> uint32_t {
        const auto exact = pc_remap.find(old_target);
        if (exact != pc_remap.end())
        {
            return static_cast<uint32_t>(exact->second);
        }
        size_t best_source = 0;
        size_t best_output = old_target;
        bool found = false;
        for (const auto &[source_pc, output_pc] : pc_remap)
        {
            if (source_pc <= old_target && (!found || source_pc > best_source))
            {
                best_source = source_pc;
                best_output = output_pc;
                found = true;
            }
        }
        return static_cast<uint32_t>(found ? best_output : old_target);
    };

    auto emit_opcode = [&](Opcode opcode) {
        bytecode_cursor::append_u8(out, static_cast<uint8_t>(opcode));
    };

    auto emit_scalar = [&](auto value) { bytecode_cursor::append_scalar(out, value); };

    struct pending_fold
    {
        enum class kind { None, Int, IntPair, Bool } tag{kind::None};
        int64_t i64{0};
        int64_t i64_left{0};
        bool flag{false};
    };

    pending_fold pending{};

    auto flush_pending = [&] {
        if (pending.tag == pending_fold::kind::None)
        {
            return;
        }
        if (pending_pc.has_value())
        {
            record_pc(*pending_pc);
            pending_pc.reset();
        }
        switch (pending.tag)
        {
        case pending_fold::kind::Int:
            emit_opcode(Opcode::PUSH_INT);
            emit_scalar(pending.i64);
            break;
        case pending_fold::kind::IntPair:
            emit_opcode(Opcode::PUSH_INT);
            emit_scalar(pending.i64_left);
            emit_opcode(Opcode::PUSH_INT);
            emit_scalar(pending.i64);
            break;
        case pending_fold::kind::Bool:
            emit_opcode(Opcode::PUSH_BOOL);
            bytecode_cursor::append_u8(out, pending.flag ? 1 : 0);
            break;
        default:
            break;
        }
        pending.tag = pending_fold::kind::None;
    };

    auto try_fold_binary_int = [&](Opcode opcode, int64_t left,
                                   int64_t right) -> std::optional<int64_t> {
        switch (opcode)
        {
        case Opcode::ADD:
            return left + right;
        case Opcode::SUB:
            return left - right;
        case Opcode::MUL:
            return left * right;
        case Opcode::DIV:
            return right == 0 ? std::nullopt : std::optional<int64_t>{left / right};
        case Opcode::MOD:
            return right == 0 ? std::nullopt : std::optional<int64_t>{left % right};
        case Opcode::BITWISE_AND:
            return left & right;
        case Opcode::BITWISE_OR:
            return left | right;
        case Opcode::BITWISE_XOR:
            return left ^ right;
        default:
            return std::nullopt;
        }
    };

    auto try_fold_compare_int = [&](Opcode opcode, int64_t left,
                                    int64_t right) -> std::optional<bool> {
        switch (opcode)
        {
        case Opcode::EQ:
            return left == right;
        case Opcode::NE:
            return left != right;
        case Opcode::LT:
            return left < right;
        case Opcode::GT:
            return left > right;
        case Opcode::LE:
            return left <= right;
        case Opcode::GE:
            return left >= right;
        default:
            return std::nullopt;
        }
    };

    while (!cursor.at_end())
    {
        const size_t insn_start = cursor.pc;
        const auto opcode = static_cast<Opcode>(cursor.read_u8());

        if (opcode == Opcode::PUSH_INT)
        {
            const int64_t value = cursor.read_scalar<int64_t>();
            if (pending.tag == pending_fold::kind::Int)
            {
                pending.i64_left = pending.i64;
                pending.i64 = value;
                pending.tag = pending_fold::kind::IntPair;
                continue;
            }
            if (pending.tag == pending_fold::kind::IntPair)
            {
                flush_pending();
            }
            pending.tag = pending_fold::kind::Int;
            pending.i64 = value;
            pending_pc = insn_start;
            continue;
        }

        if (opcode == Opcode::PUSH_BOOL)
        {
            const bool flag = cursor.read_u8() != 0;
            if (pending.tag == pending_fold::kind::None)
            {
                pending.tag = pending_fold::kind::Bool;
                pending.flag = flag;
                pending_pc = insn_start;
                continue;
            }
            flush_pending();
            record_pc(insn_start);
            emit_opcode(Opcode::PUSH_BOOL);
            bytecode_cursor::append_u8(out, flag ? 1 : 0);
            continue;
        }

        if (pending.tag == pending_fold::kind::IntPair &&
            (opcode == Opcode::ADD || opcode == Opcode::SUB || opcode == Opcode::MUL ||
             opcode == Opcode::DIV || opcode == Opcode::MOD || opcode == Opcode::BITWISE_AND ||
             opcode == Opcode::BITWISE_OR || opcode == Opcode::BITWISE_XOR))
        {
            const int64_t right = pending.i64;
            const int64_t left = pending.i64_left;
            pending.tag = pending_fold::kind::None;
            pending_pc.reset();
            record_pc(insn_start);
            if (const std::optional<int64_t> folded = try_fold_binary_int(opcode, left, right))
            {
                pending.tag = pending_fold::kind::Int;
                pending.i64 = *folded;
                pending_pc = insn_start;
                continue;
            }
            emit_opcode(Opcode::PUSH_INT);
            emit_scalar(left);
            emit_opcode(Opcode::PUSH_INT);
            emit_scalar(right);
            emit_opcode(opcode);
            continue;
        }

        if (pending.tag == pending_fold::kind::Int &&
            (opcode == Opcode::ADD || opcode == Opcode::SUB || opcode == Opcode::MUL ||
             opcode == Opcode::DIV || opcode == Opcode::MOD || opcode == Opcode::BITWISE_AND ||
             opcode == Opcode::BITWISE_OR || opcode == Opcode::BITWISE_XOR))
        {
            const int64_t right = pending.i64;
            pending.tag = pending_fold::kind::None;
            pending_pc.reset();
            record_pc(insn_start);
            emit_opcode(Opcode::PUSH_INT);
            emit_scalar(right);
            emit_opcode(opcode);
            continue;
        }

        if (pending.tag == pending_fold::kind::IntPair &&
            (opcode == Opcode::EQ || opcode == Opcode::NE || opcode == Opcode::LT ||
             opcode == Opcode::GT || opcode == Opcode::LE || opcode == Opcode::GE))
        {
            const int64_t right = pending.i64;
            const int64_t left = pending.i64_left;
            pending.tag = pending_fold::kind::None;
            pending_pc.reset();
            record_pc(insn_start);
            if (const std::optional<bool> folded = try_fold_compare_int(opcode, left, right))
            {
                pending.tag = pending_fold::kind::Bool;
                pending.flag = *folded;
                pending_pc = insn_start;
                continue;
            }
            emit_opcode(Opcode::PUSH_INT);
            emit_scalar(left);
            emit_opcode(Opcode::PUSH_INT);
            emit_scalar(right);
            emit_opcode(opcode);
            continue;
        }

        if (pending.tag == pending_fold::kind::Int &&
            (opcode == Opcode::EQ || opcode == Opcode::NE || opcode == Opcode::LT ||
             opcode == Opcode::GT || opcode == Opcode::LE || opcode == Opcode::GE))
        {
            const int64_t right = pending.i64;
            pending.tag = pending_fold::kind::None;
            pending_pc.reset();
            record_pc(insn_start);
            emit_opcode(Opcode::PUSH_INT);
            emit_scalar(right);
            emit_opcode(opcode);
            continue;
        }

        if (opcode == Opcode::NEG && pending.tag == pending_fold::kind::Int)
        {
            record_pc(insn_start);
            pending.i64 = -pending.i64;
            continue;
        }

        if (opcode == Opcode::NOT && pending.tag == pending_fold::kind::Bool)
        {
            record_pc(insn_start);
            pending.flag = !pending.flag;
            continue;
        }

        if (opcode == Opcode::POP && pending.tag != pending_fold::kind::None)
        {
            record_pc(insn_start);
            pending.tag = pending_fold::kind::None;
            pending_pc.reset();
            continue;
        }

        flush_pending();
        record_pc(insn_start);
        cursor.pc = insn_start;
        const size_t copy_start = cursor.pc;
        (void)cursor.skip_instruction();
        const size_t copy_end = cursor.pc;
        out.insert(out.end(), source.begin() + static_cast<std::ptrdiff_t>(copy_start),
                   source.begin() + static_cast<std::ptrdiff_t>(copy_end));
    }

    flush_pending();

    for (size_t index = 0; index < out.size();)
    {
        const auto opcode = static_cast<Opcode>(out[index]);
        if (opcode == Opcode::JMP || opcode == Opcode::JMP_IF_FALSE ||
            opcode == Opcode::JMP_IF_TRUE || opcode == Opcode::MONITOR_ENTER)
        {
            uint32_t target = 0;
            std::memcpy(&target, out.data() + index + 1, sizeof target);
            target = remap_target(target);
            for (int depth = 0; depth < 32; ++depth)
            {
                if (target >= out.size() ||
                    static_cast<Opcode>(out[target]) != Opcode::JMP)
                {
                    break;
                }
                uint32_t next = 0;
                std::memcpy(&next, out.data() + target + 1, sizeof next);
                target = next;
            }
            std::memcpy(out.data() + index + 1, &target, sizeof target);
            index += 1 + sizeof(uint32_t);
            continue;
        }

        bytecode_cursor step{out, strings, index};
        (void)step.skip_instruction();
        index = step.pc;
    }

    return out;
}

} // namespace munx::vm::jit
