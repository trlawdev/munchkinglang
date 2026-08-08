#pragma once

#include "../ast.hpp"
#include "../errors.hpp"
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

namespace munx::native
{

enum class backend_kind
{
    custom,
    llvm,
};

inline bool backend_available(backend_kind backend)
{
    switch (backend)
    {
    case backend_kind::custom:
        return MUNX_NATIVE_CUSTOM != 0;
    case backend_kind::llvm:
        return MUNX_NATIVE_LLVM != 0;
    }
    return false;
}

inline void compile_to_executable(const std::filesystem::path &source_dir,
                                  const ast::program &program,
                                  const std::filesystem::path &output,
                                  backend_kind backend)
{
    if (!backend_available(backend))
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

    if (backend == backend_kind::custom)
    {
        mod = mir::optimize_mir(std::move(mod));
        const std::string c = custom::emit_c(mod);
        link_generated_c(c, output);
        return;
    }

    // Light shared pre-opt before LLVM.
    mod = mir::optimize_mir(std::move(mod));
    llvm_backend::emit_and_link(mod, output);
}

} // namespace munx::native
