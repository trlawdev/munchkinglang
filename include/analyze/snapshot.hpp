#pragma once

#include "errors.hpp"
#include "semantic_index.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace munx::analyze
{

/// Immutable analysis result for one revision (rust-analyzer Analysis snapshot).
struct analysis_snapshot
{
    std::uint64_t revision{0};
    std::atomic<bool> cancelled{false};
    std::string file;
    std::string package;
    bool ok{true};
    std::vector<diagnostic> diagnostics;
    semantic_index index;

    [[nodiscard]] bool is_cancelled() const noexcept
    {
        return cancelled.load(std::memory_order_acquire);
    }

    void cancel() noexcept { cancelled.store(true, std::memory_order_release); }
};

using snapshot_ptr = std::shared_ptr<analysis_snapshot>;

} // namespace munx::analyze
