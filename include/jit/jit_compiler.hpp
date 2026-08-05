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
    std::vector<std::byte> code;
    std::unordered_map<size_t, size_t> handler_at_pc;
    std::vector<jit_step> handlers;
};

std::shared_ptr<compiled_unit> compile_unit(virtual_machine &vm,
                                            std::span<const std::byte> source,
                                            std::string_view strings);

} // namespace munx::vm::jit
