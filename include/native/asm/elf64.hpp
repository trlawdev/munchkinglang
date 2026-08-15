#pragma once

#include "object_builder.hpp"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace munx::native::asm_backend
{
namespace detail
{

inline void append_bytes(std::vector<uint8_t> &out, const void *data, size_t n)
{
    const auto *p = static_cast<const uint8_t *>(data);
    out.insert(out.end(), p, p + n);
}

template <typename T>
inline void append_pod(std::vector<uint8_t> &out, const T &v)
{
    append_bytes(out, &v, sizeof v);
}

} // namespace detail

inline std::vector<uint8_t> write_elf64(const object_image &img)
{
    using detail::append_bytes;
    using detail::append_pod;

    constexpr uint16_t EM_X86_64 = 62;
    constexpr uint16_t EM_AARCH64 = 183;
    constexpr uint16_t ET_REL = 1;
    constexpr uint16_t SHT_NULL = 0;
    constexpr uint16_t SHT_PROGBITS = 1;
    constexpr uint16_t SHT_SYMTAB = 2;
    constexpr uint16_t SHT_STRTAB = 3;
    constexpr uint16_t SHT_RELA = 4;
    constexpr uint32_t SHF_ALLOC = 0x2;
    constexpr uint32_t SHF_EXECINSTR = 0x4;
    constexpr uint32_t SHF_MERGE = 0x10;
    constexpr uint32_t SHF_STRINGS = 0x20;
    constexpr uint8_t STB_LOCAL = 0;
    constexpr uint8_t STB_GLOBAL = 1;
    constexpr uint8_t STT_NOTYPE = 0;
    constexpr uint8_t STT_FUNC = 2;
    constexpr uint8_t STT_OBJECT = 1;
    constexpr uint8_t STT_SECTION = 3;
    constexpr uint64_t R_X86_64_PC32 = 2;
    constexpr uint64_t R_X86_64_PLT32 = 4;
    constexpr uint64_t R_X86_64_64 = 1;
    constexpr uint64_t R_AARCH64_ABS64 = 257;
    constexpr uint64_t R_AARCH64_CALL26 = 283;

    const uint16_t machine =
        img.arch == cpu_arch::aarch64 ? EM_AARCH64 : EM_X86_64;

    // Section layout: 0 null, 1 .text, 2 .rodata, 3 .rela.text, 4 .shstrtab,
    // 5 .symtab, 6 .strtab
    std::string shstr = "\0.text\0.rodata\0.rela.text\0.shstrtab\0.symtab\0.strtab";
    shstr.push_back('\0');

    std::string strtab(1, '\0');
    auto intern = [&](const std::string &s) -> uint32_t {
        const uint32_t off = static_cast<uint32_t>(strtab.size());
        strtab += s;
        strtab.push_back('\0');
        return off;
    };

    struct Elf64_Sym
    {
        uint32_t st_name;
        uint8_t st_info;
        uint8_t st_other;
        uint16_t st_shndx;
        uint64_t st_value;
        uint64_t st_size;
    };

    std::vector<Elf64_Sym> syms;
    syms.push_back({}); // undef index 0

    // Section symbols for reloc targets
    const uint16_t text_ndx = 1;
    const uint16_t rodata_ndx = 2;

    Elf64_Sym text_sec{};
    text_sec.st_info = static_cast<uint8_t>((STB_LOCAL << 4) | STT_SECTION);
    text_sec.st_shndx = text_ndx;
    syms.push_back(text_sec);

    Elf64_Sym rodata_sec{};
    rodata_sec.st_info = static_cast<uint8_t>((STB_LOCAL << 4) | STT_SECTION);
    rodata_sec.st_shndx = rodata_ndx;
    syms.push_back(rodata_sec);

    std::unordered_map<std::string, uint32_t> sym_index;
    // locals first (ELF requires locals before globals in symtab for sh_info)
    for (const auto &s : img.symbols)
    {
        if (s.bind != symbol_bind::Local)
        {
            continue;
        }
        Elf64_Sym sym{};
        sym.st_name = intern(s.name);
        sym.st_info = static_cast<uint8_t>((STB_LOCAL << 4) |
                                           (s.is_text ? STT_FUNC : STT_OBJECT));
        sym.st_shndx = s.is_text ? text_ndx : rodata_ndx;
        sym.st_value = s.value;
        sym.st_size = s.size;
        sym_index[s.name] = static_cast<uint32_t>(syms.size());
        syms.push_back(sym);
    }
    const uint32_t first_nonlocal = static_cast<uint32_t>(syms.size());
    for (const auto &s : img.symbols)
    {
        if (s.bind == symbol_bind::Local)
        {
            continue;
        }
        Elf64_Sym sym{};
        sym.st_name = intern(s.name);
        if (s.bind == symbol_bind::Undefined)
        {
            sym.st_info = static_cast<uint8_t>((STB_GLOBAL << 4) | STT_NOTYPE);
            sym.st_shndx = 0;
        }
        else
        {
            sym.st_info = static_cast<uint8_t>((STB_GLOBAL << 4) |
                                               (s.is_text ? STT_FUNC : STT_OBJECT));
            sym.st_shndx = s.is_text ? text_ndx : rodata_ndx;
            sym.st_value = s.value;
            sym.st_size = s.size;
        }
        sym_index[s.name] = static_cast<uint32_t>(syms.size());
        syms.push_back(sym);
    }

    struct Elf64_Rela
    {
        uint64_t r_offset;
        uint64_t r_info;
        int64_t r_addend;
    };
    std::vector<Elf64_Rela> relas;
    for (const auto &r : img.text_relocs)
    {
        const auto it = sym_index.find(r.symbol);
        if (it == sym_index.end())
        {
            continue;
        }
        uint64_t type = R_X86_64_PC32;
        if (img.arch == cpu_arch::x86_64)
        {
            switch (r.kind)
            {
            case reloc_kind::X86_64_PC32:
                type = R_X86_64_PC32;
                break;
            case reloc_kind::X86_64_PLT32:
                type = R_X86_64_PLT32;
                break;
            case reloc_kind::X86_64_64:
                type = R_X86_64_64;
                break;
            default:
                type = R_X86_64_PLT32;
                break;
            }
        }
        else
        {
            switch (r.kind)
            {
            case reloc_kind::AARCH64_CALL26:
                type = R_AARCH64_CALL26;
                break;
            case reloc_kind::AARCH64_ABS64:
                type = R_AARCH64_ABS64;
                break;
            default:
                type = R_AARCH64_CALL26;
                break;
            }
        }
        Elf64_Rela rela{};
        rela.r_offset = r.offset;
        rela.r_info = (static_cast<uint64_t>(it->second) << 32) | type;
        rela.r_addend = r.addend;
        relas.push_back(rela);
    }

    // Offsets after ELF header (64 bytes)
    constexpr uint64_t ehdr_size = 64;
    constexpr uint64_t shdr_size = 64;
    constexpr uint16_t shnum = 7;

    uint64_t off = ehdr_size;
    const uint64_t text_off = off;
    off += img.text.size();
    const uint64_t rodata_off = off;
    off += img.rodata.size();
    // align rela
    while (off % 8)
    {
        ++off;
    }
    const uint64_t rela_off = off;
    off += relas.size() * sizeof(Elf64_Rela);
    const uint64_t shstr_off = off;
    off += shstr.size();
    while (off % 8)
    {
        ++off;
    }
    const uint64_t sym_off = off;
    off += syms.size() * sizeof(Elf64_Sym);
    const uint64_t str_off = off;
    off += strtab.size();
    while (off % 8)
    {
        ++off;
    }
    const uint64_t shoff = off;

    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(shoff + shnum * shdr_size));

    // Elf64_Ehdr
    uint8_t ehdr[64]{};
    ehdr[0] = 0x7f;
    ehdr[1] = 'E';
    ehdr[2] = 'L';
    ehdr[3] = 'F';
    ehdr[4] = 2; // ELFCLASS64
    ehdr[5] = 1; // ELFDATA2LSB
    ehdr[6] = 1; // EV_CURRENT
    std::memcpy(ehdr + 16, &ET_REL, 2);
    std::memcpy(ehdr + 18, &machine, 2);
    uint32_t version = 1;
    std::memcpy(ehdr + 20, &version, 4);
    std::memcpy(ehdr + 40, &shoff, 8);
    uint16_t ehsize = 64;
    uint16_t shentsize = 64;
    uint16_t shstrndx = 4;
    std::memcpy(ehdr + 52, &ehsize, 2);
    std::memcpy(ehdr + 58, &shentsize, 2);
    std::memcpy(ehdr + 60, &shnum, 2);
    std::memcpy(ehdr + 62, &shstrndx, 2);
    append_bytes(out, ehdr, 64);

    auto pad_to = [&](uint64_t target) {
        while (out.size() < target)
        {
            out.push_back(0);
        }
    };

    pad_to(text_off);
    append_bytes(out, img.text.data(), img.text.size());
    pad_to(rodata_off);
    append_bytes(out, img.rodata.data(), img.rodata.size());
    pad_to(rela_off);
    for (const auto &r : relas)
    {
        append_pod(out, r);
    }
    pad_to(shstr_off);
    append_bytes(out, shstr.data(), shstr.size());
    pad_to(sym_off);
    for (const auto &s : syms)
    {
        append_pod(out, s);
    }
    pad_to(str_off);
    append_bytes(out, strtab.data(), strtab.size());
    pad_to(shoff);

    auto write_shdr = [&](uint32_t name, uint32_t type, uint64_t flags,
                          uint64_t addr, uint64_t offset, uint64_t size,
                          uint32_t link, uint32_t info, uint64_t addralign,
                          uint64_t entsize) {
        uint8_t sh[64]{};
        std::memcpy(sh + 0, &name, 4);
        std::memcpy(sh + 4, &type, 4);
        std::memcpy(sh + 8, &flags, 8);
        std::memcpy(sh + 16, &addr, 8);
        std::memcpy(sh + 24, &offset, 8);
        std::memcpy(sh + 32, &size, 8);
        std::memcpy(sh + 40, &link, 4);
        std::memcpy(sh + 44, &info, 4);
        std::memcpy(sh + 48, &addralign, 8);
        std::memcpy(sh + 56, &entsize, 8);
        append_bytes(out, sh, 64);
    };

    // Find shstr offsets
    auto sh_off = [&](const char *name) -> uint32_t {
        const char *p = shstr.data();
        const char *end = p + shstr.size();
        while (p < end)
        {
            if (std::strcmp(p, name) == 0)
            {
                return static_cast<uint32_t>(p - shstr.data());
            }
            p += std::strlen(p) + 1;
        }
        return 0;
    };

    write_shdr(0, SHT_NULL, 0, 0, 0, 0, 0, 0, 0, 0);
    write_shdr(sh_off(".text"), SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 0, text_off,
               img.text.size(), 0, 0, 16, 0);
    write_shdr(sh_off(".rodata"), SHT_PROGBITS, SHF_ALLOC | SHF_MERGE | SHF_STRINGS, 0,
               rodata_off, img.rodata.size(), 0, 0, 1, 1);
    write_shdr(sh_off(".rela.text"), SHT_RELA, 0, 0, rela_off,
               relas.size() * sizeof(Elf64_Rela), 5, 1, 8, sizeof(Elf64_Rela));
    write_shdr(sh_off(".shstrtab"), SHT_STRTAB, 0, 0, shstr_off, shstr.size(), 0, 0, 1,
               0);
    write_shdr(sh_off(".symtab"), SHT_SYMTAB, 0, 0, sym_off,
               syms.size() * sizeof(Elf64_Sym), 6, first_nonlocal, 8, sizeof(Elf64_Sym));
    write_shdr(sh_off(".strtab"), SHT_STRTAB, 0, 0, str_off, strtab.size(), 0, 0, 1, 0);

    return out;
}

} // namespace munx::native::asm_backend
