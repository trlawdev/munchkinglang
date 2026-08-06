#pragma once

#include "bytecode_optimizer.hpp"
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace munx::vm
{
class virtual_machine;
struct frame;
} // namespace munx::vm

namespace munx::vm::jit
{

using jit_step = std::function<void(virtual_machine &, frame &, size_t instruction_pc)>;

/// Threaded JIT dispatch table for one code blob (function body or package init).
struct compiled_unit
{
    static constexpr size_t invalid_handler = static_cast<size_t>(-1);

    std::vector<std::byte> code;
    std::unordered_map<size_t, size_t> handler_at_pc;
    /// Dense map from instruction entry `pc` to `handlers` index (for fast dispatch).
    std::vector<size_t> handler_index;
    std::vector<jit_step> handlers;
    /// Profile generation used when this unit was compiled (runtime PGO re-JIT).
    uint32_t profile_generation{0};
    /// Branch/block counts for this optimized blob (runtime PGO during JIT).
    execution_profile runtime_profile;
};

std::shared_ptr<compiled_unit> compile_unit(virtual_machine &vm,
                                            std::span<const std::byte> source,
                                            std::string_view strings,
                                            const execution_profile *profile = nullptr);

} // namespace munx::vm::jit
