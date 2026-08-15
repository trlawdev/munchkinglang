#pragma once

#include "../../errors.hpp"
#include "../mir.hpp"
#include "object_builder.hpp"
#include "target.hpp"
#include "x64.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace munx::native::asm_backend
{

namespace detail
{

inline bool is_void_callee(const std::string &name, mir::type ty)
{
    if (ty == mir::type::void_)
    {
        return true;
    }
    return name == "munx_print" || name == "munx_println" || name == "munx_fail" ||
           name == "munx_sleep" || name == "munx_pipe_insert" ||
           name == "munx_channel_insert" || name == "munx_pipe_close" ||
           name == "munx_set_argv" || name == "munx_pipe_session_begin" ||
           name == "munx_pipe_session_end";
}

inline const char *runtime_binop(mir::opcode op)
{
    switch (op)
    {
    case mir::opcode::add:
        return "munx_add";
    case mir::opcode::sub:
        return "munx_sub";
    case mir::opcode::mul:
        return "munx_mul";
    case mir::opcode::div:
        return "munx_div";
    case mir::opcode::mod:
        return "munx_mod";
    case mir::opcode::eq:
        return "munx_eq";
    case mir::opcode::ne:
        return "munx_ne";
    case mir::opcode::lt:
        return "munx_lt";
    case mir::opcode::gt:
        return "munx_gt";
    case mir::opcode::le:
        return "munx_le";
    case mir::opcode::ge:
        return "munx_ge";
    default:
        return nullptr;
    }
}

inline void load_munx_to_pair(x64::assembler &a, int32_t slot, x64::reg tag_r,
                              x64::reg as_r)
{
    a.load8z_rbp(tag_r, slot);
    a.load64_rbp(as_r, slot + 8);
}

inline void store_munx_from_ret(x64::assembler &a, int32_t slot)
{
    a.store8_al_rbp(slot);
    a.store64_rbp(x64::RDX, slot + 8);
}

inline void zero_munx_slot(x64::assembler &a, int32_t slot)
{
    a.store8i_rbp(slot, 0);
    a.store64i_rbp(slot + 8, 0);
}

inline void copy_munx_slot(x64::assembler &a, int32_t dst, int32_t src)
{
    a.load8z_rbp(x64::RAX, src);
    a.store8_al_rbp(dst);
    a.load64_rbp(x64::RDX, src + 8);
    a.store64_rbp(x64::RDX, dst + 8);
}

struct fn_layout
{
    int32_t frame_size{0};
    uint32_t n_vals{0};
    uint32_t n_locals{0};

    int32_t local_disp(uint32_t i) const
    {
        return -static_cast<int32_t>((i + 1) * 16);
    }
    int32_t v_disp(uint32_t i) const
    {
        return -static_cast<int32_t>((n_locals + i + 1) * 16);
    }
};

inline fn_layout make_layout(const mir::function &fn)
{
    fn_layout L;
    L.n_locals = fn.local_count;
    L.n_vals = static_cast<uint32_t>(fn.code.size());
    const uint32_t slots = L.n_locals + L.n_vals;
    L.frame_size = static_cast<int32_t>(((slots * 16) + 15) & ~15);
    if (L.frame_size < 32)
    {
        L.frame_size = 32;
    }
    return L;
}

inline void emit_x64_function(object_image &img, x64::assembler &a,
                              const mir::function &fn,
                              const std::unordered_map<std::string, bool> &local_fns)
{
    static const x64::reg tag_regs[3] = {x64::RDI, x64::RDX, x64::R8};
    static const x64::reg as_regs[3] = {x64::RSI, x64::RCX, x64::R9};

    const bool is_init = fn.name.rfind("munx_init_", 0) == 0;
    const fn_layout L = make_layout(fn);
    const uint32_t start = static_cast<uint32_t>(img.text.size());

    a.push(x64::RBP);
    a.mov_rr(x64::RBP, x64::RSP);
    a.sub_rsp(L.frame_size);

    for (uint32_t i = 0; i < L.n_locals; ++i)
    {
        zero_munx_slot(a, L.local_disp(i));
    }
    for (uint32_t i = 0; i < fn.param_count && i < 3; ++i)
    {
        a.mov_rr(x64::RAX, tag_regs[i]);
        a.store8_al_rbp(L.local_disp(i));
        a.store64_rbp(as_regs[i], L.local_disp(i) + 8);
    }
    if (fn.param_count > 3)
    {
        fail_compile("asm: functions with >3 MunxValue params not supported yet");
        return;
    }
    for (uint32_t i = 0; i < L.n_vals; ++i)
    {
        zero_munx_slot(a, L.v_disp(i));
    }

    std::unordered_map<uint32_t, uint32_t> label_pos;
    struct patch
    {
        uint32_t disp_off;
        uint32_t block;
    };
    std::vector<patch> patches;

    auto vslot = [&](uint32_t id) { return L.v_disp(id); };

    auto do_call = [&](const std::string &name, const std::vector<uint32_t> &args,
                       bool has_ret, uint32_t ret_slot) {
        if (args.size() > 3)
        {
            fail_compile("asm: too many call args for " + name);
            return;
        }
        for (size_t i = 0; i < args.size(); ++i)
        {
            load_munx_to_pair(a, vslot(args[i]), tag_regs[i], as_regs[i]);
        }
        if (local_fns.count(name))
        {
            a.call_local(name);
        }
        else
        {
            a.call_plt(name);
        }
        if (has_ret)
        {
            store_munx_from_ret(a, vslot(ret_slot));
        }
    };

    for (size_t ii = 0; ii < fn.code.size(); ++ii)
    {
        const mir::instr &in = fn.code[ii];
        const uint32_t dst = static_cast<uint32_t>(ii);
        switch (in.op)
        {
        case mir::opcode::label:
            label_pos[in.block_target] = static_cast<uint32_t>(img.text.size());
            break;
        case mir::opcode::const_i64:
            a.mov_ri64(x64::RDI, static_cast<uint64_t>(in.i64));
            a.call_plt("munx_i64");
            store_munx_from_ret(a, vslot(dst));
            break;
        case mir::opcode::const_f64:
        {
            uint64_t bits = 0;
            std::memcpy(&bits, &in.f64, sizeof bits);
            a.mov_ri64(x64::RAX, bits);
            // movq %rax, %xmm0
            a.emit_u8(0x66);
            a.emit_u8(0x48);
            a.emit_u8(0x0F);
            a.emit_u8(0x6E);
            a.emit_u8(0xC0);
            a.call_plt("munx_f64");
            store_munx_from_ret(a, vslot(dst));
            break;
        }
        case mir::opcode::const_bool:
            a.mov_ri32(x64::RDI, in.b ? 1 : 0);
            a.call_plt("munx_bool");
            store_munx_from_ret(a, vslot(dst));
            break;
        case mir::opcode::const_null:
            a.call_plt("munx_null");
            store_munx_from_ret(a, vslot(dst));
            break;
        case mir::opcode::const_str:
            a.lea_rip_symbol(x64::RDI, ".Lstr" + std::to_string(in.str_index));
            a.call_plt("munx_string");
            store_munx_from_ret(a, vslot(dst));
            break;
        case mir::opcode::load_local:
            copy_munx_slot(a, vslot(dst), L.local_disp(in.local));
            break;
        case mir::opcode::store_local:
            copy_munx_slot(a, L.local_disp(in.local), vslot(in.args[0]));
            break;
        case mir::opcode::neg:
            do_call("munx_neg", in.args, true, dst);
            break;
        case mir::opcode::not_:
            do_call("munx_not", in.args, true, dst);
            break;
        case mir::opcode::add:
        case mir::opcode::sub:
        case mir::opcode::mul:
        case mir::opcode::div:
        case mir::opcode::mod:
        case mir::opcode::eq:
        case mir::opcode::ne:
        case mir::opcode::lt:
        case mir::opcode::gt:
        case mir::opcode::le:
        case mir::opcode::ge:
            do_call(runtime_binop(in.op), in.args, true, dst);
            break;
        case mir::opcode::print:
            do_call("munx_print", in.args, false, 0);
            break;
        case mir::opcode::println:
            a.call_plt("munx_println");
            break;
        case mir::opcode::call:
            do_call(in.callee, in.args, !is_void_callee(in.callee, in.ty), dst);
            break;
        case mir::opcode::ret:
            if (!is_init && in.ty != mir::type::void_)
            {
                if (in.args.empty())
                {
                    a.call_plt("munx_null");
                }
                else
                {
                    a.load8z_rbp(x64::RAX, vslot(in.args[0]));
                    a.load64_rbp(x64::RDX, vslot(in.args[0]) + 8);
                }
            }
            a.mov_rr(x64::RSP, x64::RBP);
            a.pop(x64::RBP);
            a.ret();
            break;
        case mir::opcode::br:
            patches.push_back({a.jmp_rel32(), in.block_target});
            break;
        case mir::opcode::cbr:
            load_munx_to_pair(a, vslot(in.args[0]), x64::RDI, x64::RSI);
            a.call_plt("munx_truthy");
            a.test_al();
            // ZF=1 → false; jne → true arm
            patches.push_back({a.jne_rel32(), in.block_target});
            patches.push_back({a.jmp_rel32(), in.block_target_false});
            break;
        }
    }

    if (is_init)
    {
        a.mov_rr(x64::RSP, x64::RBP);
        a.pop(x64::RBP);
        a.ret();
    }
    else
    {
        a.call_plt("munx_null");
        a.mov_rr(x64::RSP, x64::RBP);
        a.pop(x64::RBP);
        a.ret();
    }

    for (const auto &p : patches)
    {
        const auto it = label_pos.find(p.block);
        if (it == label_pos.end())
        {
            fail_compile("asm: missing label L" + std::to_string(p.block) + " in " +
                         fn.name);
            return;
        }
        a.patch_rel32(p.disp_off, it->second);
    }

    const uint32_t end = static_cast<uint32_t>(img.text.size());
    img.define_text_symbol(fn.name, start, end - start, /*global=*/false);
}

inline void emit_x64_main(object_image &img, x64::assembler &a, const mir::module &mod)
{
    const uint32_t start = static_cast<uint32_t>(img.text.size());
    a.push(x64::RBP);
    a.mov_rr(x64::RBP, x64::RSP);
    a.sub_rsp(32);

    // Save argc / argv (SysV: edi, rsi)
    a.emit_u8(0x89);
    a.emit_u8(0x7D);
    a.emit_u8(0xFC); // movl %edi, -4(%rbp)
    a.store64_rbp(x64::RSI, -16);

    // if (argc <= 0) goto zero_path
    a.emit_u8(0x83);
    a.emit_u8(0x7D);
    a.emit_u8(0xFC);
    a.emit_u8(0x00); // cmpl $0, -4(%rbp)
    const uint32_t j_le = a.jcc_rel32(0x8E); // jle

    // rdi = argc - 1; rsi = argv + 8
    a.emit_u8(0x8B);
    a.emit_u8(0x45);
    a.emit_u8(0xFC); // movl -4(%rbp), %eax
    a.emit_u8(0xFF);
    a.emit_u8(0xC8); // decl %eax
    a.emit_u8(0x89);
    a.emit_u8(0xC7); // movl %eax, %edi
    a.load64_rbp(x64::RSI, -16);
    a.emit_u8(0x48);
    a.emit_u8(0x83);
    a.emit_u8(0xC6);
    a.emit_u8(0x08); // addq $8, %rsi
    const uint32_t j_join = a.jmp_rel32();

    const uint32_t zero_path = static_cast<uint32_t>(img.text.size());
    a.patch_rel32(j_le, zero_path);
    a.xor_eax();
    a.emit_u8(0x89);
    a.emit_u8(0xC7); // movl %eax, %edi  (0)
    a.load64_rbp(x64::RSI, -16);

    const uint32_t join = static_cast<uint32_t>(img.text.size());
    a.patch_rel32(j_join, join);

    a.call_plt("munx_set_argv");
    a.call_plt("munx_pipe_session_begin");

    // Call non-entry inits, then entry init
    for (const auto &fn : mod.functions)
    {
        if (fn.name.rfind("munx_init_", 0) == 0 && !fn.is_entry_init)
        {
            a.call_local(fn.name);
        }
    }
    for (const auto &fn : mod.functions)
    {
        if (fn.is_entry_init)
        {
            a.call_local(fn.name);
        }
    }

    a.xor_eax();
    a.mov_rr(x64::RSP, x64::RBP);
    a.pop(x64::RBP);
    a.ret();

    const uint32_t end = static_cast<uint32_t>(img.text.size());
    img.define_text_symbol("main", start, end - start, /*global=*/true);
}

} // namespace detail

inline object_image emit_object(const mir::module &mod, cpu_arch arch,
                                object_format fmt)
{
    object_image img;
    img.arch = arch;
    img.format = fmt;

    if (arch == cpu_arch::aarch64)
    {
        fail_compile(
            "asm: aarch64 machine encoder is not implemented yet (object writers "
            "are ready; use --arch x86_64 on this host)");
        return img;
    }

    // Strings in .rodata
    for (uint32_t i = 0; i < mod.strings.size(); ++i)
    {
        const uint32_t off = static_cast<uint32_t>(img.rodata.size());
        const std::string &s = mod.strings[i];
        img.rodata.insert(img.rodata.end(), s.begin(), s.end());
        img.rodata.push_back(0);
        img.symbols.push_back(
            symbol{".Lstr" + std::to_string(i), symbol_bind::Local, false, off,
                   static_cast<uint32_t>(s.size() + 1)});
    }

    std::unordered_map<std::string, bool> local_fns;
    for (const auto &fn : mod.functions)
    {
        local_fns[fn.name] = true;
    }

    x64::assembler a;
    a.img = &img;
    for (const auto &fn : mod.functions)
    {
        detail::emit_x64_function(img, a, fn, local_fns);
        if (active_compile_context != nullptr && active_compile_context->failed())
        {
            return img;
        }
    }
    detail::emit_x64_main(img, a, mod);
    return img;
}

} // namespace munx::native::asm_backend
