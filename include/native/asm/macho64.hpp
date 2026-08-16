#pragma once

#include "object_builder.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace munx::native::asm_backend
{

/// Minimal Mach-O 64 relocatable (MH_OBJECT) for x86_64 / arm64.
/// Constant names are prefixed to avoid clashes with macOS <mach-o/*.h> macros.
inline std::vector<uint8_t> write_macho64(const object_image &img)
{
    constexpr uint32_t k_mh_magic_64 = 0xFEEDFACF;
    constexpr uint32_t k_mh_object = 0x1;
    constexpr uint32_t k_cpu_type_x86_64 = 0x01000007;
    constexpr uint32_t k_cpu_type_arm64 = 0x0100000C;
    constexpr uint32_t k_cpu_subtype_x86_64_all = 3;
    constexpr uint32_t k_lc_segment_64 = 0x19;
    constexpr uint32_t k_lc_symtab = 0x2;
    constexpr uint32_t k_lc_dysymtab = 0xb;
    constexpr uint8_t k_n_sect = 0xe;
    constexpr uint8_t k_n_ext = 0x01;
    constexpr uint8_t k_n_undf = 0x0;
    constexpr uint32_t k_s_attr_pure_instructions = 0x80000000;
    constexpr uint32_t k_s_attr_some_instructions = 0x00000400;

    const uint32_t cputype =
        img.arch == cpu_arch::aarch64 ? k_cpu_type_arm64 : k_cpu_type_x86_64;
    const uint32_t cpusubtype =
        img.arch == cpu_arch::aarch64 ? 0u : k_cpu_subtype_x86_64_all;

#pragma pack(push, 1)
    struct section_64
    {
        char sectname[16];
        char segname[16];
        uint64_t addr;
        uint64_t size;
        uint32_t offset;
        uint32_t align;
        uint32_t reloff;
        uint32_t nreloc;
        uint32_t flags;
        uint32_t reserved1;
        uint32_t reserved2;
        uint32_t reserved3;
    };
    struct nlist_64
    {
        uint32_t n_strx;
        uint8_t n_type;
        uint8_t n_sect;
        uint16_t n_desc;
        uint64_t n_value;
    };
#pragma pack(pop)

    std::string strtab(1, '\0');
    auto intern = [&](const std::string &s) -> uint32_t {
        const uint32_t off = static_cast<uint32_t>(strtab.size());
        strtab += s;
        strtab.push_back('\0');
        return off;
    };

    std::vector<nlist_64> ordered;
    std::unordered_map<std::string, uint32_t> ordered_index;

    auto add_sym = [&](const symbol &s) {
        nlist_64 n{};
        n.n_strx = intern(s.name);
        n.n_desc = 0;
        if (s.bind == symbol_bind::Undefined)
        {
            n.n_type = k_n_undf | k_n_ext;
            n.n_sect = 0;
            n.n_value = 0;
        }
        else if (s.bind == symbol_bind::Local)
        {
            n.n_type = k_n_sect;
            n.n_sect = s.is_text ? 1 : 2;
            n.n_value = s.value;
        }
        else
        {
            n.n_type = k_n_sect | k_n_ext;
            n.n_sect = s.is_text ? 1 : 2;
            n.n_value = s.value;
        }
        ordered_index[s.name] = static_cast<uint32_t>(ordered.size());
        ordered.push_back(n);
    };

    for (const auto &s : img.symbols)
    {
        if (s.bind == symbol_bind::Local)
        {
            add_sym(s);
        }
    }
    const uint32_t iextdefsym = static_cast<uint32_t>(ordered.size());
    uint32_t nextdef = 0;
    for (const auto &s : img.symbols)
    {
        if (s.bind == symbol_bind::Global)
        {
            add_sym(s);
            ++nextdef;
        }
    }
    const uint32_t iundefsym = iextdefsym + nextdef;
    uint32_t nundef = 0;
    for (const auto &s : img.symbols)
    {
        if (s.bind == symbol_bind::Undefined)
        {
            add_sym(s);
            ++nundef;
        }
    }

    std::vector<uint8_t> reloc_bytes;
    for (const auto &r : img.text_relocs)
    {
        const auto it = ordered_index.find(r.symbol);
        if (it == ordered_index.end())
        {
            continue;
        }
        // X86_64_RELOC_BRANCH / ARM64_RELOC_BRANCH26 = 2
        const uint32_t r_type = 2;
        int32_t addr = static_cast<int32_t>(r.offset);
        uint32_t word = 0;
        word |= (it->second & 0xffffffu);
        word |= (1u << 24); // pcrel
        word |= (2u << 25); // length = 2 → 4 bytes
        word |= (1u << 27); // extern
        word |= (r_type << 28);
        reloc_bytes.insert(reloc_bytes.end(), reinterpret_cast<uint8_t *>(&addr),
                           reinterpret_cast<uint8_t *>(&addr) + 4);
        reloc_bytes.insert(reloc_bytes.end(), reinterpret_cast<uint8_t *>(&word),
                           reinterpret_cast<uint8_t *>(&word) + 4);
    }

    constexpr uint32_t nsects = 2;
    const uint32_t seg_cmd_size =
        72 + nsects * static_cast<uint32_t>(sizeof(section_64));
    constexpr uint32_t symtab_cmd_size = 24;
    constexpr uint32_t dysym_cmd_size = 80;
    const uint32_t sizeofcmds = seg_cmd_size + symtab_cmd_size + dysym_cmd_size;
    constexpr uint32_t header_size = 32;
    const uint32_t data_start = header_size + sizeofcmds;

    uint32_t cursor = data_start;
    const uint32_t text_off = cursor;
    cursor += static_cast<uint32_t>(img.text.size());
    const uint32_t rodata_off = cursor;
    cursor += static_cast<uint32_t>(img.rodata.size());
    while (cursor % 8)
    {
        ++cursor;
    }
    const uint32_t reloc_off = cursor;
    cursor += static_cast<uint32_t>(reloc_bytes.size());
    while (cursor % 8)
    {
        ++cursor;
    }
    const uint32_t symoff = cursor;
    cursor += static_cast<uint32_t>(ordered.size() * sizeof(nlist_64));
    const uint32_t stroff = cursor;
    cursor += static_cast<uint32_t>(strtab.size());

    std::vector<uint8_t> out(cursor, 0);
    auto wr = [&](uint32_t at, const void *p, size_t n) {
        std::memcpy(out.data() + at, p, n);
    };

    uint32_t magic = k_mh_magic_64;
    uint32_t filetype = k_mh_object;
    uint32_t ncmds = 3;
    uint32_t flags = 0;
    uint32_t reserved = 0;
    wr(0, &magic, 4);
    wr(4, &cputype, 4);
    wr(8, &cpusubtype, 4);
    wr(12, &filetype, 4);
    wr(16, &ncmds, 4);
    wr(20, &sizeofcmds, 4);
    wr(24, &flags, 4);
    wr(28, &reserved, 4);

    uint32_t lc = header_size;
    uint32_t cmd = k_lc_segment_64;
    uint32_t cmdsize = seg_cmd_size;
    wr(lc, &cmd, 4);
    wr(lc + 4, &cmdsize, 4);
    uint64_t vmaddr = 0;
    uint64_t vmsize = img.text.size() + img.rodata.size();
    uint64_t fileoff = text_off;
    uint64_t filesize = vmsize;
    uint32_t maxprot = 7;
    uint32_t initprot = 7;
    uint32_t nsects_v = nsects;
    uint32_t segflags = 0;
    wr(lc + 24, &vmaddr, 8);
    wr(lc + 32, &vmsize, 8);
    wr(lc + 40, &fileoff, 8);
    wr(lc + 48, &filesize, 8);
    wr(lc + 56, &maxprot, 4);
    wr(lc + 60, &initprot, 4);
    wr(lc + 64, &nsects_v, 4);
    wr(lc + 68, &segflags, 4);

    section_64 text_sec{};
    std::strncpy(text_sec.sectname, "__text", 16);
    std::strncpy(text_sec.segname, "__TEXT", 16);
    text_sec.size = img.text.size();
    text_sec.offset = text_off;
    text_sec.align = 2;
    text_sec.reloff = reloc_bytes.empty() ? 0 : reloc_off;
    text_sec.nreloc = static_cast<uint32_t>(reloc_bytes.size() / 8);
    text_sec.flags = k_s_attr_pure_instructions | k_s_attr_some_instructions;
    wr(lc + 72, &text_sec, sizeof text_sec);

    section_64 const_sec{};
    std::strncpy(const_sec.sectname, "__const", 16);
    std::strncpy(const_sec.segname, "__DATA", 16);
    const_sec.addr = img.text.size();
    const_sec.size = img.rodata.size();
    const_sec.offset = rodata_off;
    wr(lc + 72 + sizeof(section_64), &const_sec, sizeof const_sec);

    lc += seg_cmd_size;
    cmd = k_lc_symtab;
    cmdsize = symtab_cmd_size;
    uint32_t nsyms = static_cast<uint32_t>(ordered.size());
    uint32_t strsize = static_cast<uint32_t>(strtab.size());
    wr(lc, &cmd, 4);
    wr(lc + 4, &cmdsize, 4);
    wr(lc + 8, &symoff, 4);
    wr(lc + 12, &nsyms, 4);
    wr(lc + 16, &stroff, 4);
    wr(lc + 20, &strsize, 4);

    lc += symtab_cmd_size;
    cmd = k_lc_dysymtab;
    cmdsize = dysym_cmd_size;
    wr(lc, &cmd, 4);
    wr(lc + 4, &cmdsize, 4);
    uint32_t ilocalsym = 0;
    uint32_t nlocalsym = iextdefsym;
    wr(lc + 8, &ilocalsym, 4);
    wr(lc + 12, &nlocalsym, 4);
    wr(lc + 16, &iextdefsym, 4);
    wr(lc + 20, &nextdef, 4);
    wr(lc + 24, &iundefsym, 4);
    wr(lc + 28, &nundef, 4);

    if (!img.text.empty())
    {
        wr(text_off, img.text.data(), img.text.size());
    }
    if (!img.rodata.empty())
    {
        wr(rodata_off, img.rodata.data(), img.rodata.size());
    }
    if (!reloc_bytes.empty())
    {
        wr(reloc_off, reloc_bytes.data(), reloc_bytes.size());
    }
    for (size_t i = 0; i < ordered.size(); ++i)
    {
        wr(symoff + static_cast<uint32_t>(i * sizeof(nlist_64)), &ordered[i],
           sizeof(nlist_64));
    }
    wr(stroff, strtab.data(), strtab.size());
    return out;
}

} // namespace munx::native::asm_backend
