#pragma once

#include "../ast.hpp"
#include "../errors.hpp"
#include "asm/asm_backend.hpp"
#include "custom/c_emitter.hpp"
#include "llvm/llvm_emitter.hpp"
#include "mir_builder.hpp"
#include "mir_optimizer.hpp"
#include "toolchain.hpp"

#include <filesystem>
#include <string>

#ifndef MUNX_NATIVE_CUSTOM
#define MUNX_NATIVE_CUSTOM 1
#endif

#ifndef MUNX_NATIVE_LLVM
#define MUNX_NATIVE_LLVM 0
#endif

#ifndef MUNX_NATIVE_ASM
#define MUNX_NATIVE_ASM 1
#endif

namespace munx::native
{

enum class backend_kind
{
    custom,
    llvm,
    asm_,
};

inline bool backend_available(backend_kind backend)
{
    switch (backend)
    {
    case backend_kind::custom:
        return MUNX_NATIVE_CUSTOM != 0;
    case backend_kind::llvm:
        return MUNX_NATIVE_LLVM != 0;
    case backend_kind::asm_:
        return MUNX_NATIVE_ASM != 0;
    }
    return false;
}

struct compile_options
{
    backend_kind backend{backend_kind::custom};
    asm_backend::cpu_arch asm_arch{asm_backend::host_arch()};
    asm_backend::object_format asm_format{asm_backend::host_object_format()};
};

inline void compile_to_executable(const std::filesystem::path &source_dir,
                                  const ast::program &program,
                                  const std::filesystem::path &output,
                                  compile_options opts);

inline void compile_to_executable(const std::filesystem::path &source_dir,
                                  const ast::program &program,
                                  const std::filesystem::path &output,
                                  backend_kind backend)
{
    compile_options opts;
    opts.backend = backend;
    compile_to_executable(source_dir, program, output, opts);
}

inline void compile_to_executable(const std::filesystem::path &source_dir,
                                  const ast::program &program,
                                  const std::filesystem::path &output,
                                  compile_options opts)
{
    if (!backend_available(opts.backend))
    {
        fail_compile("native: requested backend is not available in this munxc build");
        return;
    }

    mir_builder builder;
    mir::module mod = builder.build(source_dir, program);
    if (active_compile_context != nullptr && active_compile_context->failed())
    {
        return;
    }

    mod = mir::optimize_mir(std::move(mod));

    if (opts.backend == backend_kind::custom)
    {
        const std::string c = custom::emit_c(mod);
        link_generated_c(c, output);
        return;
    }
    if (opts.backend == backend_kind::asm_)
    {
        asm_backend::compile_options aopts;
        aopts.arch = opts.asm_arch;
        aopts.format = opts.asm_format;
        asm_backend::emit_and_link(mod, output, aopts);
        return;
    }

    llvm_backend::emit_and_link(mod, output);
}

} // namespace munx::native
