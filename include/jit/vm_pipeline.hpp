#pragma once

#include "jit_compiler.hpp"
#include <cstddef>
#include <optional>

namespace munx::vm::jit
{

/// Three-stage overlap for threaded JIT dispatch (v8 stack path).
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
