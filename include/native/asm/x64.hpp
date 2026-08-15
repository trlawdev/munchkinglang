#pragma once

#include "object_builder.hpp"

#include <cstdint>
#include <vector>

namespace munx::native::asm_backend::x64
{

enum reg : uint8_t
{
    RAX = 0,
    RCX = 1,
    RDX = 2,
    RBX = 3,
    RSP = 4,
    RBP = 5,
    RSI = 6,
    RDI = 7,
    R8 = 8,
    R9 = 9,
    R10 = 10,
    R11 = 11,
    R12 = 12,
    R13 = 13,
    R14 = 14,
    R15 = 15,
};

struct assembler
{
    object_image *img{nullptr};
    std::vector<uint8_t> &text() { return img->text; }

    void emit_u8(uint8_t b) { text().push_back(b); }
    void emit_u32(uint32_t v)
    {
        emit_u8(static_cast<uint8_t>(v));
        emit_u8(static_cast<uint8_t>(v >> 8));
        emit_u8(static_cast<uint8_t>(v >> 16));
        emit_u8(static_cast<uint8_t>(v >> 24));
    }
    void emit_u64(uint64_t v)
    {
        emit_u32(static_cast<uint32_t>(v));
        emit_u32(static_cast<uint32_t>(v >> 32));
    }

    void rex(bool w, uint8_t reg, uint8_t rm)
    {
        uint8_t r = 0x40;
        if (w)
        {
            r |= 0x08;
        }
        if (reg & 8)
        {
            r |= 0x04;
        }
        if (rm & 8)
        {
            r |= 0x01;
        }
        if (r != 0x40 || (reg & 8) || (rm & 8) || w)
        {
            // Always emit REX when w or high regs; also for sil/dil etc. we use 64-bit ops
            if (w || (reg & 8) || (rm & 8))
            {
                emit_u8(static_cast<uint8_t>(0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) |
                                             ((rm & 8) ? 1 : 0)));
            }
        }
    }

    void rex_w(uint8_t reg, uint8_t rm)
    {
        emit_u8(static_cast<uint8_t>(0x48 | ((reg & 8) ? 4 : 0) | ((rm & 8) ? 1 : 0)));
    }

