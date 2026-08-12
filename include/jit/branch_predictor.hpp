#pragma once

#include "execution_profile.hpp"
#include <cstddef>
#include <cstdint>

namespace munx::vm::jit
{

/// Hybrid global-history perceptron + bimodal counter + dedicated loop predictor.
///
/// Used by threaded JIT and interpreter `JMP_IF_*` sites. Back-edges consult a
/// separate 256-entry table keyed by `(branch_pc >> 4)`.
class branch_predictor
{
public:
    static constexpr size_t history_bits = 12;
    static constexpr size_t table_size = 4096;
    static constexpr size_t back_edge_table_size = 256;
    static constexpr int confidence_threshold = 8;

    static branch_predictor &instance()
    {
        static thread_local branch_predictor predictor;
        return predictor;
    }

    /// Predict whether the jump target is taken at @p branch_pc.
    [[nodiscard]] bool predict(size_t branch_pc, size_t jump_pc,
                               size_t fallthrough_pc) const
    {
        (void)fallthrough_pc;
        const bool back_edge = jump_pc < branch_pc;
        if (back_edge)
        {
            const uint8_t loop_counter = back_edge_table_[back_edge_index(branch_pc)];
            if (loop_counter >= 2)
            {
                return true;
            }
            if (loop_counter <= 1 && loop_counter != 0)
            {
                return false;
            }
        }

        const size_t index = index_for(branch_pc);
        const entry &slot = table_[index];

        int perceptron_sum = slot.bias;
        uint32_t history = history_;
        for (size_t bit = 0; bit < history_bits; ++bit)
        {
            if ((history & 1U) != 0)
            {
                perceptron_sum += slot.weights[bit];
            }
            history >>= 1U;
        }

        const bool counter_taken = slot.counter >= 2;
        if (perceptron_sum >= confidence_threshold)
        {
            return true;
        }
        if (perceptron_sum <= -confidence_threshold)
        {
            return false;
        }

        if (slot.warmup < 4 && back_edge)
        {
            return true;
        }
        if (slot.warmup < 4 && jump_pc > branch_pc)
        {
            return false;
        }

        return counter_taken;
    }

    /// Update tables after the actual outcome is known.
    void train(size_t branch_pc, bool taken, size_t jump_pc = 0)
    {
        if (jump_pc < branch_pc)
        {
            uint8_t &loop_counter = back_edge_table_[back_edge_index(branch_pc)];
            if (taken)
            {
                if (loop_counter < 3)
                {
                    ++loop_counter;
                }
            }
            else if (loop_counter > 0)
            {
                --loop_counter;
            }
        }

        entry &slot = table_[index_for(branch_pc)];
        const int direction = taken ? 1 : -1;

        slot.bias += direction;
        if (slot.bias > 127)
        {
            slot.bias = 127;
        }
        else if (slot.bias < -128)
        {
            slot.bias = -128;
        }

        if (taken)
        {
            if (slot.counter < 3)
            {
                ++slot.counter;
            }
        }
        else if (slot.counter > 0)
        {
            --slot.counter;
        }

        uint32_t history = history_;
        for (size_t bit = 0; bit < history_bits; ++bit)
        {
            if ((history & 1U) != 0)
            {
                int8_t weight = static_cast<int8_t>(slot.weights[bit] + direction);
                if (weight > 127)
                {
                    weight = 127;
                }
                else if (weight < -128)
                {
                    weight = -128;
                }
                slot.weights[bit] = weight;
            }
            history >>= 1U;
        }

        if (slot.warmup < 255)
        {
            ++slot.warmup;
        }

        local_history_[branch_pc & (local_history_size - 1)] =
            static_cast<uint8_t>(
                ((local_history_[branch_pc & (local_history_size - 1)] << 1U) |
                 (taken ? 1U : 0U)) &
                0xFFU);

        history_ = ((history_ << 1U) | (taken ? 1U : 0U)) &
                   ((1U << history_bits) - 1U);
    }

    /// Apply a static `likely` / `unlikely` compiler hint at @p branch_pc.
    /// @param expected_taken True when the jump arm is the expected outcome.
    void seed_static_hint(size_t branch_pc, bool expected_taken) noexcept
    {
        entry &slot = table_[index_for(branch_pc)];
        if (expected_taken)
        {
            slot.counter = 3;
            slot.bias = 32;
        }
        else
        {
            slot.counter = 0;
            slot.bias = -32;
        }
        slot.warmup = 255;
    }

    /// Warm-start tables from collected runtime profile data.
    void seed_from_profile(const execution_profile &profile)
    {
        for (const auto &[branch_pc, site] : profile.branches)
        {
            const uint32_t total = site.total();
            if (total == 0)
            {
                continue;
            }

            const bool taken_bias = site.taken >= site.not_taken;
            const int rounds =
                static_cast<int>(total >= 64 ? 6 : total >= 16 ? 4 : 2);
            for (int round = 0; round < rounds; ++round)
            {
                train(branch_pc, taken_bias, branch_pc);
            }

            entry &slot = table_[index_for(branch_pc)];
            if (site.strongly_taken())
            {
                slot.counter = 3;
                slot.bias = 32;
            }
            else if (site.strongly_not_taken())
            {
                slot.counter = 0;
                slot.bias = -32;
            }
            slot.warmup = 255;
        }
    }

private:
    static constexpr size_t local_history_size = 256;

    struct entry
    {
        int8_t bias{0};
        int8_t weights[history_bits]{};
        uint8_t counter{1};
        uint8_t warmup{0};
    };

    [[nodiscard]] static size_t back_edge_index(size_t branch_pc) noexcept
    {
        return (branch_pc >> 4) & (back_edge_table_size - 1);
    }

    [[nodiscard]] size_t index_for(size_t branch_pc) const
    {
        const uint32_t local =
            local_history_[branch_pc & (local_history_size - 1)];
        return ((branch_pc >> 2) ^ history_ ^ (static_cast<uint32_t>(local) << 4)) &
               (table_size - 1);
    }

    entry table_[table_size]{};
    uint8_t back_edge_table_[back_edge_table_size]{};
    uint8_t local_history_[local_history_size]{};
    uint32_t history_{0};
};

} // namespace munx::vm::jit
