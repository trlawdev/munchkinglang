#pragma once

#include "object_builder.hpp"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace munx::native::asm_backend
{

/// Minimal PE/COFF relocatable object (x86_64 / ARM64).
inline std::vector<uint8_t> write_coff64(const object_image &img)
{
    constexpr uint16_t IMAGE_FILE_MACHINE_AMD64 = 0x8664;
    constexpr uint16_t IMAGE_FILE_MACHINE_ARM64 = 0xAA64;
    constexpr uint16_t IMAGE_SCN_CNT_CODE = 0x00000020;
    constexpr uint16_t IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040;
    constexpr uint32_t IMAGE_SCN_MEM_EXECUTE = 0x20000000;
    constexpr uint32_t IMAGE_SCN_MEM_READ = 0x40000000;
    constexpr uint32_t IMAGE_SCN_ALIGN_16BYTES = 0x00500000;
    constexpr uint32_t IMAGE_SCN_ALIGN_1BYTES = 0x00100000;
    constexpr uint8_t IMAGE_SYM_CLASS_EXTERNAL = 2;
    constexpr uint8_t IMAGE_SYM_CLASS_STATIC = 3;
    constexpr uint16_t IMAGE_REL_AMD64_REL32 = 4;
    constexpr uint16_t IMAGE_REL_ARM64_BRANCH26 = 3;

    const uint16_t machine = img.arch == cpu_arch::aarch64
                                 ? IMAGE_FILE_MACHINE_ARM64
                                 : IMAGE_FILE_MACHINE_AMD64;

    std::string strtab;
    // COFF string table starts with 4-byte length
    auto intern = [&](const std::string &s) -> uint32_t {
        if (s.size() <= 8)
        {
            return 0; // stored inline
        }
        const uint32_t off = static_cast<uint32_t>(strtab.size() + 4);
        strtab += s;
        strtab.push_back('\0');
        return off;
    };

#pragma pack(push, 1)
    struct coff_reloc
    {
        uint32_t virtual_address;
        uint32_t symbol_table_index;
        uint16_t type;
    };
    struct coff_sym
    {
        char name[8];
        uint32_t value;
        int16_t section;
        uint16_t type;
        uint8_t storage;
        uint8_t naux;
    };
#pragma pack(pop)

    std::vector<coff_sym> syms;
    std::unordered_map<std::string, uint32_t> sym_index;

    auto add_sym = [&](const symbol &s) {
        coff_sym cs{};
        std::memset(cs.name, 0, 8);
        if (s.name.size() <= 8)
        {
            std::memcpy(cs.name, s.name.data(), s.name.size());
        }
        else
        {
            const uint32_t off = intern(s.name);
            std::memset(cs.name, 0, 4);
            std::memcpy(cs.name + 4, &off, 4);
        }
        cs.value = s.value;
        if (s.bind == symbol_bind::Undefined)
        {
            cs.section = 0;
            cs.storage = IMAGE_SYM_CLASS_EXTERNAL;
        }
        else
        {
            cs.section = static_cast<int16_t>(s.is_text ? 1 : 2);
            cs.storage = s.bind == symbol_bind::Local ? IMAGE_SYM_CLASS_STATIC
                                                      : IMAGE_SYM_CLASS_EXTERNAL;
        }
        cs.type = s.is_text ? 0x20 : 0; // IMAGE_SYM_DTYPE_FUNCTION << 4
        sym_index[s.name] = static_cast<uint32_t>(syms.size());
        syms.push_back(cs);
    };

    for (const auto &s : img.symbols)
    {
        add_sym(s);
    }

    std::vector<coff_reloc> relocs;
    for (const auto &r : img.text_relocs)
    {
        const auto it = sym_index.find(r.symbol);
        if (it == sym_index.end())
        {
            continue;
        }
        coff_reloc cr{};
        cr.virtual_address = r.offset;
        cr.symbol_table_index = it->second;
        cr.type = img.arch == cpu_arch::aarch64 ? IMAGE_REL_ARM64_BRANCH26
                                                : IMAGE_REL_AMD64_REL32;
        relocs.push_back(cr);
    }

    constexpr uint16_t nsections = 2;
    constexpr uint32_t header_size = 20;
    constexpr uint32_t sec_hdr_size = 40;
    const uint32_t sec_table = header_size;
    uint32_t cursor = header_size + nsections * sec_hdr_size;

    const uint32_t text_off = cursor;
    cursor += static_cast<uint32_t>(img.text.size());
    const uint32_t reloc_off = relocs.empty() ? 0 : cursor;
    cursor += static_cast<uint32_t>(relocs.size() * sizeof(coff_reloc));
    const uint32_t rodata_off = cursor;
    cursor += static_cast<uint32_t>(img.rodata.size());
    const uint32_t sym_off = cursor;
    cursor += static_cast<uint32_t>(syms.size() * sizeof(coff_sym));
    const uint32_t str_off = cursor;
    const uint32_t str_size = 4 + static_cast<uint32_t>(strtab.size());
    cursor += str_size;

    std::vector<uint8_t> out(cursor, 0);
    auto wr = [&](uint32_t at, const void *p, size_t n) {
        std::memcpy(out.data() + at, p, n);
    };

    // IMAGE_FILE_HEADER
    wr(0, &machine, 2);
    wr(2, &nsections, 2);
    uint32_t timedate = 0;
    wr(4, &timedate, 4);
    wr(8, &sym_off, 4);
    uint32_t nsyms = static_cast<uint32_t>(syms.size());
    wr(12, &nsyms, 4);
    uint16_t opt_hdr = 0;
    wr(16, &opt_hdr, 2);
    uint16_t characteristics = 0;
    wr(18, &characteristics, 2);

    auto write_sec = [&](uint32_t at, const char *name, uint32_t size,
                         uint32_t raw_off, uint32_t nreloc, uint32_t reloc_ptr,
                         uint32_t chars) {
        char nm[8]{};
        std::strncpy(nm, name, 8);
        wr(at, nm, 8);
        uint32_t vsize = 0;
        wr(at + 8, &vsize, 4);
        uint32_t vaddr = 0;
        wr(at + 12, &vaddr, 4);
        wr(at + 16, &size, 4);
        wr(at + 20, &raw_off, 4);
        wr(at + 24, &reloc_ptr, 4);
        uint32_t lines = 0;
        wr(at + 28, &lines, 4);
        uint16_t nr = static_cast<uint16_t>(nreloc);
        wr(at + 32, &nr, 2);
        uint16_t nl = 0;
        wr(at + 34, &nl, 2);
        wr(at + 36, &chars, 4);
    };

    write_sec(sec_table, ".text", static_cast<uint32_t>(img.text.size()), text_off,
              static_cast<uint32_t>(relocs.size()), reloc_off,
              IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ |
                  IMAGE_SCN_ALIGN_16BYTES);
    write_sec(sec_table + sec_hdr_size, ".rdata",
              static_cast<uint32_t>(img.rodata.size()), rodata_off, 0, 0,
              IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ |
                  IMAGE_SCN_ALIGN_1BYTES);

    if (!img.text.empty())
    {
        wr(text_off, img.text.data(), img.text.size());
    }
    for (size_t i = 0; i < relocs.size(); ++i)
    {
        wr(reloc_off + static_cast<uint32_t>(i * sizeof(coff_reloc)), &relocs[i],
           sizeof(coff_reloc));
    }
    if (!img.rodata.empty())
    {
        wr(rodata_off, img.rodata.data(), img.rodata.size());
    }
    for (size_t i = 0; i < syms.size(); ++i)
    {
        wr(sym_off + static_cast<uint32_t>(i * sizeof(coff_sym)), &syms[i],
           sizeof(coff_sym));
    }
    wr(str_off, &str_size, 4);
    if (!strtab.empty())
    {
        wr(str_off + 4, strtab.data(), strtab.size());
    }
    return out;
}

} // namespace munx::native::asm_backend