    void modrm(uint8_t mod, uint8_t reg, uint8_t rm)
    {
        emit_u8(static_cast<uint8_t>((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
    }

    /// push %reg
    void push(reg r)
    {
        if (r & 8)
        {
            emit_u8(0x41);
        }
        emit_u8(static_cast<uint8_t>(0x50 + (r & 7)));
    }

    /// pop %reg
    void pop(reg r)
    {
        if (r & 8)
        {
            emit_u8(0x41);
        }
        emit_u8(static_cast<uint8_t>(0x58 + (r & 7)));
    }

    /// mov %rsp, %rbp
    void mov_rr(reg dst, reg src)
    {
        rex_w(src, dst);
        emit_u8(0x89);
        modrm(3, src, dst);
    }

    /// mov imm64, %reg
    void mov_ri64(reg dst, uint64_t imm)
    {
        emit_u8(static_cast<uint8_t>(0x48 | ((dst & 8) ? 1 : 0)));
        emit_u8(static_cast<uint8_t>(0xB8 + (dst & 7)));
        emit_u64(imm);
    }

    /// mov imm32 sign-extended, %reg
    void mov_ri32(reg dst, int32_t imm)
    {
        rex_w(0, dst);
        emit_u8(0xC7);
        modrm(3, 0, dst);
        emit_u32(static_cast<uint32_t>(imm));
    }

    /// xor %reg, %reg
    void xor_rr(reg r)
    {
        if (r & 8)
        {
            emit_u8(0x45);
            emit_u8(0x31);
            modrm(3, r, r);
        }
        else
        {
            emit_u8(0x31);
            modrm(3, r, r);
        }
    }

    void xor_eax()
    {
        emit_u8(0x31);
        emit_u8(0xC0);
    }

    /// sub imm32, %rsp
    void sub_rsp(int32_t imm)
    {
        rex_w(0, RSP);
        emit_u8(0x81);
        modrm(3, 5, RSP);
        emit_u32(static_cast<uint32_t>(imm));
    }

    /// add imm32, %rsp
    void add_rsp(int32_t imm)
    {
        rex_w(0, RSP);
        emit_u8(0x81);
        modrm(3, 0, RSP);
        emit_u32(static_cast<uint32_t>(imm));
    }

    /// lea disp(%rbp), %reg
    void lea_rbp(reg dst, int32_t disp)
    {
        rex_w(dst, RBP);
        emit_u8(0x8D);
        if (disp >= -128 && disp <= 127)
        {
            modrm(1, dst, RBP);
            emit_u8(static_cast<uint8_t>(disp));
        }
        else
        {
            modrm(2, dst, RBP);
            emit_u32(static_cast<uint32_t>(disp));
        }
    }

    /// mov %reg, disp(%rbp)
    void store64_rbp(reg src, int32_t disp)
    {
        rex_w(src, RBP);
        emit_u8(0x89);
        if (disp >= -128 && disp <= 127)
        {
            modrm(1, src, RBP);
            emit_u8(static_cast<uint8_t>(disp));
        }
        else
        {
            modrm(2, src, RBP);
            emit_u32(static_cast<uint32_t>(disp));
        }
    }

    /// mov disp(%rbp), %reg
    void load64_rbp(reg dst, int32_t disp)
    {
        rex_w(dst, RBP);
        emit_u8(0x8B);
        if (disp >= -128 && disp <= 127)
        {
            modrm(1, dst, RBP);
            emit_u8(static_cast<uint8_t>(disp));
        }
        else
        {
            modrm(2, dst, RBP);
            emit_u32(static_cast<uint32_t>(disp));
        }
    }

    /// movzbl disp(%rbp), %reg32 (zero-extend byte)
    void load8z_rbp(reg dst, int32_t disp)
    {
        // REX.W optional; use 32-bit movzx
        uint8_t rex = 0x40;
        bool need = false;
        if (dst & 8)
        {
            rex |= 0x04;
            need = true;
        }
        if (need)
        {
            emit_u8(rex);
        }
        emit_u8(0x0F);
        emit_u8(0xB6);
        if (disp >= -128 && disp <= 127)
        {
            modrm(1, dst, RBP);
            emit_u8(static_cast<uint8_t>(disp));
        }
        else
        {
            modrm(2, dst, RBP);
            emit_u32(static_cast<uint32_t>(disp));
        }
    }

    /// movb %al, disp(%rbp)
    void store8_al_rbp(int32_t disp)
    {
        emit_u8(0x88);
        if (disp >= -128 && disp <= 127)
        {
            modrm(1, RAX, RBP);
            emit_u8(static_cast<uint8_t>(disp));
        }
        else
        {
            modrm(2, RAX, RBP);
            emit_u32(static_cast<uint32_t>(disp));
        }
    }

    /// movb $imm, disp(%rbp)
    void store8i_rbp(int32_t disp, uint8_t imm)
    {
        emit_u8(0xC6);
        if (disp >= -128 && disp <= 127)
        {
            modrm(1, 0, RBP);
            emit_u8(static_cast<uint8_t>(disp));
        }
        else
        {
            modrm(2, 0, RBP);
            emit_u32(static_cast<uint32_t>(disp));
        }
        emit_u8(imm);
    }

    /// movq $imm32 sign-ext, disp(%rbp)
    void store64i_rbp(int32_t disp, int32_t imm)
    {
        rex_w(0, RBP);
        emit_u8(0xC7);
        if (disp >= -128 && disp <= 127)
        {
            modrm(1, 0, RBP);
            emit_u8(static_cast<uint8_t>(disp));
        }
        else
        {
            modrm(2, 0, RBP);
            emit_u32(static_cast<uint32_t>(disp));
        }
        emit_u32(static_cast<uint32_t>(imm));
    }

    /// test %al, %al
    void test_al()
    {
        emit_u8(0x84);
        emit_u8(0xC0);
    }

    /// jmp rel32 — returns patch offset of displacement
    uint32_t jmp_rel32()
    {
        emit_u8(0xE9);
        const uint32_t off = static_cast<uint32_t>(text().size());
        emit_u32(0);
        return off;
    }

    /// je/jne rel32 — returns patch offset
    uint32_t jcc_rel32(uint8_t cc)
    {
        emit_u8(0x0F);
        emit_u8(cc);
        const uint32_t off = static_cast<uint32_t>(text().size());
        emit_u32(0);
        return off;
    }

    uint32_t je_rel32() { return jcc_rel32(0x84); }
    uint32_t jne_rel32() { return jcc_rel32(0x85); }

    void patch_rel32(uint32_t disp_off, uint32_t target)
    {
        const int32_t rel =
            static_cast<int32_t>(target) - static_cast<int32_t>(disp_off + 4);
        text()[disp_off + 0] = static_cast<uint8_t>(rel);
        text()[disp_off + 1] = static_cast<uint8_t>(rel >> 8);
        text()[disp_off + 2] = static_cast<uint8_t>(rel >> 16);
        text()[disp_off + 3] = static_cast<uint8_t>(rel >> 24);
    }

    /// call rel32 to symbol (PLT reloc); displacement field gets -4 addend
    void call_plt(const std::string &sym)
    {
        emit_u8(0xE8);
        const uint32_t off = static_cast<uint32_t>(text().size());
        emit_u32(0);
        img->add_reloc(off, reloc_kind::X86_64_PLT32, sym, -4, true);
    }

    /// call relative to local symbol (PC32)
    void call_local(const std::string &sym)
    {
        emit_u8(0xE8);
        const uint32_t off = static_cast<uint32_t>(text().size());
        emit_u32(0);
        img->add_reloc(off, reloc_kind::X86_64_PC32, sym, -4, false);
    }

    void ret() { emit_u8(0xC3); }

    void nop() { emit_u8(0x90); }

    /// lea rip-relative rodata into %reg — needs special reloc; use GOT-style:
    /// For ELF we emit: lea label(%rip), %reg with R_X86_64_PC32 to section+addend
    /// Simpler: movabs imm64 address patched — use lea with reloc to local rodata symbol
    void lea_rip_symbol(reg dst, const std::string &sym)
    {
        // rex.w lea disp32(%rip), %dst
        emit_u8(static_cast<uint8_t>(0x48 | ((dst & 8) ? 4 : 0)));
        emit_u8(0x8D);
        emit_u8(static_cast<uint8_t>(0x05 | ((dst & 7) << 3))); // mod=0 rm=5 → rip
        const uint32_t off = static_cast<uint32_t>(text().size());
        emit_u32(0);
        img->add_reloc(off, reloc_kind::X86_64_PC32, sym, -4, false);
    }
};

} // namespace munx::native::asm_backend::x64
