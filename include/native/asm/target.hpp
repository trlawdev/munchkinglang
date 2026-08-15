#pragma once

#include <cstdint>
#include <string>

namespace munx::native::asm_backend
{

enum class cpu_arch
{
    x86_64,
    aarch64,
};

enum class object_format
{
    elf64,
    macho64,
    coff64, // PE/COFF relocatable object
};

inline cpu_arch host_arch()
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return cpu_arch::aarch64;
#else
    return cpu_arch::x86_64;
#endif
}

inline object_format host_object_format()
{
#if defined(_WIN32)
    return object_format::coff64;
#elif defined(__APPLE__)
    return object_format::macho64;
#else
    return object_format::elf64;
#endif
}

inline const char *arch_name(cpu_arch arch)
{
    switch (arch)
    {
    case cpu_arch::x86_64:
        return "x86_64";
    case cpu_arch::aarch64:
        return "aarch64";
    }
    return "unknown";
}

inline const char *format_name(object_format fmt)
{
    switch (fmt)
    {
    case object_format::elf64:
        return "elf64";
    case object_format::macho64:
        return "macho64";
    case object_format::coff64:
        return "coff64";
    }
    return "unknown";
}

inline bool parse_arch(const std::string &s, cpu_arch &out)
{
    if (s == "x86_64" || s == "x64" || s == "amd64")
    {
        out = cpu_arch::x86_64;
        return true;
    }
    if (s == "aarch64" || s == "arm64")
    {
        out = cpu_arch::aarch64;
        return true;
    }
    return false;
}

inline bool parse_format(const std::string &s, object_format &out)
{
    if (s == "elf" || s == "elf64")
    {
        out = object_format::elf64;
        return true;
    }
    if (s == "macho" || s == "macho64" || s == "mach-o")
    {
        out = object_format::macho64;
        return true;
    }
    if (s == "pe" || s == "coff" || s == "coff64")
    {
        out = object_format::coff64;
        return true;
    }
    return false;
}

} // namespace munx::native::asm_backend
