#pragma once

#include "../bytecode_compiler.hpp"
#include "../errors.hpp"
#include "mx32.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace munx::isa
{

inline constexpr uint32_t k_mx32_magic = 0x3233584Du; // 'MX32' LE

/// Pack an in-memory mx32 module into a contiguous blob (for .mxb v9 code sections).
inline std::vector<std::byte> serialize_mx32(const mx_module &m)
{
    std::vector<std::byte> out;
    auto append = [&](const void *p, size_t n) {
        const auto *b = static_cast<const std::byte *>(p);
        out.insert(out.end(), b, b + n);
    };
    auto append_u32 = [&](uint32_t v) { append(&v, 4); };
    auto append_str = [&](const std::string &s) {
        append_u32(static_cast<uint32_t>(s.size()));
        if (!s.empty())
        {
            append(s.data(), s.size());
        }
    };

    append_u32(k_mx32_magic);
    append_u32(static_cast<uint32_t>(m.code.size()));
    if (!m.code.empty())
    {
        append(m.code.data(), m.code.size() * sizeof(uint32_t));
    }
    append_u32(m.entry_pc);
    append_u32(static_cast<uint32_t>(m.functions.size()));
    for (const auto &f : m.functions)
    {
        append_str(f.name);
        append_u32(f.entry);
        append_u32(f.param_count);
        append_u32(f.local_slots);
    }
    append_u32(static_cast<uint32_t>(m.pool.size()));
    for (const auto &e : m.pool)
    {
        append_u32(static_cast<uint32_t>(e.tag));
        switch (e.tag)
        {
        case pool_tag::i64:
            append(&e.i64, sizeof e.i64);
            break;
        case pool_tag::f64:
            append(&e.f64, sizeof e.f64);
            break;
        case pool_tag::boolean:
        {
            const uint8_t b = e.b ? 1 : 0;
            append(&b, 1);
            break;
        }
        case pool_tag::string:
            append_str(e.str);
            break;
        case pool_tag::null:
            break;
        }
    }
    return out;
}

inline mx_module deserialize_mx32(const std::byte *data, size_t len)
{
    mx_module m;
    size_t off = 0;
    auto need = [&](size_t n) {
        if (off + n > len)
        {
            fail_compile("mx32: truncated module blob");
        }
    };
    auto read_u32 = [&]() -> uint32_t {
        need(4);
        uint32_t v = 0;
        std::memcpy(&v, data + off, 4);
        off += 4;
        return v;
    };
    auto read_bytes = [&](size_t n) -> const std::byte * {
        need(n);
        const std::byte *p = data + off;
        off += n;
        return p;
    };
    auto read_str = [&]() -> std::string {
        const uint32_t n = read_u32();
        if (n == 0)
        {
            return {};
        }
        const auto *p = read_bytes(n);
        return std::string{reinterpret_cast<const char *>(p), n};
    };

    if (read_u32() != k_mx32_magic)
    {
        fail_compile("mx32: bad magic (expected MX32 blob)");
    }
    const uint32_t nwords = read_u32();
    m.code.resize(nwords);
    if (nwords > 0)
    {
        std::memcpy(m.code.data(), read_bytes(nwords * 4), nwords * 4);
    }
    m.entry_pc = read_u32();
    const uint32_t nfn = read_u32();
    m.functions.reserve(nfn);
    for (uint32_t i = 0; i < nfn; ++i)
    {
        mx_module::fn f;
        f.name = read_str();
        f.entry = read_u32();
        f.param_count = read_u32();
        f.local_slots = read_u32();
        m.functions.push_back(std::move(f));
    }
    const uint32_t np = read_u32();
    m.pool.reserve(np);
    for (uint32_t i = 0; i < np; ++i)
    {
        pool_entry e;
        e.tag = static_cast<pool_tag>(read_u32());
        switch (e.tag)
        {
        case pool_tag::i64:
            need(8);
            std::memcpy(&e.i64, read_bytes(8), 8);
            break;
        case pool_tag::f64:
            need(8);
            std::memcpy(&e.f64, read_bytes(8), 8);
            break;
        case pool_tag::boolean:
            e.b = *read_bytes(1) != std::byte{0};
            break;
        case pool_tag::string:
            e.str = read_str();
            break;
        case pool_tag::null:
            break;
        }
        m.pool.push_back(std::move(e));
    }
    return m;
}

/// Write a minimal .mxb v9 whose entry bytecode is a serialized mx32 module.
inline void write_mx32_mxb(const mx_module &mod, const std::string &package_name,
                           const std::filesystem::path &path)
{
    const std::vector<std::byte> blob = serialize_mx32(mod);

    // String table: package name only (function names live inside the blob).
    std::vector<std::byte> strings;
    const uint32_t pkg_off = 0;
    strings.insert(strings.end(),
                   reinterpret_cast<const std::byte *>(package_name.data()),
                   reinterpret_cast<const std::byte *>(package_name.data()) +
                       package_name.size());

    const size_t header_size = sizeof(mx_program_header);
    // layout: header | (no imports) | empty func table | init blob | strings
    const size_t func_array_off = header_size;
    const size_t init_off = func_array_off; // 0 functions
    const size_t str_off = init_off + blob.size();

    std::vector<std::byte> out(str_off + strings.size(), std::byte{0});

    mx_program_header header{};
    header.mx_signature[0] = std::byte{'M'};
    header.mx_signature[1] = std::byte{'X'};
    header.mx_bytecode_version = 9;
    header.package_name_offset = pkg_off;
    header.package_name_length = package_name.size();
    header.package_import_array_offset = header_size;
    header.num_package_import_list = 0;
    header.entry_point_bytecode_offset = init_off;
    header.entry_point_bytecode_length = blob.size();
    header.string_table_offset = str_off;
    header.string_table_length = strings.size();
    header.entry_point_package_descriptor.package_name_offset = pkg_off;
    header.entry_point_package_descriptor.package_name_length = package_name.size();
    header.entry_point_package_descriptor.package_bytecode_offset = init_off;
    header.entry_point_package_descriptor.package_bytecode_length = blob.size();
    header.entry_point_package_descriptor.function_descriptor_array_offset =
        func_array_off;
    header.entry_point_package_descriptor.num_function_descriptors = 0;
    header.entry_point_package_descriptor.init_debug_map_offset = 0;
    header.entry_point_package_descriptor.init_debug_map_length = 0;

    std::memcpy(out.data(), &header, sizeof header);
    if (!blob.empty())
    {
        std::memcpy(out.data() + init_off, blob.data(), blob.size());
    }
    if (!strings.empty())
    {
        std::memcpy(out.data() + str_off, strings.data(), strings.size());
    }

    std::ofstream file{path, std::ios::binary};
    if (!file)
    {
        fail_compile("could not write " + path.string());
    }
    file.write(reinterpret_cast<const char *>(out.data()),
               static_cast<std::streamsize>(out.size()));
}

inline bool is_mx32_blob(const std::byte *data, size_t len)
{
    if (len < 4)
    {
        return false;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, data, 4);
    return magic == k_mx32_magic;
}

} // namespace munx::isa
