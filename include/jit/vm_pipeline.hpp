#pragma once

#include "jit_compiler.hpp"
#include "ooo.hpp"
#include <cstddef>
#include <optional>

namespace munx::vm::jit
{

/// In-order 5-stage latch bundle (IF/ID/EX/MEM/WB).
struct stage_if_id
{
    bool valid{false};
    uint32_t pc{0};
    uint32_t insn{0};
};

struct stage_id_ex
{
    bool valid{false};
    uop op{};
    vm::value v_rs1{};
    vm::value v_rs2{};
};

struct stage_ex_mem
{
    bool valid{false};
    uop op{};
    vm::value result{};
    bool branch_taken{false};
    uint32_t branch_target{0};
    bool mispredict{false};
    bool is_store{false};
    uint32_t store_slot{0};
    vm::value store_val{};
};

struct stage_mem_wb
{
    bool valid{false};
    uop op{};
    vm::value result{};
    bool has_rd{false};
};

struct inorder_pipeline
{
    stage_if_id if_id{};
    stage_id_ex id_ex{};
    stage_ex_mem ex_mem{};
    stage_mem_wb mem_wb{};
    bool stall_fetch{false};
    bool squash{false};
    uint32_t squash_pc{0};

    void reset()
    {
        if_id = {};
        id_ex = {};
        ex_mem = {};
        mem_wb = {};
        stall_fetch = false;
        squash = false;
        squash_pc = 0;
    }
};

/// Legacy three-stage overlap for threaded JIT dispatch (v8 stack path).
struct pipeline_state
{
    size_t fetch_pc{0};
    std::optional<size_t> fetched_handler;
    std::optional<size_t> speculative_handler;
    bool speculative{false};

    void reset(size_t pc) noexcept
    {
        fetch_pc = pc;
        fetched_handler.reset();
        speculative_handler.reset();
        speculative = false;
    }
};

[[nodiscard]] inline size_t pipeline_fetch_handler(const compiled_unit &unit, size_t pc)
{
    if (pc < unit.handler_index.size())
    {
        return unit.handler_index[pc];
    }
    return compiled_unit::invalid_handler;
}

} // namespace munx::vm::jit
