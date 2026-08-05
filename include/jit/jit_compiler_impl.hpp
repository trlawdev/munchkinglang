#pragma once

#include "jit_compiler.hpp"

namespace munx::vm::jit
{

inline std::shared_ptr<compiled_unit>
compile_unit(virtual_machine &vm, std::span<const std::byte> source,
             std::string_view strings)
{
    auto unit = std::make_shared<compiled_unit>();
    unit->code = optimize_bytecode(source, strings);

    bytecode_cursor cursor{unit->code, strings};
    while (!cursor.at_end())
    {
        const size_t insn_pc = cursor.pc;
        const auto opcode = static_cast<Opcode>(cursor.read_u8());

        auto bind = [&](jit_step step) {
            unit->handler_at_pc.emplace(insn_pc, unit->handlers.size());
            unit->handlers.push_back(std::move(step));
        };

        switch (opcode)
        {
        case Opcode::PUSH_INT:
        {
            const int64_t literal = cursor.read_scalar<int64_t>();
            bind([literal](virtual_machine &, frame &current, size_t) {
                current.pc += 1 + sizeof(int64_t);
                current.stack.push_back(value{literal});
            });
            break;
        }
        case Opcode::PUSH_FLOAT:
        {
            const double literal = cursor.read_scalar<double>();
            bind([literal](virtual_machine &, frame &current, size_t) {
                current.pc += 1 + sizeof(double);
                current.stack.push_back(value{literal});
            });
            break;
        }
        case Opcode::PUSH_STRING:
        {
            const std::string literal = cursor.read_name();
            bind([literal = std::move(literal)](virtual_machine &, frame &current, size_t) {
                current.pc += 1 + 8;
                current.stack.push_back(value{make_string(literal)});
            });
            break;
        }
        case Opcode::PUSH_CHAR:
        {
            const char literal = static_cast<char>(cursor.read_u8());
            bind([literal](virtual_machine &, frame &current, size_t) {
                current.pc += 2;
                current.stack.push_back(value{literal});
            });
            break;
        }
        case Opcode::PUSH_BOOL:
        {
            const bool literal = cursor.read_u8() != 0;
            bind([literal](virtual_machine &, frame &current, size_t) {
                current.pc += 2;
                current.stack.push_back(value{literal});
            });
            break;
        }
        case Opcode::PUSH_NULL:
            bind([](virtual_machine &, frame &current, size_t) {
                current.pc += 1;
                current.stack.push_back(value{});
            });
            break;
        case Opcode::PUSH_REGEX:
        {
            const std::string pattern = cursor.read_name();
            bind([pattern](virtual_machine &, frame &current, size_t) {
                current.pc += 1 + 8;
                current.stack.push_back(value{regex_value{pattern}});
            });
            break;
        }
        case Opcode::PUSH_ENUM:
        {
            const std::string type = cursor.read_name();
            const std::string member = cursor.read_name();
            bind([type, member](virtual_machine &, frame &current, size_t) {
                current.pc += 1 + 16;
                current.stack.push_back(value{enum_value{type, member}});
            });
            break;
        }
        case Opcode::PUSH_FUNC:
        {
            const std::string name = cursor.read_name();
            bind([name](virtual_machine &machine, frame &current, size_t) {
                current.pc += 1 + 8;
                current.stack.push_back(machine.jit_push_function(name));
            });
            break;
        }
        case Opcode::POP:
            bind([](virtual_machine &, frame &current, size_t) {
                current.pc += 1;
                (void)virtual_machine::jit_pop(current);
            });
            break;
        case Opcode::DUP:
            bind([](virtual_machine &, frame &current, size_t) {
                current.pc += 1;
                virtual_machine::jit_dup(current);
            });
            break;
        case Opcode::SWAP:
            bind([](virtual_machine &, frame &current, size_t) {
                current.pc += 1;
                virtual_machine::jit_swap(current);
            });
            break;
        case Opcode::LOAD:
        {
            const std::string name = cursor.read_name();
            bind([name](virtual_machine &machine, frame &current, size_t) {
                current.pc += 1 + 8;
                current.stack.push_back(machine.jit_load_name(current, name));
            });
            break;
        }
        case Opcode::STORE:
        {
            const std::string name = cursor.read_name();
            bind([name](virtual_machine &machine, frame &current, size_t) {
                current.pc += 1 + 8;
                machine.jit_store_name(current, name,
                                       virtual_machine::jit_pop(current));
            });
            break;
        }
        case Opcode::ADD:
        case Opcode::SUB:
        case Opcode::MUL:
        case Opcode::DIV:
        case Opcode::MOD:
            bind([opcode](virtual_machine &, frame &current, size_t) {
                current.pc += 1;
                const value right = virtual_machine::jit_pop(current);
                const value left = virtual_machine::jit_pop(current);
                current.stack.push_back(
                    virtual_machine::jit_binary_arithmetic(opcode, left, right));
            });
            break;
        case Opcode::NEG:
            bind([](virtual_machine &, frame &current, size_t) {
                current.pc += 1;
                current.stack.push_back(
                    virtual_machine::jit_neg(virtual_machine::jit_pop(current)));
            });
            break;
        case Opcode::EQ:
        case Opcode::NE:
            bind([opcode](virtual_machine &, frame &current, size_t) {
                current.pc += 1;
                const value right = virtual_machine::jit_pop(current);
                const value left = virtual_machine::jit_pop(current);
                const bool equal = values_equal(left, right);
                current.stack.push_back(value{opcode == Opcode::EQ ? equal : !equal});
            });
            break;
        case Opcode::LT:
        case Opcode::GT:
        case Opcode::LE:
        case Opcode::GE:
            bind([opcode](virtual_machine &, frame &current, size_t) {
                current.pc += 1;
                const value right = virtual_machine::jit_pop(current);
                const value left = virtual_machine::jit_pop(current);
                current.stack.push_back(
                    virtual_machine::jit_compare(opcode, left, right));
            });
            break;
        case Opcode::NOT:
            bind([](virtual_machine &, frame &current, size_t) {
                current.pc += 1;
                current.stack.push_back(
                    value{!is_truthy(virtual_machine::jit_pop(current))});
            });
            break;
        case Opcode::BITWISE_AND:
        case Opcode::BITWISE_OR:
        case Opcode::BITWISE_XOR:
            bind([opcode](virtual_machine &, frame &current, size_t) {
                current.pc += 1;
                const value right = virtual_machine::jit_pop(current);
                const value left = virtual_machine::jit_pop(current);
                current.stack.push_back(
                    virtual_machine::jit_bitwise(opcode, left, right));
            });
            break;
        case Opcode::BITWISE_NOT:
            bind([](virtual_machine &, frame &current, size_t) {
                current.pc += 1;
                current.stack.push_back(virtual_machine::jit_bitwise_not(
                    virtual_machine::jit_pop(current)));
            });
            break;
        case Opcode::JMP:
        {
            const uint32_t target = cursor.read_scalar<uint32_t>();
            bind([target](virtual_machine &machine, frame &current, size_t) {
                current.pc = machine.jit_jump_target(current, target);
            });
            break;
        }
        case Opcode::JMP_IF_FALSE:
        {
            const uint32_t target = cursor.read_scalar<uint32_t>();
            bind([target](virtual_machine &machine, frame &current, size_t) {
                current.pc += 1 + sizeof(uint32_t);
                if (!is_truthy(virtual_machine::jit_pop(current)))
                {
                    current.pc = machine.jit_jump_target(current, target);
                }
            });
            break;
        }
        case Opcode::JMP_IF_TRUE:
        {
            const uint32_t target = cursor.read_scalar<uint32_t>();
            bind([target](virtual_machine &machine, frame &current, size_t) {
                current.pc += 1 + sizeof(uint32_t);
                if (is_truthy(virtual_machine::jit_pop(current)))
                {
                    current.pc = machine.jit_jump_target(current, target);
                }
            });
            break;
        }
        case Opcode::CALL:
        {
            const uint8_t argument_count = cursor.read_u8();
            bind([argument_count](virtual_machine &machine, frame &current,
                                  size_t) {
                current.pc += 2;
                value_vector arguments(argument_count);
                for (size_t index = argument_count; index > 0; --index)
                {
                    arguments[index - 1] = virtual_machine::jit_pop(current);
                }
                const value callee = virtual_machine::jit_pop(current);
                current.stack.push_back(machine.call_value(callee, arguments));
            });
            break;
        }
        case Opcode::RET:
            bind([](virtual_machine &, frame &current, size_t) {
                current.pc += 1;
                current.jit_return = virtual_machine::jit_pop(current);
                current.pc = current.code.size();
            });
            break;
        case Opcode::HALT:
            bind([](virtual_machine &, frame &current, size_t) {
                current.pc += 1;
                current.jit_return = value{};
                current.pc = current.code.size();
            });
            break;
        case Opcode::MAKE_ARRAY:
        case Opcode::MAKE_TUPLE:
        {
            const uint32_t count = cursor.read_scalar<uint32_t>();
            bind([opcode, count](virtual_machine &, frame &current, size_t) {
                current.pc += 1 + sizeof(uint32_t);
                auto items = std::make_shared<sequence_object>();
                items->items.resize(count);
                for (size_t index = count; index > 0; --index)
                {
                    items->items[index - 1] = virtual_machine::jit_pop(current);
                }
                if (opcode == Opcode::MAKE_ARRAY)
                {
                    current.stack.push_back(value{array_value{items}});
                }
                else
                {
                    current.stack.push_back(value{tuple_value{items}});
                }
            });
            break;
        }
        case Opcode::MAKE_MAP:
        {
            const uint32_t count = cursor.read_scalar<uint32_t>();
            bind([count](virtual_machine &, frame &current, size_t) {
                current.pc += 1 + sizeof(uint32_t);
                auto map = std::make_shared<map_object>();
                for (size_t index = count; index > 0; --index)
                {
                    const value item = virtual_machine::jit_pop(current);
                    const value key = virtual_machine::jit_pop(current);
                    map_store_entry(*map, key, item);
                }
                current.stack.push_back(value{map_value{std::move(map)}});
            });
            break;
        }
        case Opcode::INDEX_GET:
            bind([](virtual_machine &machine, frame &current, size_t) {
                current.pc += 1;
                const value index = virtual_machine::jit_pop(current);
                const value container = virtual_machine::jit_pop(current);
                current.stack.push_back(
                    virtual_machine::jit_index_get(container, index));
            });
            break;
        case Opcode::MEMBER_GET:
        {
            const std::string member = cursor.read_name();
            bind([member](virtual_machine &machine, frame &current, size_t) {
                current.pc += 1 + 8;
                const value container = virtual_machine::jit_pop(current);
                current.stack.push_back(
                    virtual_machine::jit_member_get(container, member));
            });
            break;
        }
        case Opcode::UNPACK:
        {
            const uint8_t count = cursor.read_u8();
            bind([count](virtual_machine &, frame &current, size_t) {
                current.pc += 2;
                const value aggregate = virtual_machine::jit_pop(current);
                const value_vector &items =
                    virtual_machine::jit_unpack_elements(aggregate);
                if (items.size() < count)
                {
                    throw_error("cannot destructure " +
                                std::to_string(items.size()) + " value(s) into " +
                                std::to_string(count) + " target(s)");
                }
                for (size_t index = count; index > 0; --index)
                {
                    current.stack.push_back(items[index - 1]);
                }
            });
            break;
        }
        case Opcode::CAST:
        {
            const decoded_type target = cursor.read_type();
            const size_t end_pc = cursor.pc;
            bind([target, end_pc](virtual_machine &machine, frame &current,
                                  size_t) {
                current.pc = end_pc;
                const value operand = virtual_machine::jit_pop(current);
                current.stack.push_back(
                    machine.jit_cast_value(operand, target));
            });
            break;
        }
        case Opcode::ALLOC:
        {
            const uint32_t count = cursor.read_scalar<uint32_t>();
            bind([count](virtual_machine &, frame &current, size_t) {
                current.pc += 1 + sizeof(uint32_t);
                auto buffer = std::make_shared<buffer_object>();
                buffer->items.resize(count);
                for (size_t index = count; index > 0; --index)
                {
                    buffer->items[index - 1] = virtual_machine::jit_pop(current);
                }
                buffer->capacity = as_integer(virtual_machine::jit_pop(current));
                if (buffer->capacity < 0)
                {
                    throw_error("alloc capacity must not be negative");
                }
                current.stack.push_back(value{buffer});
            });
            break;
        }
        case Opcode::FREE:
        {
            const std::string name = cursor.read_name();
            bind([name](virtual_machine &machine, frame &current, size_t) {
                current.pc += 1 + 8;
                machine.jit_free_buffer(current, name);
                current.stack.push_back(value{});
            });
            break;
        }
        case Opcode::CLONE_OBJECT:
            bind([](virtual_machine &, frame &current, size_t) {
                current.pc += 1;
                current.stack.push_back(
                    virtual_machine::jit_clone_for_return(virtual_machine::jit_pop(current)));
            });
            break;
        case Opcode::MAKE_SIMD:
            bind([](virtual_machine &, frame &current, size_t) {
                current.pc += 1;
                const value array_value_item = virtual_machine::jit_pop(current);
                const auto *array = array_value_item.get_if<array_value>();
                if (array == nullptr)
                {
                    throw_error("MAKE_SIMD expects an array operand");
                }
                current.stack.push_back(value{make_simd_from_array(*array)});
            });
            break;
        case Opcode::SIMD_TO_ARRAY:
            bind([](virtual_machine &, frame &current, size_t) {
                current.pc += 1;
                const value simd_item = virtual_machine::jit_pop(current);
                const auto *simd = simd_item.get_if<simd_value>();
                if (simd == nullptr)
                {
                    throw_error("SIMD_TO_ARRAY expects a SIMD operand");
                }
                current.stack.push_back(value{simd_to_array(*simd)});
            });
            break;
        case Opcode::PIPE_INSERT:
        {
            const std::string name = cursor.read_name();
            bind([name](virtual_machine &machine, frame &current, size_t) {
                current.pc += 1 + 8;
                const value item = virtual_machine::jit_pop(current);
                machine.jit_pipe_insert(current, name, item);
                current.stack.push_back(value{});
            });
            break;
        }
        case Opcode::PIPE_EXTRACT:
        {
            const std::string name = cursor.read_name();
            bind([name](virtual_machine &machine, frame &current, size_t) {
                current.pc += 1 + 8;
                current.stack.push_back(machine.jit_pipe_extract(current, name));
            });
            break;
        }
        case Opcode::DEFINE_ENUM:
        {
            const std::string name = cursor.read_name();
            const uint32_t count = cursor.read_scalar<uint32_t>();
            std::vector<std::string> members;
            members.reserve(count);
            for (uint32_t index = 0; index < count; ++index)
            {
                members.push_back(cursor.read_name());
            }
            const size_t end_pc = cursor.pc;
            bind([name, members = std::move(members),
                  end_pc](virtual_machine &machine, frame &current, size_t) {
                current.pc = end_pc;
                machine.jit_define_enum(name, members);
            });
            break;
        }
        case Opcode::DEFINE_OBJECT:
        {
            const std::string name = cursor.read_name();
            const uint32_t count = cursor.read_scalar<uint32_t>();
            std::vector<std::string> fields;
            fields.reserve(count);
            for (uint32_t index = 0; index < count; ++index)
            {
                fields.push_back(cursor.read_name());
            }
            const size_t end_pc = cursor.pc;
            bind([name, fields = std::move(fields),
                  end_pc](virtual_machine &machine, frame &current, size_t) {
                current.pc = end_pc;
                machine.jit_define_object_type(current, name, fields);
            });
            break;
        }
        case Opcode::LOCK_CREATE:
        {
            const std::string name = cursor.read_name();
            bind([name](virtual_machine &machine, frame &current, size_t) {
                current.pc += 1 + 8;
                machine.jit_lock_create(current, name);
            });
            break;
        }
        case Opcode::LOCK_ACQUIRE:
        {
            const std::string name = cursor.read_name();
            bind([name](virtual_machine &machine, frame &current, size_t) {
                current.pc += 1 + 8;
                machine.jit_lock_acquire(current, name);
            });
            break;
        }
        case Opcode::LOCK_RELEASE:
        {
            const std::string name = cursor.read_name();
            bind([name](virtual_machine &machine, frame &current, size_t) {
                current.pc += 1 + 8;
                machine.jit_lock_release(current, name);
            });
            break;
        }
        case Opcode::MONITOR_ENTER:
        {
            const uint32_t handler = cursor.read_scalar<uint32_t>();
            bind([handler](virtual_machine &machine, frame &current, size_t) {
                current.pc += 1 + sizeof(uint32_t);
                current.monitors.push_back(monitor_state{
                    current.pc,
                    machine.jit_jump_target(current, handler),
                    current.stack.size()});
            });
            break;
        }
        case Opcode::MONITOR_EXIT:
            bind([](virtual_machine &, frame &current, size_t) {
                current.pc += 1;
                if (!current.monitors.empty())
                {
                    current.monitors.pop_back();
                }
            });
            break;
        default:
            throw compilation_error{"unsupported opcode in JIT compiler: " +
                                    std::to_string(static_cast<int>(opcode))};
        }
    }

    return unit;
}

} // namespace munx::vm::jit

