#pragma once

#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

namespace munx::vm::jit
{

/// Per-branch-site counters collected at runtime (interpreter and JIT).
struct branch_profile
{
    uint32_t taken{0};
    uint32_t not_taken{0};

    [[nodiscard]] uint32_t total() const noexcept { return taken + not_taken; }

    [[nodiscard]] bool strongly_taken() const noexcept
    {
        const uint32_t count = total();
        return count >= 32 && taken * 20 >= count * 19;
    }

    [[nodiscard]] bool strongly_not_taken() const noexcept
    {
        const uint32_t count = total();
        return count >= 32 && not_taken * 20 >= count * 19;
    }

    [[nodiscard]] bool biased_taken() const noexcept
    {
        const uint32_t count = total();
        return count >= 8 && taken * 3 >= count * 2;
    }

    [[nodiscard]] bool biased_not_taken() const noexcept
    {
        const uint32_t count = total();
        return count >= 8 && not_taken * 3 >= count * 2;
    }
};

/// Execution profile for one immutable bytecode blob (function body / init).
struct execution_profile
{
    uint32_t generation{0};
    uint32_t interpret_runs{0};
    std::unordered_map<size_t, uint32_t> block_hits;
    std::unordered_map<size_t, branch_profile> branches;

    void record_block(size_t pc) noexcept
    {
        ++block_hits[pc];
    }

    void record_branch(size_t branch_pc, bool taken) noexcept
    {
        branch_profile &site = branches[branch_pc];
        if (taken)
        {
            ++site.taken;
        }
        else
        {
            ++site.not_taken;
        }
        const uint32_t total = site.total();
        if (total == 32 || total == 128 || total == 512)
        {
            bump_generation();
        }
    }

    void note_interpret_run() noexcept { ++interpret_runs; }

    void bump_generation() noexcept { ++generation; }

    [[nodiscard]] uint32_t branch_total(size_t branch_pc) const noexcept
    {
        const auto found = branches.find(branch_pc);
        if (found == branches.end())
        {
            return 0;
        }
        return found->second.total();
    }

    [[nodiscard]] bool branch_likely_taken(size_t branch_pc) const noexcept
    {
        const auto found = branches.find(branch_pc);
        if (found == branches.end())
        {
            return false;
        }
        return found->second.biased_taken();
    }

    [[nodiscard]] bool is_mature() const noexcept
    {
        uint32_t branch_events = 0;
        for (const auto &[_, site] : branches)
        {
            branch_events += site.total();
        }
        return branch_events >= 256 || interpret_runs >= 1;
    }
};

struct bytecode_profile_key
{
    const std::byte *data{nullptr};
    size_t size{0};

    bool operator==(const bytecode_profile_key &other) const noexcept
    {
        if (size != other.size)
        {
            return false;
        }
        for (size_t index = 0; index < size; ++index)
        {
            if (data[index] != other.data[index])
            {
                return false;
            }
        }
        return true;
    }
};

struct bytecode_profile_key_hash
{
    size_t operator()(const bytecode_profile_key &key) const noexcept
    {
        size_t hash = key.size;
        for (size_t index = 0; index < key.size; ++index)
        {
            hash ^= static_cast<size_t>(key.data[index]) + 0x9e3779b9U + (hash << 6) +
                    (hash >> 2);
        }
        return hash;
    }
};

/// Process-wide profile store keyed by raw bytecode bytes.
class profile_registry
{
public:
    static profile_registry &instance()
    {
        static profile_registry registry;
        return registry;
    }

    execution_profile &get(std::span<const std::byte> code)
    {
        const bytecode_profile_key key{code.data(), code.size()};
        return profiles_[key];
    }

    [[nodiscard]] const execution_profile *find(std::span<const std::byte> code) const
    {
        const bytecode_profile_key key{code.data(), code.size()};
        const auto found = profiles_.find(key);
        if (found == profiles_.end())
        {
            return nullptr;
        }
        return &found->second;
    }

private:
    std::unordered_map<bytecode_profile_key, execution_profile, bytecode_profile_key_hash>
        profiles_;
};

/// When true, runtime profiles drive bytecode specialization before JIT compile.
inline bool pgo_enabled()
{
    if (const char *configured = std::getenv("MUNX_VM_PGO"))
    {
        return configured[0] != '0' || configured[1] != '\0';
    }
    return true;
}

/// Interpreted invocations per code blob before first JIT compile (0 = disable).
inline unsigned jit_warmup_invocations()
{
    if (const char *configured = std::getenv("MUNX_VM_JIT_WARMUP"))
    {
        return static_cast<unsigned>(std::strtoul(configured, nullptr, 10));
    }
    return 0;
}

} // namespace munx::vm::jit
