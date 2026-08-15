#pragma once

#include "bytecode_optimizer.hpp"
#include "execution_profile.hpp"
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace munx::vm
{
class virtual_machine;
struct frame;
} // namespace munx::vm

namespace munx::vm::jit
{

/// Direct-call handler: function pointer + heap payload (no std::function).
struct jit_step
{
    using call_fn = void (*)(void *, virtual_machine &, frame &, size_t);

    call_fn call{nullptr};
    std::shared_ptr<void> self;

    void operator()(virtual_machine &vm, frame &current, size_t instruction_pc) const
    {
        call(self.get(), vm, current, instruction_pc);
    }
};

/// Box a movable callable into a @ref jit_step.
template <typename F>
jit_step make_step(F &&fn)
{
    using Fn = std::decay_t<F>;
    auto heap = std::make_shared<Fn>(std::forward<F>(fn));
    jit_step step;
    step.self = heap;
    step.call = [](void *self, virtual_machine &vm, frame &current,
                   size_t instruction_pc) {
        (*static_cast<Fn *>(self))(vm, current, instruction_pc);
    };
    return step;
}

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