namespace munx::vm
{

inline std::shared_ptr<jit::compiled_unit>
virtual_machine::get_jit_unit(std::span<const std::byte> source)
{
    const jit_cache_key key{source.data(), source.size()};
    {
        std::lock_guard<std::mutex> guard{jit_mutex_};
        const auto found = jit_cache_.find(key);
        if (found != jit_cache_.end())
        {
            return found->second;
        }
    }
    auto unit = jit::compile_unit(*this, source, image_.strings);
    std::lock_guard<std::mutex> guard{jit_mutex_};
    jit_cache_[key] = unit;
    return unit;
}

inline value virtual_machine::execute_jit(frame &current)
{
    current.jit_unit = get_jit_unit(current.code);
    current.code = current.jit_unit->code;
    current.jit_return.reset();

    const std::shared_ptr<jit::compiled_unit> &unit = current.jit_unit;
    while (current.pc < unit->code.size())
    {
        const size_t instruction_pc = current.pc;
        sync_call_stack_pc(instruction_pc);
        try
        {
            const auto found = unit->handler_at_pc.find(current.pc);
            if (found == unit->handler_at_pc.end())
            {
                throw_error("JIT missing handler at pc " +
                            std::to_string(current.pc) + " in " +
                            current.description);
            }
            unit->handlers[found->second](*this, current, instruction_pc);
            if (current.jit_return.has_value())
            {
                return std::move(*current.jit_return);
            }
        }
        catch (const runtime_exception &error)
        {
            while (!current.monitors.empty() &&
                   !current.monitors.back().covers(instruction_pc))
            {
                current.monitors.pop_back();
            }
            if (current.monitors.empty())
            {
                throw;
            }
            const monitor_state handler = current.monitors.back();
            current.monitors.pop_back();
            current.stack.resize(handler.stack_depth);
            current.stack.push_back(error.payload());
            current.pc = handler.handler_pc;
        }
    }
    return current.jit_return.value_or(value{});
}

} // namespace munx::vm
