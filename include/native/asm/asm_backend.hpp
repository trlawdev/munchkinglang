#pragma once

#include "../../errors.hpp"
#include "../mir.hpp"
#include "../toolchain.hpp"
#include "codegen.hpp"
#include "object_builder.hpp"
#include "target.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace munx::native::asm_backend
{

struct compile_options
{
    cpu_arch arch{host_arch()};
    object_format format{host_object_format()};
};

inline void emit_and_link(const mir::module &mod, const std::filesystem::path &output,
                          compile_options opts = {})
{
    object_image img = emit_object(mod, opts.arch, opts.format);
    if (active_compile_context != nullptr && active_compile_context->failed())
    {
        return;
    }
    const std::vector<uint8_t> bytes = write_object(img);
    if (bytes.empty())
    {
        fail_compile("asm: failed to serialize object image");
        return;
    }
    link_generated_object(bytes, output);
}

} // namespace munx::native::asm_backend
