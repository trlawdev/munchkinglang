#pragma once

#include "bytecode_optimizer.hpp"
#include "branch_predictor.hpp"
#include "bytecode_utils.hpp"
#include "execution_profile.hpp"
#include <cstring>
#include <vector>

namespace munx::vm::jit
{

/// Statically seed loop/back-edge bias by scanning conditional jumps.
inline void seed_predictor_from_bytecode(std::span<const std::byte> source,
                                         std::string_view strings,
                                         branch_predictor &predictor)
{
    bytecode_cursor cursor{source, strings};
    while (!cursor.at_end())
    {
        const size_t branch_pc = cursor.pc;
        const auto opcode = static_cast<Opcode>(cursor.read_u8());
        if (opcode == Opcode::JMP_IF_FALSE || opcode == Opcode::JMP_IF_TRUE)
        {
            const uint32_t target = cursor.read_scalar<uint32_t>();
            const size_t jump_pc = target;
            if (jump_pc < branch_pc)
            {
                for (int index = 0; index < 4; ++index)
                {
                    predictor.train(branch_pc, true, jump_pc);
                }
            }
        }
        else
        {
            cursor.pc = branch_pc;
            (void)cursor.skip_instruction();
        }
    }
}

/// Profile-guided bytecode specialization: remove strongly biased branches.
inline std::vector<std::byte>
apply_profile_optimization(std::span<const std::byte> source, std::string_view strings,
                           const execution_profile &profile)
{
    if (!pgo_enabled() || profile.branches.empty())
    {
        return std::vector<std::byte>(source.begin(), source.end());
    }

    std::vector<std::byte> out;
    out.reserve(source.size());

    bytecode_cursor cursor{source, strings};
    while (!cursor.at_end())
    {
        const size_t insn_pc = cursor.pc;
        const auto opcode = static_cast<Opcode>(cursor.read_u8());

        if (opcode == Opcode::JMP_IF_FALSE || opcode == Opcode::JMP_IF_TRUE)
        {
            const uint32_t target = cursor.read_scalar<uint32_t>();
            const auto found = profile.branches.find(insn_pc);
            if (found != profile.branches.end())
            {
                const branch_profile &site = found->second;
                if (site.strongly_not_taken())
                {
                    // Condition is consumed; fall through without a branch.
                    bytecode_cursor::append_u8(out, static_cast<uint8_t>(Opcode::POP));
                    continue;
                }
                if (site.strongly_taken())
                {
                    bytecode_cursor::append_u8(out, static_cast<uint8_t>(Opcode::POP));
                    bytecode_cursor::append_u8(out, static_cast<uint8_t>(Opcode::JMP));
                    bytecode_cursor::append_scalar(out, target);
                    continue;
                }
            }

            cursor.pc = insn_pc;
            (void)cursor.skip_instruction();
            const size_t end = cursor.pc;
            out.insert(out.end(), source.begin() + static_cast<std::ptrdiff_t>(insn_pc),
                       source.begin() + static_cast<std::ptrdiff_t>(end));
            continue;
        }

        cursor.pc = insn_pc;
        (void)cursor.skip_instruction();
        const size_t end = cursor.pc;
        out.insert(out.end(), source.begin() + static_cast<std::ptrdiff_t>(insn_pc),
                   source.begin() + static_cast<std::ptrdiff_t>(end));
    }

    return out;
}

/// Full optimize pipeline: profile specialization on source, then constant fold/peephole.
inline std::vector<std::byte>
optimize_bytecode_with_profile(std::span<const std::byte> source,
                               std::string_view strings,
                               const execution_profile *profile)
{
    std::span<const std::byte> input = source;
    std::vector<std::byte> specialized;
    if (profile != nullptr && pgo_enabled())
    {
        specialized = apply_profile_optimization(source, strings, *profile);
        input = specialized;
    }
    return optimize_bytecode(input, strings);
}

} // namespace munx::vm::jit
