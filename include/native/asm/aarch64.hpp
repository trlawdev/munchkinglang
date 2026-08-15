#pragma once

#include "object_builder.hpp"

#include <cstdint>
#include <vector>

namespace munx::native::asm_backend::aarch64
{

/// Minimal AArch64 instruction helpers (scaffolding for future encoder).
struct assembler
{
    object_image *img{nullptr};
    std::vector<uint8_t> &text() { return img->text; }

    void emit_u32(uint32_t insn)
    {
        text().push_back(static_cast<uint8_t>(insn));
        text().push_back(static_cast<uint8_t>(insn >> 8));
        text().push_back(static_cast<uint8_t>(insn >> 16));
        text().push_back(static_cast<uint8_t>(insn >> 24));
    }

    /// RET
    void ret() { emit_u32(0xD65F03C0); }

    /// BL rel26 to symbol (CALL26 reloc)
    void bl(const std::string &sym, bool external)
    {
        const uint32_t off = static_cast<uint32_t>(text().size());
        emit_u32(0x94000000); // BL #0
        img->add_reloc(off, reloc_kind::AARCH64_CALL26, sym, 0, external);
    }

    /// NOP
    void nop() { emit_u32(0xD503201F); }
};

} // namespace munx::native::asm_backend::aarch64
