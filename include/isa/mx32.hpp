#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace munx::isa
{

/// Architectural register count for mx32.
inline constexpr uint32_t k_arch_regs = 32;
inline constexpr uint32_t k_r0 = 0; ///< Hardwired null / zero-ish result sink.

/// Fixed 32-bit Munx ISA (MX bytecode v9+).
enum class mx_op : uint8_t
{
    NOP = 0,
    HALT,
    RET,
    MOV,     ///< rd = rs1
    LDC,     ///< rd = pool[imm14]  (I-type: rs1 unused)
    LI,      ///< rd = sign_ext(imm19) as i64 (U-type)
    ADD,
    SUB,
    MUL,
    DIV,
    MOD,
    NEG,     ///< rd = -rs1
    NOT,     ///< rd = !rs1
    EQ,
    NE,
    LT,
    GT,
    LE,
    GE,
    LD_SLOT, ///< rd = slots[rs1 + imm]
    ST_SLOT, ///< slots[rd + imm] = rs1  (rd used as base)
    PRINT,   ///< print rs1
    PRINTLN,
    JMP,     ///< pc += imm19 (insn units; B-type, cond field unused)
    BEQ,     ///< if rs1 truthy-eq? use: if (rs1) pc += imm; encoded as B with rs in cond nibble via custom
    BNE,
    BR_TRUE,  ///< if truthy(rs1) pc += imm19; rs1 in rd field of B-form helper
    BR_FALSE, ///< if !truthy(rs1) pc += imm19
    CALL,     ///< rd = call rs1 with argc=imm9 (args in R1..)
    ARGV_LEN, ///< rd = munx_argv_len()
    ARGV_GET, ///< rd = munx_argv_get(rs1)
    HINT,     ///< imm8 expected_taken for next branch (sideband)
};

enum class pool_tag : uint8_t
{
    i64 = 1,
    f64 = 2,
    string = 3,
    boolean = 4,
    null = 5,
};

struct pool_entry
{
    pool_tag tag{pool_tag::null};
    int64_t i64{0};
    double f64{0};
    bool b{false};
    std::string str;
};

struct mx_module
{
    std::vector<uint32_t> code;
    std::vector<pool_entry> pool;
    /// Function entry points: name → code index (insn index).
    struct fn
    {
        std::string name;
        uint32_t entry{0};
        uint32_t param_count{0};
        uint32_t local_slots{0}; ///< spill slots beyond regs
    };
    std::vector<fn> functions;
    uint32_t entry_pc{0}; ///< package init entry
};

// ---- bit packing -----------------------------------------------------------

inline constexpr uint32_t enc_r(mx_op op, uint32_t rd, uint32_t rs1, uint32_t rs2,
                                uint32_t imm9 = 0)
{
    return (static_cast<uint32_t>(op) << 24) | ((rd & 31u) << 19) |
           ((rs1 & 31u) << 14) | ((rs2 & 31u) << 9) | (imm9 & 0x1ffu);
}

inline constexpr uint32_t enc_i(mx_op op, uint32_t rd, uint32_t rs1, uint32_t imm14)
{
    return (static_cast<uint32_t>(op) << 24) | ((rd & 31u) << 19) |
           ((rs1 & 31u) << 14) | (imm14 & 0x3fffu);
}

inline constexpr uint32_t enc_u(mx_op op, uint32_t rd, uint32_t imm19)
{
    return (static_cast<uint32_t>(op) << 24) | ((rd & 31u) << 19) | (imm19 & 0x7ffffu);
}

/// Branch / jump: imm19 is signed insn delta packed in low 19 bits (twos-complement).
inline constexpr uint32_t enc_b(mx_op op, uint32_t reg_or_cond, int32_t imm19)
{
    const uint32_t imm = static_cast<uint32_t>(imm19) & 0x7ffffu;
    return (static_cast<uint32_t>(op) << 24) | ((reg_or_cond & 31u) << 19) | imm;
}

inline constexpr mx_op dec_op(uint32_t w) { return static_cast<mx_op>((w >> 24) & 0xffu); }
inline constexpr uint32_t dec_rd(uint32_t w) { return (w >> 19) & 31u; }
inline constexpr uint32_t dec_rs1(uint32_t w) { return (w >> 14) & 31u; }
inline constexpr uint32_t dec_rs2(uint32_t w) { return (w >> 9) & 31u; }
inline constexpr uint32_t dec_imm9(uint32_t w) { return w & 0x1ffu; }
inline constexpr uint32_t dec_imm14(uint32_t w) { return w & 0x3fffu; }

inline constexpr int32_t dec_imm19_signed(uint32_t w)
{
    const uint32_t u = w & 0x7ffffu;
    if (u & 0x40000u)
    {
        return static_cast<int32_t>(u | 0xFFF80000u);
    }
    return static_cast<int32_t>(u);
}

inline const char *op_name(mx_op op)
{
    switch (op)
    {
    case mx_op::NOP:
        return "NOP";
    case mx_op::HALT:
        return "HALT";
    case mx_op::RET:
        return "RET";
    case mx_op::MOV:
        return "MOV";
    case mx_op::LDC:
        return "LDC";
    case mx_op::LI:
        return "LI";
    case mx_op::ADD:
        return "ADD";
    case mx_op::SUB:
        return "SUB";
    case mx_op::MUL:
        return "MUL";
    case mx_op::DIV:
        return "DIV";
    case mx_op::MOD:
        return "MOD";
    case mx_op::NEG:
        return "NEG";
    case mx_op::NOT:
        return "NOT";
    case mx_op::EQ:
        return "EQ";
    case mx_op::NE:
        return "NE";
    case mx_op::LT:
        return "LT";
    case mx_op::GT:
        return "GT";
    case mx_op::LE:
        return "LE";
    case mx_op::GE:
        return "GE";
    case mx_op::LD_SLOT:
        return "LD_SLOT";
    case mx_op::ST_SLOT:
        return "ST_SLOT";
    case mx_op::PRINT:
        return "PRINT";
    case mx_op::PRINTLN:
        return "PRINTLN";
    case mx_op::JMP:
        return "JMP";
    case mx_op::BEQ:
        return "BEQ";
    case mx_op::BNE:
        return "BNE";
    case mx_op::BR_TRUE:
        return "BR_TRUE";
    case mx_op::BR_FALSE:
        return "BR_FALSE";
    case mx_op::CALL:
        return "CALL";
    case mx_op::ARGV_LEN:
        return "ARGV_LEN";
    case mx_op::ARGV_GET:
        return "ARGV_GET";
    case mx_op::HINT:
        return "HINT";
    }
    return "???";
}

} // namespace munx::isa
