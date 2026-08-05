#pragma once

#include "branch_predictor.hpp"
#include "execution_profile.hpp"
#include "jit_compiler.hpp"
#include "pgo_optimizer.hpp"
#include "vm_pipeline.hpp"

namespace munx::vm::jit
{

inline void compile_handlers(compiled_unit &unit, virtual_machine &,
                             std::string_view strings);

inline std::shared_ptr<compiled_unit>
compile_unit(virtual_machine &vm, std::span<const std::byte> source,
             std::string_view strings, const execution_profile *profile)
{
    auto unit = std::make_shared<compiled_unit>();
    unit->code = optimize_bytecode_with_profile(source, strings, profile);
    if (profile != nullptr)
    {
        unit->profile_generation = profile->generation;
    }

    branch_predictor &predictor = branch_predictor::instance();
    seed_predictor_from_bytecode(source, strings, predictor);
    if (profile != nullptr)
    {
        predictor.seed_from_profile(*profile);
    }

    compile_handlers(*unit, vm, strings);
    return unit;
}

inline void compile_handlers(compiled_unit &unit, virtual_machine &,
                             std::string_view strings)
{
    bytecode_cursor cursor{unit.code, strings};
    while (!cursor.at_end())
    {
        const size_t insn_pc = cursor.pc;
        const auto opcode = static_cast<Opcode>(cursor.read_u8());

        auto bind = [&](jit_step step) {
            unit.handler_at_pc.emplace(insn_pc, unit.handlers.size());
            unit.handlers.push_back(std::move(step));
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
                value item = virtual_machine::jit_pop(current);
                machine.jit_store_name(current, name, item);
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
            const size_t branch_pc = insn_pc;
            bind([target, branch_pc](virtual_machine &machine, frame &current, size_t) {
                const size_t fallthrough_pc = branch_pc + 1 + sizeof(uint32_t);
                const size_t jump_pc = machine.jit_jump_target(current, target);

                branch_predictor &predictor = branch_predictor::instance();
                const bool predicted_taken =
                    predictor.predict(branch_pc, jump_pc, fallthrough_pc);
                const size_t predicted_pc =
                    predicted_taken ? jump_pc : fallthrough_pc;
                current.pc = predicted_pc;

                if (current.jit_unit &&
                    predicted_pc < current.jit_unit->handler_index.size())
                {
                    const size_t next =
                        current.jit_unit->handler_index[predicted_pc];
                    if (next != compiled_unit::invalid_handler)
                    {
                        current.jit_next_handler = next;
                    }
                }

                const bool actually_taken =
                    !is_truthy(virtual_machine::jit_pop(current));
                if (actually_taken != predicted_taken)
                {
                    current.pc = actually_taken ? jump_pc : fallthrough_pc;
                    current.jit_next_handler.reset();
                    if (current.jit_unit &&
                        current.pc < current.jit_unit->handler_index.size())
                    {
                        const size_t next =
                            current.jit_unit->handler_index[current.pc];
                        if (next != compiled_unit::invalid_handler)
                        {
                            current.jit_next_handler = next;
                        }
                    }
                }
                predictor.train(branch_pc, actually_taken, jump_pc);
                if (current.jit_unit)
                {
                    current.jit_unit->runtime_profile.record_branch(branch_pc,
                                                                    actually_taken);
                }
            });
            break;
        }
        case Opcode::JMP_IF_TRUE:
        {
            const uint32_t target = cursor.read_scalar<uint32_t>();
            const size_t branch_pc = insn_pc;
            bind([target, branch_pc](virtual_machine &machine, frame &current, size_t) {
                const size_t fallthrough_pc = branch_pc + 1 + sizeof(uint32_t);
                const size_t jump_pc = machine.jit_jump_target(current, target);

                branch_predictor &predictor = branch_predictor::instance();
                const bool predicted_taken =
                    predictor.predict(branch_pc, jump_pc, fallthrough_pc);
                const size_t predicted_pc =
                    predicted_taken ? jump_pc : fallthrough_pc;
                current.pc = predicted_pc;

                if (current.jit_unit &&
                    predicted_pc < current.jit_unit->handler_index.size())
                {
                    const size_t next =
                        current.jit_unit->handler_index[predicted_pc];
                    if (next != compiled_unit::invalid_handler)
                    {
                        current.jit_next_handler = next;
                    }
                }

                const bool actually_taken =
                    is_truthy(virtual_machine::jit_pop(current));
                if (actually_taken != predicted_taken)
                {
                    current.pc = actually_taken ? jump_pc : fallthrough_pc;
                    current.jit_next_handler.reset();
                    if (current.jit_unit &&
                        current.pc < current.jit_unit->handler_index.size())
                    {
                        const size_t next =
                            current.jit_unit->handler_index[current.pc];
                        if (next != compiled_unit::invalid_handler)
                        {
                            current.jit_next_handler = next;
                        }
                    }
                }
                predictor.train(branch_pc, actually_taken, jump_pc);
                if (current.jit_unit)
                {
                    current.jit_unit->runtime_profile.record_branch(branch_pc,
                                                                    actually_taken);
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
                    value item = virtual_machine::jit_pop(current);
                    value key = virtual_machine::jit_pop(current);
                    map_store_entry(*map, key, item);
                }
                current.stack.push_back(value{map_value{std::move(map)}});
            });
            break;
        }
        case Opcode::INDEX_GET:
            bind([](virtual_machine &, frame &current, size_t) {
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
            bind([member](virtual_machine &, frame &current, size_t) {
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
                value item = virtual_machine::jit_pop(current);
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
            fail_compile("unsupported opcode in JIT compiler: " +
                         std::to_string(static_cast<int>(opcode)));
        }
    }

    unit.handler_index.assign(unit.code.size(), compiled_unit::invalid_handler);
    for (const auto &[pc, index] : unit.handler_at_pc)
    {
        if (pc < unit.handler_index.size())
        {
            unit.handler_index[pc] = index;
        }
    }
}

inline std::shared_ptr<compiled_unit>
recompile_with_runtime_profile(std::shared_ptr<compiled_unit> unit,
                               virtual_machine &vm, std::string_view strings)
{
    const execution_profile saved = unit->runtime_profile;
    unit->code = apply_profile_optimization(unit->code, strings, saved);
    unit->handlers.clear();
    unit->handler_at_pc.clear();
    unit->handler_index.clear();
    compile_handlers(*unit, vm, strings);
    unit->profile_generation = saved.generation;
    return unit;
}

} // namespace munx::vm::jit

namespace munx::vm
{

inline std::shared_ptr<jit::compiled_unit>
virtual_machine::get_jit_unit(std::span<const std::byte> source)
{
    const jit_cache_key key{source.data(), source.size()};
    jit::execution_profile &profile = jit::profile_registry::instance().get(source);

    std::shared_ptr<jit::compiled_unit> stale_unit;
    {
        std::lock_guard<std::mutex> guard{jit_mutex_};
        const auto found = jit_cache_.find(key);
        if (found != jit_cache_.end())
        {
            const std::shared_ptr<jit::compiled_unit> &cached = found->second;
            const bool stale_runtime =
                jit::pgo_enabled() && cached->runtime_profile.is_mature() &&
                cached->profile_generation < cached->runtime_profile.generation;
            if (stale_runtime)
            {
                stale_unit = cached;
                jit_cache_.erase(found);
            }
            else if (!jit::pgo_enabled() ||
                     cached->profile_generation >= profile.generation)
            {
                return cached;
            }
            else
            {
                jit_cache_.erase(found);
            }
        }
    }

    if (stale_unit)
    {
        auto refreshed =
            jit::recompile_with_runtime_profile(stale_unit, *this, image_.strings);
        std::lock_guard<std::mutex> guard{jit_mutex_};
        jit_cache_[key] = refreshed;
        return refreshed;
    }

    const jit::execution_profile *profile_ptr =
        profile.is_mature() ? &profile : nullptr;
    auto unit = jit::compile_unit(*this, source, image_.strings, profile_ptr);
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
    vm_dispatch_scope dispatch{*this};
    jit::pipeline_state pipeline{};
    pipeline.reset(current.pc);
    while (current.pc < unit->code.size())
    {
        const size_t instruction_pc = current.pc;
        sync_call_stack_pc(instruction_pc);

        size_t handler_index = jit::compiled_unit::invalid_handler;
        if (current.jit_next_handler.has_value())
        {
            handler_index = *current.jit_next_handler;
            current.jit_next_handler.reset();
        }
        else
        {
            handler_index = jit::pipeline_fetch_handler(*unit, instruction_pc);
        }

        if (handler_index == jit::compiled_unit::invalid_handler)
        {
            throw_error("JIT missing handler at pc " +
                        std::to_string(instruction_pc) + " in " +
                        current.description);
            if (runtime_fault_pending())
            {
                if (dispatch_runtime_trap(current, instruction_pc))
                {
                    pipeline.reset(current.pc);
                    continue;
                }
                return value{};
            }
        }

        unit->handlers[handler_index](*this, current, instruction_pc);
        if (current.jit_return.has_value())
        {
            return std::move(*current.jit_return);
        }

        if (runtime_fault_pending())
        {
            if (dispatch_runtime_trap(current, instruction_pc))
            {
                pipeline.reset(current.pc);
                continue;
            }
            return value{};
        }

        if (current.pc < unit->handler_index.size())
        {
            const size_t next = unit->handler_index[current.pc];
            if (next != jit::compiled_unit::invalid_handler)
            {
                current.jit_next_handler = next;
            }
        }
        pipeline.reset(current.pc);
    }
    return current.jit_return.value_or(value{});
}

} // namespace munx::vm
