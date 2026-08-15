#pragma once

#include "../isa/mx32.hpp"
#include "../vm_value.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace munx::vm::jit
{

inline constexpr size_t k_phys_regs = 128;
inline constexpr size_t k_rob_size = 64;
inline constexpr size_t k_rs_size = 32;
inline constexpr size_t k_stb_size = 16;
inline constexpr size_t k_branch_stack = 8;

enum class fu_kind : uint8_t
{
    alu,
    branch,
    mem,
    runtime, // print / serialize
};

struct uop
{
    munx::isa::mx_op op{munx::isa::mx_op::NOP};
    uint32_t pc{0};
    uint32_t arch_rd{0};
    uint32_t arch_rs1{0};
    uint32_t arch_rs2{0};
    int32_t imm{0};
    uint32_t imm_u{0};
    bool has_rd{false};
    bool serialize{false};
    fu_kind fu{fu_kind::alu};
    /// Memory address for LD_SLOT/ST_SLOT (frame slot index). Known at decode.
    bool mem_addr_known{false};
    uint32_t mem_slot{0};
    bool is_load{false};
    bool is_store{false};
};

/// True if two memory ops may access the same location (must order).
/// When both addresses are known and differ, the dependence is false — safe to
/// reorder (no alias).
[[nodiscard]] inline bool may_alias_mem(const uop &a, const uop &b)
{
    if (!a.mem_addr_known || !b.mem_addr_known)
    {
        return true; // conservative
    }
    return a.mem_slot == b.mem_slot;
}

struct rob_entry
{
    bool valid{false};
    bool complete{false};
    bool has_dest{false};
    bool exception{false};
    bool mispredict{false};
    uint32_t pc{0};
    munx::isa::mx_op op{munx::isa::mx_op::NOP};
    uint32_t arch_rd{0};
    uint32_t dest_phys{0};
    uint32_t old_phys{0};
    uint32_t correct_pc{0};
    vm::value result{};
    bool side_effect{false};
    fu_kind fu{fu_kind::alu};
    uop decoded{};
};

struct rs_entry
{
    bool valid{false};
    uint32_t rob_id{0};
    uop op{};
    uint32_t phys_src1{0};
    uint32_t phys_src2{0};
    uint32_t phys_dest{0};
    bool src1_ready{false};
    bool src2_ready{false};
    vm::value src1{};
    vm::value src2{};
};

struct rat_checkpoint
{
    uint32_t rob_id{0};
    std::array<uint32_t, munx::isa::k_arch_regs> map{};
};

struct ooo_state
{
    std::array<vm::value, k_phys_regs> phys_rf{};
    std::array<bool, k_phys_regs> phys_ready{};
    std::array<uint32_t, munx::isa::k_arch_regs> rat{};     ///< frontend rename
    std::array<uint32_t, munx::isa::k_arch_regs> arch_rat{}; ///< commit map
    std::vector<uint32_t> free_list;
    std::array<rob_entry, k_rob_size> rob{};
    uint32_t rob_head{0};
    uint32_t rob_tail{0};
    uint32_t rob_count{0};
    std::array<rs_entry, k_rs_size> rs{};
    std::vector<rat_checkpoint> branch_stack;

    void reset()
    {
        free_list.clear();
        for (uint32_t i = 0; i < k_phys_regs; ++i)
        {
            phys_rf[i] = vm::value{};
            phys_ready[i] = true;
            if (i >= munx::isa::k_arch_regs)
            {
                free_list.push_back(i);
            }
        }
        for (uint32_t i = 0; i < munx::isa::k_arch_regs; ++i)
        {
            rat[i] = i;
            arch_rat[i] = i;
            phys_rf[i] = vm::value{};
            phys_ready[i] = true;
        }
        rob = {};
        rob_head = rob_tail = rob_count = 0;
        rs = {};
        branch_stack.clear();
    }

    [[nodiscard]] bool rob_full() const { return rob_count >= k_rob_size; }

    std::optional<uint32_t> alloc_phys()
    {
        if (free_list.empty())
        {
            return std::nullopt;
        }
        const uint32_t p = free_list.back();
        free_list.pop_back();
        phys_ready[p] = false;
        return p;
    }

    void free_phys(uint32_t p)
    {
        if (p >= munx::isa::k_arch_regs)
        {
            free_list.push_back(p);
            phys_ready[p] = true;
        }
    }
};

} // namespace munx::vm::jit
