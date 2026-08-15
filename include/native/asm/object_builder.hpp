#pragma once

#include "target.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace munx::native::asm_backend
{

enum class reloc_kind : uint8_t
{
    // x86_64
    X86_64_PC32,   // relative 32-bit (call/jmp)
    X86_64_64,     // absolute 64-bit
    X86_64_PLT32,  // PLT-relative call
    // aarch64
    AARCH64_CALL26,
    AARCH64_ABS64,
    AARCH64_ADR_PREL_PG_HI21,
    AARCH64_ADD_ABS_LO12_NC,
};

enum class symbol_bind : uint8_t
{
    Local,
    Global,
    Undefined,
};

struct reloc
{
    uint32_t offset{0}; ///< Offset into .text
    reloc_kind kind{reloc_kind::X86_64_PC32};
    std::string symbol;
    int64_t addend{0};
};

struct symbol
{
    std::string name;
    symbol_bind bind{symbol_bind::Local};
    bool is_text{true}; ///< text vs rodata
    uint32_t value{0};  ///< offset in section (0 for undefined)
    uint32_t size{0};
};

/// In-memory relocatable object before format-specific serialization.
struct object_image
{
    cpu_arch arch{cpu_arch::x86_64};
    object_format format{object_format::elf64};
    std::vector<uint8_t> text;
    std::vector<uint8_t> rodata;
    std::vector<symbol> symbols;
    std::vector<reloc> text_relocs;

    void define_text_symbol(const std::string &name, uint32_t offset, uint32_t size,
                            bool global)
    {
        symbols.push_back(symbol{name, global ? symbol_bind::Global : symbol_bind::Local,
                                 true, offset, size});
    }

    void need_undef(const std::string &name)
    {
        for (const auto &s : symbols)
        {
            if (s.name == name)
            {
                return;
            }
        }
        symbols.push_back(
            symbol{name, symbol_bind::Undefined, true, 0, 0});
    }

    void add_reloc(uint32_t offset, reloc_kind kind, const std::string &symbol,
                   int64_t addend = 0, bool external = false)
    {
        if (external)
        {
            need_undef(symbol);
        }
        text_relocs.push_back(reloc{offset, kind, symbol, addend});
    }
};

} // namespace munx::native::asm_backend

#include "coff.hpp"
#include "elf64.hpp"
#include "macho64.hpp"

namespace munx::native::asm_backend
{

inline std::vector<uint8_t> write_object(const object_image &img)
{
    switch (img.format)
    {
    case object_format::elf64:
        return write_elf64(img);
    case object_format::macho64:
        return write_macho64(img);
    case object_format::coff64:
        return write_coff64(img);
    }
    return {};
}

} // namespace munx::native::asm_backend
