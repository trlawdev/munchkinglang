#pragma once

#include "../Opcode.hpp"
#include "../ast.hpp"
#include "../errors.hpp"
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace munx::vm::jit
{

/// Decoded CAST operand (mirrors @ref munx::vm::type_spec without pulling in vm.hpp).
struct decoded_type
{
    ast::type_kind kind{ast::type_kind::Primitive};
    ast::primitive_kind primitive{ast::primitive_kind::Void};
    std::string name;
    std::vector<decoded_type> elements;
};

/// Static bytecode cursor for optimizers and the JIT (no live frame required).
struct bytecode_cursor
{
    std::span<const std::byte> code;
    std::string_view strings;
    size_t pc{0};

    [[nodiscard]] bool at_end() const { return pc >= code.size(); }

    [[nodiscard]] size_t remaining() const { return code.size() - pc; }

    uint8_t peek_u8() const
    {
        if (pc >= code.size())
        {
            fail_compile("bytecode ended unexpectedly");
        }
        return std::to_integer<uint8_t>(code[pc]);
    }

    uint8_t read_u8()
    {
        if (pc >= code.size())
        {
            fail_compile("bytecode ended mid-instruction");
        }
        return std::to_integer<uint8_t>(code[pc++]);
    }

    template <typename T>
    T read_scalar()
    {
        if (pc + sizeof(T) > code.size())
        {
            fail_compile("bytecode ended mid-operand");
        }
        T item{};
        std::memcpy(&item, code.data() + pc, sizeof(T));
        pc += sizeof(T);
        return item;
    }

    std::string read_name()
    {
        const auto offset = read_scalar<uint32_t>();
        const auto length = read_scalar<uint32_t>();
        if (offset > strings.size() || length > strings.size() - offset)
        {
            fail_compile("string operand is outside the string table");
        }
        return std::string{strings.substr(offset, length)};
    }

    decoded_type read_type()
    {
        decoded_type target;
        target.kind = static_cast<ast::type_kind>(read_u8());
        switch (target.kind)
        {
        case ast::type_kind::Primitive:
            target.primitive = static_cast<ast::primitive_kind>(read_u8());
            break;
        case ast::type_kind::Named:
            target.name = read_name();
            break;
        case ast::type_kind::Array:
            target.elements.push_back(read_type());
            break;
        case ast::type_kind::Tuple:
        {
            const auto count = read_scalar<uint32_t>();
            for (uint32_t index = 0; index < count; ++index)
            {
                target.elements.push_back(read_type());
            }
            break;
        }
        case ast::type_kind::Map:
            target.elements.push_back(read_type());
            target.elements.push_back(read_type());
            break;
        case ast::type_kind::Lambda:
        {
            const auto count = read_scalar<uint32_t>();
            for (uint32_t index = 0; index < count; ++index)
            {
                target.elements.push_back(read_type());
            }
            target.elements.push_back(read_type());
            break;
        }
        }
        return target;
    }

    /// Advance past one full instruction; returns the opcode and start offset.
    Opcode skip_instruction()
    {
        const size_t start = pc;
        const auto opcode = static_cast<Opcode>(read_u8());
        switch (opcode)
        {
        case Opcode::PUSH_INT:
        case Opcode::PUSH_FLOAT:
            pc = start + 1 + (opcode == Opcode::PUSH_INT ? 8 : 8);
            break;
        case Opcode::PUSH_STRING:
        case Opcode::PUSH_REGEX:
            read_name();
            break;
        case Opcode::PUSH_CHAR:
        case Opcode::PUSH_BOOL:
            read_u8();
            break;
        case Opcode::PUSH_NULL:
        case Opcode::POP:
        case Opcode::DUP:
        case Opcode::SWAP:
        case Opcode::NEG:
        case Opcode::NOT:
        case Opcode::BITWISE_NOT:
        case Opcode::RET:
        case Opcode::HALT:
        case Opcode::INDEX_GET:
        case Opcode::MONITOR_EXIT:
        case Opcode::CLONE_OBJECT:
        case Opcode::MAKE_SIMD:
        case Opcode::SIMD_TO_ARRAY:
            break;
        case Opcode::PUSH_ENUM:
            read_name();
            read_name();
            break;
        case Opcode::PUSH_FUNC:
        case Opcode::LOAD:
        case Opcode::STORE:
        case Opcode::MEMBER_GET:
        case Opcode::FREE:
        case Opcode::PIPE_INSERT:
        case Opcode::PIPE_EXTRACT:
        case Opcode::CHANNEL_INSERT:
        case Opcode::CHANNEL_EXTRACT:
        case Opcode::LOCK_CREATE:
        case Opcode::LOCK_ACQUIRE:
        case Opcode::LOCK_RELEASE:
            read_name();
            break;
        case Opcode::ADD:
        case Opcode::SUB:
        case Opcode::MUL:
        case Opcode::DIV:
        case Opcode::MOD:
        case Opcode::EQ:
        case Opcode::NE:
        case Opcode::LT:
        case Opcode::GT:
        case Opcode::LE:
        case Opcode::GE:
        case Opcode::BITWISE_AND:
        case Opcode::BITWISE_OR:
        case Opcode::BITWISE_XOR:
            break;
        case Opcode::JMP:
        case Opcode::JMP_IF_FALSE:
        case Opcode::JMP_IF_TRUE:
        case Opcode::MONITOR_ENTER:
            read_scalar<uint32_t>();
            break;
        case Opcode::HINT_BRANCH:
            read_u8();
            break;
        case Opcode::CALL:
            read_u8();
            break;
        case Opcode::MAKE_ARRAY:
        case Opcode::MAKE_TUPLE:
        case Opcode::MAKE_MAP:
        case Opcode::ALLOC:
            read_scalar<uint32_t>();
            break;
        case Opcode::UNPACK:
            read_u8();
            break;
        case Opcode::CAST:
            read_type();
            break;
        case Opcode::DEFINE_ENUM:
            read_name();
            for (uint32_t count = read_scalar<uint32_t>(); count > 0; --count)
            {
                read_name();
            }
            break;
        case Opcode::DEFINE_OBJECT:
            read_name();
            for (uint32_t count = read_scalar<uint32_t>(); count > 0; --count)
            {
                read_name();
            }
            break;
        }
        return opcode;
    }

    static void append_u8(std::vector<std::byte> &out, uint8_t byte)
    {
        out.push_back(static_cast<std::byte>(byte));
    }

    template <typename T>
    static void append_scalar(std::vector<std::byte> &out, T value)
    {
        const auto *bytes = reinterpret_cast<const std::byte *>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(T));
    }

    static void append_name(std::vector<std::byte> &out, std::string_view strings,
                            std::string_view name)
    {
        const auto offset = static_cast<uint32_t>(name.data() - strings.data());
        const auto length = static_cast<uint32_t>(name.size());
        append_scalar(out, offset);
        append_scalar(out, length);
    }
};

/// When true, the interpreter is used instead of threaded JIT dispatch.
inline bool &jit_force_interpreter()
{
    static bool disabled = false;
    return disabled;
}

inline bool jit_enabled()
{
    if (jit_force_interpreter())
    {
        return false;
    }
    if (const char *configured = std::getenv("MUNX_VM_JIT"))
    {
        return configured[0] != '0' || configured[1] != '\0';
    }
    return true;
}

} // namespace munx::vm::jit
