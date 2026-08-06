#pragma once

#include "platform.hpp"
#include "Opcode.hpp"
#include "bytecode_decoder.hpp"
#include "jit/bytecode_utils.hpp"
#include "jit/branch_predictor.hpp"
#include "jit/execution_profile.hpp"
#include "jit/jit_compiler.hpp"
#include "regex_util.hpp"
#include "simd_ops.hpp"
#include "vm_pipe.hpp"
#include "vm_value.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <regex>
#include <span>
#include <string_view>

#if !MUNX_VM_HAS_SOCKETS && defined(MUNX_PLATFORM_WINDOWS)
#error "Windows builds require Winsock socket support"
#endif

/// Interpreter for `.mxb` images produced by @ref munx::bytecode_compiler.
namespace munx::vm
{

// ---------------------------------------------------------------------------
// Program image
// ---------------------------------------------------------------------------

/// One executable code blob plus the parameter names its prologue binds.
struct function_def
{
    std::string name;                  ///< Function or generated lambda name.
    std::string package;               ///< Owning package.
    std::span<const std::byte> code;   ///< Body bytecode.
    std::vector<std::string> parameters; ///< In declaration order.
    std::vector<debug_loc_entry> debug_map; ///< PC → source location anchors.
};

/// One package: its functions and its top-level initializer bytecode.
struct package_def
{
    std::string name;
    std::span<const std::byte> init_code;
    std::vector<function_def *> functions;
    std::vector<debug_loc_entry> init_debug_map;
};

/// A loaded `.mxb` file: owned bytes plus decoded structure.
struct program_image
{
    std::vector<std::byte> bytes;
    std::string_view strings;
    std::vector<std::unique_ptr<function_def>> functions;
    std::vector<package_def> packages; ///< Imports first, entry point last.
    std::string entry_package;
};

/// @return Parameter names bound by the STORE prologue of @p code.
inline std::vector<std::string> scan_parameters(std::span<const std::byte> code,
                                                std::string_view strings)
{
    std::vector<std::string> parameters;
    size_t pc = 0;
    while (pc + 9 <= code.size() &&
           static_cast<Opcode>(code[pc]) == Opcode::STORE)
    {
        uint32_t offset = 0;
        uint32_t length = 0;
        std::memcpy(&offset, code.data() + pc + 1, sizeof offset);
        std::memcpy(&length, code.data() + pc + 5, sizeof length);
        if (offset > strings.size() || length > strings.size() - offset)
        {
            break;
        }
        parameters.emplace_back(strings.substr(offset, length));
        pc += 9;
    }
    // The prologue binds the last argument first.
    std::reverse(parameters.begin(), parameters.end());
    return parameters;
}

inline void resolve_debug_loc(std::span<const debug_loc_entry> debug_map, size_t pc,
                              uint32_t &line, uint32_t &column, std::string &file)
{
    if (debug_map.empty())
    {
        return;
    }
    size_t low = 0;
    size_t high = debug_map.size();
    while (low < high)
    {
        const size_t mid = low + (high - low) / 2;
        if (debug_map[mid].pc <= pc)
        {
            low = mid + 1;
        }
        else
        {
            high = mid;
        }
    }
    if (low == 0)
    {
        return;
    }
    const debug_loc_entry &entry = debug_map[low - 1];
    line = entry.line;
    column = entry.column;
    file = entry.file;
}

inline std::vector<debug_loc_entry> parse_debug_map(std::span<const std::byte> blob,
                                                    std::string_view strings)
{
    std::vector<debug_loc_entry> entries;
    if (blob.size() < sizeof(uint32_t))
    {
        return entries;
    }
    uint32_t count = 0;
    std::memcpy(&count, blob.data(), sizeof count);
    size_t offset = sizeof(uint32_t);
    entries.reserve(count);
    for (uint32_t index = 0; index < count; ++index)
    {
        if (offset + 20 > blob.size())
        {
            fail_compile("truncated debug map in bytecode image");
        }
        debug_loc_entry entry{};
        std::memcpy(&entry.pc, blob.data() + offset, sizeof entry.pc);
        offset += sizeof entry.pc;
        std::memcpy(&entry.line, blob.data() + offset, sizeof entry.line);
        offset += sizeof entry.line;
        std::memcpy(&entry.column, blob.data() + offset, sizeof entry.column);
        offset += sizeof entry.column;
        uint32_t file_offset = 0;
        uint32_t file_length = 0;
        std::memcpy(&file_offset, blob.data() + offset, sizeof file_offset);
        offset += sizeof file_offset;
        std::memcpy(&file_length, blob.data() + offset, sizeof file_length);
        offset += sizeof file_length;
        if (file_offset > strings.size() || file_length > strings.size() - file_offset)
        {
            fail_compile("invalid debug map file reference in bytecode image");
        }
        entry.file = std::string{strings.substr(file_offset, file_length)};
        entries.push_back(std::move(entry));
    }
    return entries;
}

/// Read, validate, and structure the `.mxb` image at @p path.
/// @throws compilation_error if the file is missing or malformed.
inline program_image load_program(const std::filesystem::path &path)
{
    std::ifstream input{path, std::ios::binary};
    if (!input)
    {
        fail_compile("could not open bytecode file: " + path.string());
    }
    input.seekg(0, std::ios::end);
    const std::streamoff end = input.tellg();
    if (end < 0)
    {
        fail_compile("could not determine bytecode file size: " +
                                path.string());
    }
    input.seekg(0, std::ios::beg);

    program_image image;
    image.bytes.resize(static_cast<size_t>(end));
    input.read(reinterpret_cast<char *>(image.bytes.data()),
               static_cast<std::streamsize>(image.bytes.size()));
    if (!input && !image.bytes.empty())
    {
        fail_compile("failed to read bytecode file: " + path.string());
    }

    // Every offset, operand, and opcode is checked here, so the interpreter
    // can decode instructions without revalidating the container.
    std::ostringstream discarded;
    bytecode_decoder validator{image.bytes, discarded};
    validator.decode();

    mx_program_header header{};
    std::memcpy(&header, image.bytes.data(), sizeof header);
    image.strings = std::string_view{
        reinterpret_cast<const char *>(image.bytes.data() +
                                       header.string_table_offset),
        header.string_table_length};

    const auto blob = [&image](size_t offset, size_t length) {
        return std::span<const std::byte>{image.bytes.data() + offset, length};
    };
    const auto name_of = [&image](size_t offset, size_t length) {
        return std::string{image.strings.substr(offset, length)};
    };

    const auto add_package = [&](const mx_package_descriptor &descriptor) {
        package_def package;
        package.name = name_of(descriptor.package_name_offset,
                               descriptor.package_name_length);
        package.init_code = blob(descriptor.package_bytecode_offset,
                                 descriptor.package_bytecode_length);
        if (descriptor.init_debug_map_length > 0)
        {
            package.init_debug_map =
                parse_debug_map(blob(descriptor.init_debug_map_offset,
                                     descriptor.init_debug_map_length),
                                image.strings);
        }
        for (size_t index = 0; index < descriptor.num_function_descriptors;
             ++index)
        {
            mx_function_descriptor function{};
            std::memcpy(&function,
                        image.bytes.data() +
                            descriptor.function_descriptor_array_offset +
                            index * sizeof(mx_function_descriptor),
                        sizeof function);
            auto definition = std::make_unique<function_def>();
            definition->name = name_of(function.function_name_offset,
                                       function.function_name_length);
            definition->package = package.name;
            definition->code = blob(function.function_content_offset,
                                    function.function_content_length);
            definition->parameters =
                scan_parameters(definition->code, image.strings);
            if (function.debug_map_length > 0)
            {
                definition->debug_map =
                    parse_debug_map(blob(function.debug_map_offset, function.debug_map_length),
                                    image.strings);
            }
            package.functions.push_back(definition.get());
            image.functions.push_back(std::move(definition));
        }
        image.packages.push_back(std::move(package));
    };

    for (size_t index = 0; index < header.num_package_import_list; ++index)
    {
        mx_package_descriptor descriptor{};
        std::memcpy(&descriptor,
                    image.bytes.data() + header.package_import_array_offset +
                        index * sizeof(mx_package_descriptor),
                    sizeof descriptor);
        add_package(descriptor);
    }
    add_package(header.entry_point_package_descriptor);
    image.entry_package = image.packages.back().name;
    return image;
}

// ---------------------------------------------------------------------------
// Execution state
// ---------------------------------------------------------------------------

/// Package-level variable table, shared by every munx thread.
class environment
{
    mutable std::mutex mutex_;
    std::unordered_map<std::string, value> values_;

public:
    bool try_get(const std::string &name, value &out) const
    {
        std::lock_guard<std::mutex> guard{mutex_};
        const auto found = values_.find(name);
        if (found == values_.end())
        {
            return false;
        }
        out = found->second;
        return true;
    }

    void set(const std::string &name, value &item)
    {
        std::lock_guard<std::mutex> guard{mutex_};
        values_[name] = std::move(item);
    }

    /// Assign only when @p name already exists.
    /// @return True when the assignment happened.
    bool assign_existing(const std::string &name, value &item)
    {
        std::lock_guard<std::mutex> guard{mutex_};
        const auto found = values_.find(name);
        if (found == values_.end())
        {
            return false;
        }
        found->second = std::move(item);
        return true;
    }
};

/// An active `monitor` region and the operand depth to restore on a trap.
///
/// The protected region runs from just after `MONITOR_ENTER` up to its handler,
/// so a jump out of the region (a `break` or `return` inside `monitor`) leaves
/// a handler that is no longer in scope. Recording the range lets the frame
/// discard those instead of trapping into a dead handler.
struct monitor_state
{
    size_t region_start;
    size_t handler_pc;
    size_t stack_depth;

    bool covers(size_t pc) const
    {
        return pc >= region_start && pc < handler_pc;
    }
};

/// One invocation: a code blob, its operand stack, and its local variables.
struct frame
{
    std::string description;
    std::span<const std::byte> code;
    size_t pc{0};
    value_vector stack;
    std::unordered_map<std::string, value> locals;
    std::vector<monitor_state> monitors;
    bool global_scope{false};
    std::optional<value> jit_return;
    std::shared_ptr<jit::compiled_unit> jit_unit;
    /// When set, `execute_jit` dispatches this handler index without a map lookup.
    std::optional<size_t> jit_next_handler;
    std::span<const debug_loc_entry> debug_map{};
};

/// Recursively decoded CAST operand.
struct type_spec
{
    ast::type_kind kind{ast::type_kind::Primitive};
    ast::primitive_kind primitive{ast::primitive_kind::Void};
    std::string name;
    std::vector<type_spec> elements;
};

// ---------------------------------------------------------------------------
// Interpreter
// ---------------------------------------------------------------------------

/// Executes a loaded program image.
class virtual_machine
{
    program_image image_;
    environment globals_;
    std::unordered_map<std::string, const function_def *> functions_;
    std::unordered_map<std::string, std::vector<std::string>> enums_;
    std::mutex threads_mutex_;
    std::vector<thread_ref> threads_;
    std::mutex console_mutex_;
    std::atomic<bool> failed_{false};

    struct jit_cache_key
    {
        const std::byte *data{nullptr};
        size_t size{0};

        bool operator==(const jit_cache_key &other) const noexcept
        {
            if (size != other.size)
            {
                return false;
            }
            if (size == 0)
            {
                return true;
            }
            return std::memcmp(data, other.data, size) == 0;
        }
    };

    struct jit_cache_key_hash
    {
        size_t operator()(const jit_cache_key &key) const noexcept
        {
            size_t hash = key.size;
            for (size_t index = 0; index < key.size && index < 64; ++index)
            {
                hash ^= static_cast<size_t>(std::to_integer<uint8_t>(key.data[index])) +
                        0x9e3779b9 + (hash << 6) + (hash >> 2);
            }
            return hash;
        }
    };

    mutable std::mutex jit_mutex_;
    std::unordered_map<jit_cache_key, std::shared_ptr<jit::compiled_unit>,
                       jit_cache_key_hash>
        jit_cache_;

    static constexpr size_t call_depth_limit = 512;

    /// Deliver a pending runtime fault to a covering `trap` handler when possible.
    static bool dispatch_runtime_trap(frame &current, size_t instruction_pc)
    {
        if (!runtime_fault_pending())
        {
            return false;
        }
        while (!current.monitors.empty() &&
               !current.monitors.back().covers(instruction_pc))
        {
            current.monitors.pop_back();
        }
        if (current.monitors.empty())
        {
            return false;
        }
        const monitor_state handler = current.monitors.back();
        current.monitors.pop_back();
        current.stack.resize(handler.stack_depth);
        current.stack.push_back(tls_runtime_fault.payload);
        current.pc = handler.handler_pc;
        tls_runtime_fault.clear();
        return true;
    }

public:
    /// Bind @p image to a fresh runtime; @p arguments becomes `argv`.
    virtual_machine(program_image image, std::vector<std::string> arguments)
        : image_(std::move(image))
    {
        register_functions();
        install_prelude(std::move(arguments));
    }

    /// Run every package initializer, imports first.
    /// @return Process exit status: 0 on success, 1 after a runtime error.
    int run()
    {
        call_stack_state stack;
        active_call_stack = &stack;
        for (const package_def &package : image_.packages)
        {
            frame initializer;
            initializer.description = "package " + package.name;
            initializer.code = package.init_code;
            initializer.global_scope = true;
            initializer.debug_map =
                std::span<const debug_loc_entry>(package.init_debug_map);
            push_call_stack(initializer);
            execute(initializer);
            pop_call_stack();
            if (runtime_fault_pending())
            {
                report_error(tls_runtime_fault.message, tls_runtime_fault.stack_trace);
                tls_runtime_fault.clear();
                break;
            }
        }
        active_call_stack = nullptr;
        join_threads();
        std::cout.flush();
        return failed_ ? 1 : 0;
    }

    /// Invoke @p callee with @p arguments. Used by opcodes and by builtins
    /// such as `thread`.
    value call_value(const value &callee, value_vector &arguments)
    {
        if (const auto *function = callee.get_if<function_value>())
        {
            const value result = call_function(*function->def, arguments);
            if (runtime_fault_pending())
            {
                return value{};
            }
            return result;
        }
        if (const auto *builtin = callee.get_if<builtin_ref>())
        {
            const value result = (*builtin)->invoke(*this, arguments);
            if (runtime_fault_pending())
            {
                return value{};
            }
            return result;
        }
        if (const auto *handle = callee.get_if<io_ref>())
        {
            std::string text;
            for (const value &argument : arguments)
            {
                text += to_display_string(argument);
            }
            return value{static_cast<int64_t>(write_handle(*handle, text))};
        }
        throw_error(std::string{"value of type "} + type_name(callee) +
                    " is not callable");
        return value{};
    }

    /// Print @p message as a runtime diagnostic and mark the run as failed.
    void report_error(std::string_view message)
    {
        report_error(message, {});
    }

    void report_error(std::string_view message,
                      const std::vector<stack_frame_info> &trace)
    {
        std::lock_guard<std::mutex> guard{console_mutex_};
        std::cout.flush();
        std::cerr << "munx: runtime error: " << message << '\n';
        for (auto it = trace.rbegin(); it != trace.rend(); ++it)
        {
            uint32_t line = it->line;
            uint32_t column = it->column;
            std::string file = it->file;
            resolve_debug_loc(it->debug_map, it->pc, line, column, file);
            std::cerr << "  at ";
            if (!file.empty() && line != 0)
            {
                std::cerr << file << ':' << line << ':' << column << ' ';
            }
            else if (it->pc != 0)
            {
                std::cerr << "pc " << it->pc << ' ';
            }
            std::cerr << "in " << it->label << '\n';
        }
        failed_ = true;
    }

    /// Write @p text to standard output under the console lock.
    void write_console(std::string_view text)
    {
        std::lock_guard<std::mutex> guard{console_mutex_};
        std::cout << text;
    }

    /// Start @p callee on a new munx thread with @p arguments.
    value spawn_thread(value &callee, value_vector &arguments)
    {
        auto handle = std::make_shared<thread_object>();
        handle->worker = std::thread([this, callee = std::move(callee),
                                      arguments = std::move(arguments)]() mutable {
            call_stack_state stack;
            active_call_stack = &stack;
            call_value(callee, arguments);
            if (runtime_fault_pending())
            {
                report_error(tls_runtime_fault.message, tls_runtime_fault.stack_trace);
                tls_runtime_fault.clear();
            }
            active_call_stack = nullptr;
        });
        {
            std::lock_guard<std::mutex> guard{threads_mutex_};
            threads_.push_back(handle);
        }
        return value{handle};
    }

    static void push_call_stack(const frame &current)
    {
        if (active_call_stack == nullptr)
        {
            return;
        }
        stack_frame_info info{};
        info.label = current.description;
        info.pc = current.pc;
        info.debug_map = current.debug_map;
        active_call_stack->push(std::move(info));
    }

    static void pop_call_stack()
    {
        if (active_call_stack != nullptr)
        {
            active_call_stack->pop();
        }
    }

    static void sync_call_stack_pc(size_t pc)
    {
        if (active_call_stack != nullptr)
        {
            active_call_stack->update_pc(pc);
        }
    }

    static int64_t checked_int_add(int64_t left, int64_t right)
    {
        int64_t result = 0;
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_add_overflow(left, right, &result))
        {
            throw_overflow("integer addition");
        }
#else
        if ((right > 0 && left > std::numeric_limits<int64_t>::max() - right) ||
            (right < 0 && left < std::numeric_limits<int64_t>::min() - right))
        {
            throw_overflow("integer addition");
        }
        result = left + right;
#endif
        return result;
    }

    static int64_t checked_int_sub(int64_t left, int64_t right)
    {
        int64_t result = 0;
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_sub_overflow(left, right, &result))
        {
            throw_overflow("integer subtraction");
        }
#else
        if ((right > 0 && left < std::numeric_limits<int64_t>::min() + right) ||
            (right < 0 && left > std::numeric_limits<int64_t>::max() + right))
        {
            throw_overflow("integer subtraction");
        }
        result = left - right;
#endif
        return result;
    }

    static int64_t checked_int_mul(int64_t left, int64_t right)
    {
        int64_t result = 0;
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_mul_overflow(left, right, &result))
        {
            throw_overflow("integer multiplication");
        }
#else
        if (left != 0 && right != 0 &&
            ((left > 0 && right > 0 && left > std::numeric_limits<int64_t>::max() / right) ||
             (left < 0 && right < 0 && left < std::numeric_limits<int64_t>::max() / right) ||
             (left > 0 && right < 0 && right < std::numeric_limits<int64_t>::min() / left) ||
             (left < 0 && right > 0 && left < std::numeric_limits<int64_t>::min() / right)))
        {
            throw_overflow("integer multiplication");
        }
        result = left * right;
#endif
        return result;
    }

    static int64_t checked_int_neg(int64_t value)
    {
        if (value == std::numeric_limits<int64_t>::min())
        {
            throw_overflow("integer negation");
        }
        return -value;
    }

    // -- threaded JIT entry points (used by jit_compiler_impl.hpp) ----------

    [[nodiscard]] value jit_push_function(const std::string &name)
    {
        const auto found = functions_.find(name);
        if (found == functions_.end())
        {
            throw_error("unknown function `" + name + "`");
            return value{};
        }
        return value{function_value{found->second}};
    }

    value jit_load_name(frame &current, const std::string &name)
    {
        return load_name(current, name);
    }

    void jit_store_name(frame &current, const std::string &name, value &item)
    {
        store_name(current, name, item);
    }

    [[nodiscard]] size_t jit_jump_target(const frame &current,
                                         uint32_t target) const
    {
        return jump_target(current, target);
    }

    value jit_cast_value(const value &operand, const jit::decoded_type &target)
    {
        return cast_value(operand, from_decoded_type(target));
    }

    void jit_define_enum(const std::string &name,
                         const std::vector<std::string> &members)
    {
        enums_[name] = members;
    }

    void jit_define_object_type(frame &current, const std::string &name,
                                const std::vector<std::string> &fields)
    {
        define_object_type(current, name, fields);
    }

    void jit_free_buffer(frame &current, const std::string &name)
    {
        const value target = load_name(current, name);
        if (const auto *buffer = target.get_if<buffer_ref>())
        {
            release_buffer(*buffer, name);
        }
    }

    void jit_pipe_insert(frame &current, const std::string &name, value &item)
    {
        expect_pipe(load_name(current, name), name)->insert(item);
    }

    value jit_pipe_extract(frame &current, const std::string &name)
    {
        return expect_pipe(load_name(current, name), name)->extract();
    }

    void jit_lock_create(frame &current, const std::string &name)
    {
        auto guard = std::make_shared<lock_object>();
        guard->name = name;
        value guard_value{guard};
        store_name(current, name, guard_value);
    }

    void jit_lock_acquire(frame &current, const std::string &name)
    {
        expect_lock(load_name(current, name), name)->acquire();
    }

    void jit_lock_release(frame &current, const std::string &name)
    {
        expect_lock(load_name(current, name), name)->release();
    }

    static value jit_pop(frame &current) { return pop(current); }

    static value jit_clone_for_return(const value &item)
    {
        return clone_for_return(item);
    }

    static void jit_dup(frame &current)
    {
        if (current.stack.empty())
        {
            throw_error("operand stack underflow in " + current.description);
        }
        current.stack.push_back(current.stack.back());
    }

    static void jit_swap(frame &current)
    {
        if (current.stack.size() < 2)
        {
            throw_error("operand stack underflow in " + current.description);
        }
        std::iter_swap(current.stack.end() - 1, current.stack.end() - 2);
    }

    static value jit_neg(const value &operand)
    {
        if (is_integral(operand))
        {
            return value{checked_int_neg(as_integer(operand))};
        }
        return value{-as_number(operand)};
    }

    static value jit_binary_arithmetic(Opcode opcode, const value &left,
                                       const value &right)
    {
        return binary_arithmetic(opcode, left, right);
    }

    static value jit_compare(Opcode opcode, const value &left,
                             const value &right)
    {
        return compare(opcode, left, right);
    }

    static value jit_bitwise(Opcode opcode, const value &left,
                             const value &right)
    {
        return bitwise(opcode, left, right);
    }

    static value jit_bitwise_not(const value &operand)
    {
        if (!is_integral(operand))
        {
            throw_error(std::string{"`~` needs an integer, got "} +
                        type_name(operand));
        }
        return value{~as_integer(operand)};
    }

    static value jit_index_get(const value &container, const value &index)
    {
        return index_value(container, index);
    }

    static value jit_member_get(const value &container, const std::string &member)
    {
        return member_value(container, member);
    }

    static const value_vector &jit_unpack_elements(const value &aggregate)
    {
        return elements_of(aggregate);
    }

private:
    // -- setup --------------------------------------------------------------

    /// Publish every compiled function, imports first so the entry package
    /// wins on a name collision.
    void register_functions()
    {
        for (const package_def &package : image_.packages)
        {
            for (const function_def *function : package.functions)
            {
                functions_[function->name] = function;
                value entry{function_value{function}};
                globals_.set(function->name, entry);
            }
        }
    }

    void define_builtin(const std::string &name,
                        std::function<value(virtual_machine &, value_vector &)> body)
    {
        auto builtin = std::make_shared<builtin_object>();
        builtin->name = name;
        builtin->invoke = std::move(body);
        value entry{builtin};
        globals_.set(name, entry);
    }

    static void expect_arity(const value_vector &arguments, size_t least,
                             size_t most, const char *name)
    {
        if (arguments.size() < least || arguments.size() > most)
        {
            throw_error(std::string{name} + " expects " +
                        (least == most ? std::to_string(least)
                                       : std::to_string(least) + " to " +
                                             std::to_string(most)) +
                        " argument(s), got " + std::to_string(arguments.size()));
            return;
        }
    }

    static const std::string &expect_string(const value &item, const char *name)
    {
        if (const auto *text = try_string(item))
        {
            return *text;
        }
        throw_error(std::string{name} + " expects a string, got " +
                    type_name(item));
        static const std::string k_empty;
        return k_empty;
    }

    static const io_ref &expect_handle(const value &item, const char *name)
    {
        if (const auto *handle = item.get_if<io_ref>())
        {
            return *handle;
        }
        throw_error(std::string{name} + " expects an io handle, got " +
                    type_name(item));
        static io_ref k_empty = std::make_shared<io_object>();
        return k_empty;
    }

    void install_prelude(const std::vector<std::string> &arguments)
    {
        value mode_in{mode_value{"in"}};
        value mode_out{mode_value{"out"}};
        value mode_subscribe{mode_value{"subscribe"}};
        value null_value{};
        globals_.set("in", mode_in);
        globals_.set("out", mode_out);
        globals_.set("subscribe", mode_subscribe);
        globals_.set("null", null_value);

        auto argv = std::make_shared<sequence_object>();
        for (const std::string &argument : arguments)
        {
            argv->items.push_back(value{argument});
        }
        value argv_value{array_value{argv}};
        globals_.set("argv", argv_value);

        auto process = std::make_shared<namespace_object>();
        process->name = "process";
        {
            auto id = std::make_shared<builtin_object>();
            id->name = "process.id";
            id->invoke = [](virtual_machine &, value_vector &) {
                return value{static_cast<int64_t>(munx::platform_process_id())};
            };
            process->members.emplace("id", value{id});
        }
        value process_value{process};
        globals_.set("process", process_value);

        install_console_builtins();
        install_string_builtins();
        install_collection_builtins();
        install_map_builtins();
        install_io_builtins();
        install_concurrency_builtins();
    }

    void install_console_builtins()
    {
        define_builtin("print", [](virtual_machine &machine,
                                   value_vector &arguments) {
            std::string text;
            for (const value &argument : arguments)
            {
                text += to_display_string(argument);
            }
            machine.write_console(text);
            return value{static_cast<int64_t>(text.size())};
        });
        define_builtin("println", [](virtual_machine &machine,
                                     value_vector &arguments) {
            std::string text;
            for (const value &argument : arguments)
            {
                text += to_display_string(argument);
            }
            text += '\n';
            machine.write_console(text);
            return value{static_cast<int64_t>(text.size())};
        });
        define_builtin("fail", [](virtual_machine &, value_vector &arguments) -> value {
            std::string message;
            for (const value &argument : arguments)
            {
                message += to_display_string(argument);
            }
            throw_error(message.empty() ? "failed" : message);
            return value{};
        });
        define_builtin("fix", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 1, 1, "fix");
            return arguments.front();
        });
    }

    void install_string_builtins()
    {
        define_builtin("concat", [](virtual_machine &, value_vector &arguments) {
            std::string text;
            for (const value &argument : arguments)
            {
                text += to_display_string(argument);
            }
            return value{std::move(text)};
        });
        define_builtin("trim", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 1, 1, "trim");
            const std::string &text = expect_string(arguments.front(), "trim");
            const auto first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
            {
                return value{std::string{}};
            }
            const auto last = text.find_last_not_of(" \t\r\n");
            return value{text.substr(first, last - first + 1)};
        });
        define_builtin("split", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 1, 2, "split");
            const std::string &text = expect_string(arguments.front(), "split");
            const std::string separator =
                arguments.size() > 1 ? to_display_string(arguments[1]) : " ";
            auto parts = std::make_shared<sequence_object>();
            if (separator.empty())
            {
                for (const char letter : text)
                {
                    parts->items.push_back(value{std::string(1, letter)});
                }
                return value{array_value{parts}};
            }
            size_t start = 0;
            while (true)
            {
                const size_t hit = text.find(separator, start);
                if (hit == std::string::npos)
                {
                    parts->items.push_back(value{text.substr(start)});
                    break;
                }
                parts->items.push_back(value{text.substr(start, hit - start)});
                start = hit + separator.size();
            }
            return value{array_value{parts}};
        });
        define_builtin("has_substring_regex",
                       [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 2, 2, "has_substring_regex");
            const std::string &text =
                expect_string(arguments.front(), "has_substring_regex");
            const std::string pattern = to_display_string(arguments[1]);
            munx::error regex_error;
            const bool matched =
                munx::regex_search_noexcept(text, pattern, &regex_error);
            if (!regex_error.ok())
            {
                throw_error(regex_error.message);
            }
            return value{matched};
        });
        define_builtin("len", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 1, 1, "len");
            return value{length_of(arguments.front())};
        });
    }

    void install_collection_builtins()
    {
        define_builtin("queue", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 0, 1, "queue");
            return value{array_value{std::make_shared<sequence_object>()}};
        });
        define_builtin("append", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 2, 2, "append");
            expect_sequence(arguments.front(), "append")
                ->items.push_back(arguments[1]);
            return arguments.front();
        });
        define_builtin("push", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 2, 2, "push");
            if (const auto *channel = arguments.front().get_if<pipe_ref>())
            {
                (*channel)->insert(arguments[1]);
                return value{};
            }
            expect_sequence(arguments.front(), "push")
                ->items.push_back(arguments[1]);
            return value{};
        });
        define_builtin("pop", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 1, 1, "pop");
            if (const auto *channel = arguments.front().get_if<pipe_ref>())
            {
                return (*channel)->extract();
            }
            const sequence_ref items = expect_sequence(arguments.front(), "pop");
            if (items->items.empty())
            {
                return value{};
            }
            value front = std::move(items->items.front());
            items->items.erase(items->items.begin());
            return front;
        });
        define_builtin("remove_at", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 2, 2, "remove_at");
            const sequence_ref items =
                expect_sequence(arguments.front(), "remove_at");
            const int64_t position = as_integer(arguments[1]);
            if (position < 0 ||
                static_cast<size_t>(position) >= items->items.size())
            {
                throw_error("remove_at index " + std::to_string(position) +
                            " is out of range (size " +
                            std::to_string(items->items.size()) + ")");
            }
            value removed =
                std::move(items->items[static_cast<size_t>(position)]);
            items->items.erase(items->items.begin() + position);
            return removed;
        });
        define_builtin("sleep", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 1, 1, "sleep");
            const int64_t milliseconds = as_integer(arguments.front());
            if (milliseconds > 0)
            {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds{milliseconds});
            }
            return value{};
        });
    }

    void install_map_builtins()
    {
        define_builtin("get", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 2, 2, "get");
            const map_ref map = expect_map(arguments[0], "get");
            if (const value *found = map_find_entry(*map, arguments[1]))
            {
                return *found;
            }
            return value{};
        });
        define_builtin("insert", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 2, 2, "insert");
            const map_ref target = expect_map(arguments[0], "insert");
            const map_ref patch = expect_map(arguments[1], "insert");
            map_merge(*target, *patch);
            return value{};
        });
        define_builtin("env", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 1, 1, "env");
            const std::string name = string_data(arguments.front());
            if (const char *env_text = std::getenv(name.c_str()))
            {
                return value{make_string(env_text)};
            }
            return value{};
        });
    }

    void install_io_builtins()
    {
        define_builtin("open", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 2, 3, "open");
            const auto *kind = arguments.front().get_if<enum_value>();
            if (kind == nullptr)
            {
                throw_error("open expects an io_type enum member as its first "
                            "argument");
            }
            if (kind->member == "tty")
            {
                return open_terminal(arguments);
            }
            if (kind->member == "file")
            {
                return open_file(arguments);
            }
            if (kind->member == "socket")
            {
                return open_socket(arguments);
            }
            throw_error("unsupported io_type::" + kind->member);
            return value{};
        });
        define_builtin("close", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 1, 1, "close");
            close_handle(expect_handle(arguments.front(), "close"));
            return value{};
        });
        define_builtin("write", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 1, 8, "write");
            const io_ref &handle = expect_handle(arguments.front(), "write");
            std::string text;
            for (size_t index = 1; index < arguments.size(); ++index)
            {
                text += to_display_string(arguments[index]);
            }
            return value{static_cast<int64_t>(write_handle(handle, text))};
        });
        define_builtin("read", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 1, 2, "read");
            const io_ref &handle = expect_handle(arguments.front(), "read");
            const int64_t limit =
                arguments.size() > 1 ? as_integer(arguments[1]) : -1;
            return read_handle(handle, limit);
        });
        define_builtin("readln", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 0, 1, "readln");
            if (arguments.empty())
            {
                std::string line;
                if (!std::getline(std::cin, line))
                {
                    return value{};
                }
                return value{std::move(line)};
            }
            return read_line(expect_handle(arguments.front(), "readln"));
        });
        define_builtin("bind", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 3, 3, "bind");
            return bind_socket(expect_handle(arguments.front(), "bind"),
                               expect_string(arguments[1], "bind"),
                               as_integer(arguments[2]));
        });
        define_builtin("listen", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 1, 2, "listen");
            const int64_t backlog =
                arguments.size() > 1 ? as_integer(arguments[1]) : 16;
            return listen_socket(expect_handle(arguments.front(), "listen"),
                                 backlog);
        });
        define_builtin("accept", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 1, 1, "accept");
            return accept_socket(expect_handle(arguments.front(), "accept"));
        });
    }

    void install_concurrency_builtins()
    {
        define_builtin("pipe", [](virtual_machine &, value_vector &arguments) {
            expect_arity(arguments, 0, 2, "pipe");
            const std::string id =
                arguments.empty() ? std::string{"anonymous"}
                                  : to_display_string(arguments.front());
            if (arguments.size() < 2)
            {
                throw_error("pipe expects `in`, `out`, or `subscribe` as its mode");
            }
            const auto *mode = arguments[1].get_if<mode_value>();
            if (mode == nullptr)
            {
                throw_error("pipe expects `in`, `out`, or `subscribe` as its mode");
            }
            const bool subscribing = mode->name == "subscribe";
            const bool reading = mode->name == "in" || subscribing;
            const bool writing = mode->name == "out";
            if (reading == writing)
            {
                throw_error("pipe mode must be `in`, `out`, or `subscribe`");
            }
            return value{pipe_object::open(id, reading, writing, subscribing)};
        });
        define_builtin("thread", [](virtual_machine &machine,
                                    value_vector &arguments) {
            expect_arity(arguments, 1, 2, "thread");
            value_vector captured;
            if (arguments.size() > 1)
            {
                if (const auto *tuple = arguments[1].get_if<tuple_value>())
                {
                    captured = tuple->data->items;
                }
                else if (const auto *array = arguments[1].get_if<array_value>())
                {
                    captured = array->data->items;
                }
                else if (!arguments[1].is_null())
                {
                    captured.push_back(arguments[1]);
                }
            }
            value callee = arguments.front();
            return machine.spawn_thread(callee, captured);
        });
        define_builtin("join", [](virtual_machine &, value_vector &arguments) {
            for (const value &argument : arguments)
            {
                if (const auto *handle = argument.get_if<thread_ref>())
                {
                    (*handle)->join();
                    continue;
                }
                if (!argument.is_null())
                {
                    throw_error(std::string{"join expects thread handles, got "} +
                                type_name(argument));
                }
            }
            return value{};
        });
    }

    // -- io helpers ---------------------------------------------------------

    static value open_terminal(value_vector &arguments)
    {
        auto handle = std::make_shared<io_object>();
        handle->kind = io_object::handle_kind::Term;
        const auto *mode = arguments.size() > 1
                               ? arguments[1].get_if<mode_value>()
                               : nullptr;
        const bool reading = mode != nullptr && mode->name == "in";
        handle->readable = reading;
        handle->writable = !reading;
        handle->term_in = reading ? &std::cin : nullptr;
        handle->term_out = reading ? nullptr : &std::cout;
        handle->description = reading ? "term in" : "term out";
        return value{handle};
    }

    static value open_file(value_vector &arguments)
    {
        expect_arity(arguments, 3, 3, "open(io_type::file, …)");
        const std::string &path = expect_string(arguments[1], "open");
        const auto *mode = arguments[2].get_if<mode_value>();
        if (mode == nullptr)
        {
            throw_error("open expects `in` or `out` as the file mode");
        }
        const bool reading = mode->name == "in";
        auto handle = std::make_shared<io_object>();
        handle->kind = io_object::handle_kind::File;
        handle->readable = reading;
        handle->writable = !reading;
        handle->description = "file " + path;
        handle->file = std::make_unique<std::fstream>(
            path, reading ? std::ios::in
                          : std::ios::out | std::ios::trunc);
        if (!handle->file->is_open())
        {
            throw_error("could not open file: " + path);
        }
        return value{handle};
    }

    static value open_socket(value_vector &arguments)
    {
#if MUNX_VM_HAS_SOCKETS
        expect_arity(arguments, 3, 3, "open(io_type::socket, …)");
        const auto *family = arguments[1].get_if<enum_value>();
        const auto *kind = arguments[2].get_if<enum_value>();
        if (family == nullptr || kind == nullptr)
        {
            throw_error("open expects af_type and sock_type enum members");
        }
        const int domain = family->member == "inet6" ? AF_INET6 : AF_INET;
        const int type = kind->member == "dgram" ? SOCK_DGRAM : SOCK_STREAM;
        const munx::platform_socket descriptor =
            ::socket(domain, type, 0);
        if (!munx::platform_socket_valid(descriptor))
        {
            throw_error(std::string{"socket() failed: "} +
                        munx::socket_error_message());
        }
        const int reuse = 1;
        ::setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char *>(&reuse), sizeof reuse);
        auto handle = std::make_shared<io_object>();
        handle->kind = io_object::handle_kind::Socket;
        handle->readable = true;
        handle->writable = true;
        handle->descriptor = munx::platform_socket_to_descriptor(descriptor);
        handle->description = "socket";
        return value{handle};
#else
        (void)arguments;
        throw_error("sockets are not supported on this platform");
#endif
    }

    static size_t write_handle(const io_ref &handle, const std::string &text)
    {
        std::lock_guard<std::mutex> guard{handle->mutex};
        if (handle->closed)
        {
            throw_error("write to a closed " + handle->description);
        }
        if (!handle->writable)
        {
            throw_error("write to a read-only " + handle->description);
        }
        switch (handle->kind)
        {
        case io_object::handle_kind::Term:
            *handle->term_out << text;
            handle->term_out->flush();
            return text.size();
        case io_object::handle_kind::File:
            handle->file->write(text.data(),
                                static_cast<std::streamsize>(text.size()));
            // Flushed eagerly so log-style writers are observable while the
            // handle is still open.
            handle->file->flush();
            if (!*handle->file)
            {
                throw_error("failed to write to " + handle->description);
            }
            return text.size();
        case io_object::handle_kind::Socket:
#if MUNX_VM_HAS_SOCKETS
        {
            size_t sent = 0;
            while (sent < text.size())
            {
                const int written = ::send(
                    munx::platform_descriptor_to_socket(handle->descriptor),
                    text.data() + sent,
                    static_cast<int>(text.size() - sent), 0);
                if (written <= 0)
                {
                    throw_error(std::string{"send() failed: "} +
                                munx::socket_error_message());
                }
                sent += static_cast<size_t>(written);
            }
            return sent;
        }
#else
            throw_error("sockets are not supported on this platform");
#endif
        }
        return 0;
    }

    static value read_handle(const io_ref &handle, int64_t limit)
    {
        std::lock_guard<std::mutex> guard{handle->mutex};
        if (handle->closed)
        {
            throw_error("read from a closed " + handle->description);
        }
        if (!handle->readable)
        {
            throw_error("read from a write-only " + handle->description);
        }
        const size_t requested =
            limit > 0 ? static_cast<size_t>(limit) : static_cast<size_t>(4096);
        switch (handle->kind)
        {
        case io_object::handle_kind::File:
        {
            if (limit <= 0)
            {
                std::ostringstream contents;
                contents << handle->file->rdbuf();
                std::string text = contents.str();
                if (text.empty())
                {
                    return value{};
                }
                return value{std::move(text)};
            }
            std::string text(requested, '\0');
            handle->file->read(text.data(),
                               static_cast<std::streamsize>(requested));
            text.resize(static_cast<size_t>(handle->file->gcount()));
            if (text.empty())
            {
                return value{};
            }
            return value{std::move(text)};
        }
        case io_object::handle_kind::Term:
        {
            std::string text(requested, '\0');
            handle->term_in->read(text.data(),
                                  static_cast<std::streamsize>(requested));
            text.resize(static_cast<size_t>(handle->term_in->gcount()));
            if (text.empty())
            {
                return value{};
            }
            return value{std::move(text)};
        }
        case io_object::handle_kind::Socket:
#if MUNX_VM_HAS_SOCKETS
        {
            std::string text(requested, '\0');
            const int received = ::recv(
                munx::platform_descriptor_to_socket(handle->descriptor),
                text.data(), static_cast<int>(requested), 0);
            if (received < 0)
            {
                throw_error(std::string{"recv() failed: "} +
                            munx::socket_error_message());
            }
            if (received == 0)
            {
                return value{};
            }
            text.resize(static_cast<size_t>(received));
            return value{std::move(text)};
        }
#else
            throw_error("sockets are not supported on this platform");
#endif
        }
        return value{};
    }

    static value read_line(const io_ref &handle)
    {
        std::lock_guard<std::mutex> guard{handle->mutex};
        if (handle->closed || !handle->readable)
        {
            throw_error("readln on a closed or write-only " +
                        handle->description);
        }
        std::string line;
        if (handle->kind == io_object::handle_kind::File)
        {
            if (!std::getline(*handle->file, line))
            {
                return value{};
            }
            return value{std::move(line)};
        }
        if (handle->kind == io_object::handle_kind::Term)
        {
            if (!std::getline(*handle->term_in, line))
            {
                return value{};
            }
            return value{std::move(line)};
        }
#if MUNX_VM_HAS_SOCKETS
        char letter = '\0';
        while (true)
        {
            const int received = ::recv(
                munx::platform_descriptor_to_socket(handle->descriptor),
                &letter, 1, 0);
            if (received <= 0)
            {
                if (line.empty())
                {
                    return value{};
                }
                break;
            }
            if (letter == '\n')
            {
                break;
            }
            line.push_back(letter);
        }
        return value{std::move(line)};
#else
        throw_error("sockets are not supported on this platform");
#endif
    }

    static void close_handle(const io_ref &handle)
    {
        std::lock_guard<std::mutex> guard{handle->mutex};
        if (handle->closed)
        {
            return;
        }
        handle->closed = true;
        if (handle->kind == io_object::handle_kind::File && handle->file)
        {
            handle->file->flush();
            handle->file->close();
        }
#if MUNX_VM_HAS_SOCKETS
        if (handle->kind == io_object::handle_kind::Socket &&
            munx::platform_descriptor_valid(handle->descriptor))
        {
            munx::platform_close_socket_descriptor(handle->descriptor);
            handle->descriptor = -1;
        }
#endif
    }

    static value bind_socket(const io_ref &handle, const std::string &host,
                             int64_t port)
    {
#if MUNX_VM_HAS_SOCKETS
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port));
        if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
        {
            throw_error("bind: invalid address " + host);
        }
        if (::bind(munx::platform_descriptor_to_socket(handle->descriptor),
                   reinterpret_cast<const sockaddr *>(&address),
                   sizeof address) != 0)
        {
            throw_error(std::string{"bind() failed: "} +
                        munx::socket_error_message());
        }
        return value{};
#else
        (void)handle;
        (void)host;
        (void)port;
        throw_error("sockets are not supported on this platform");
#endif
    }

    static value listen_socket(const io_ref &handle, int64_t backlog)
    {
#if MUNX_VM_HAS_SOCKETS
        if (::listen(munx::platform_descriptor_to_socket(handle->descriptor),
                     static_cast<int>(backlog)) != 0)
        {
            throw_error(std::string{"listen() failed: "} +
                        munx::socket_error_message());
        }
        return value{};
#else
        (void)handle;
        (void)backlog;
        throw_error("sockets are not supported on this platform");
#endif
    }

    static value accept_socket(const io_ref &handle)
    {
#if MUNX_VM_HAS_SOCKETS
        sockaddr_in peer{};
        socklen_t peer_size = sizeof peer;
        const munx::platform_socket descriptor = ::accept(
            munx::platform_descriptor_to_socket(handle->descriptor),
            reinterpret_cast<sockaddr *>(&peer), &peer_size);
        if (!munx::platform_socket_valid(descriptor))
        {
            throw_error(std::string{"accept() failed: "} +
                        munx::socket_error_message());
        }
        char address[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &peer.sin_addr, address, sizeof address);

        auto client = std::make_shared<io_object>();
        client->kind = io_object::handle_kind::Socket;
        client->readable = true;
        client->writable = true;
        client->descriptor = munx::platform_socket_to_descriptor(descriptor);
        client->description = "socket";

        auto pair = std::make_shared<sequence_object>();
        pair->items.push_back(value{client});
        pair->items.push_back(value{std::string{address} + ":" +
                                    std::to_string(ntohs(peer.sin_port))});
        return value{tuple_value{pair}};
#else
        (void)handle;
        throw_error("sockets are not supported on this platform");
#endif
    }

    void join_threads()
    {
        while (true)
        {
            thread_ref pending;
            {
                std::lock_guard<std::mutex> guard{threads_mutex_};
                if (threads_.empty())
                {
                    return;
                }
                pending = threads_.back();
                threads_.pop_back();
            }
            pending->join();
        }
    }

    // -- value helpers ------------------------------------------------------

    static int64_t length_of(const value &item)
    {
        if (const auto *text = try_string(item))
        {
            return static_cast<int64_t>(text->size());
        }
        if (const auto *array = item.get_if<array_value>())
        {
            return static_cast<int64_t>(array->data->items.size());
        }
        if (const auto *tuple = item.get_if<tuple_value>())
        {
            return static_cast<int64_t>(tuple->data->items.size());
        }
        if (const auto *buffer = item.get_if<buffer_ref>())
        {
            return static_cast<int64_t>((*buffer)->items.size());
        }
        if (const auto *object = item.get_if<object_ref>())
        {
            return static_cast<int64_t>((*object)->fields.size());
        }
        throw_error(std::string{"cannot take the length of "} + type_name(item));
        return 0;
    }

    /// @return The mutable backing store of an array or tuple value.
    static sequence_ref expect_sequence(const value &item, const char *name)
    {
        if (const auto *array = item.get_if<array_value>())
        {
            return array->data;
        }
        if (const auto *tuple = item.get_if<tuple_value>())
        {
            return tuple->data;
        }
        throw_error(std::string{name} + " expects an array, got " +
                    type_name(item));
        static sequence_ref k_empty = std::make_shared<sequence_object>();
        return k_empty;
    }

    static const value_vector &elements_of(const value &item)
    {
        if (const auto *array = item.get_if<array_value>())
        {
            return array->data->items;
        }
        if (const auto *tuple = item.get_if<tuple_value>())
        {
            return tuple->data->items;
        }
        if (const auto *buffer = item.get_if<buffer_ref>())
        {
            return (*buffer)->items;
        }
        throw_error(std::string{"expected an array or tuple, got "} +
                    type_name(item));
        static const value_vector k_empty;
        return k_empty;
    }

    static value index_value(const value &container, const value &index)
    {
        const int64_t position = as_integer(index);
        if (const auto *text = try_string(container))
        {
            if (position < 0 || static_cast<size_t>(position) >= text->size())
            {
                throw_error("string index " + std::to_string(position) +
                            " is out of range");
            }
            return value{(*text)[static_cast<size_t>(position)]};
        }
        const value_vector &items = elements_of(container);
        if (position < 0 || static_cast<size_t>(position) >= items.size())
        {
            throw_error("index " + std::to_string(position) +
                        " is out of range (size " +
                        std::to_string(items.size()) + ")");
        }
        return items[static_cast<size_t>(position)];
    }

    static value member_value(const value &container, const std::string &member)
    {
        if (const auto *object = container.get_if<object_ref>())
        {
            const auto &names = (*object)->field_names;
            for (size_t index = 0; index < names.size(); ++index)
            {
                if (names[index] == member)
                {
                    return (*object)->fields[index];
                }
            }
            throw_error("object " + (*object)->type_name +
                        " has no field `" + member + "`");
        }
        if (const auto *space = container.get_if<namespace_ref>())
        {
            const auto found = (*space)->members.find(member);
            if (found == (*space)->members.end())
            {
                throw_error((*space)->name + " has no member `" + member + "`");
            }
            return found->second;
        }
        if (member == "len")
        {
            return value{length_of(container)};
        }
        if (const auto *error = container.get_if<exception_value>())
        {
            if (member == "message")
            {
                return value{error->message};
            }
            if (member == "kind")
            {
                return value{std::string{exception_kind_name(error->kind)}};
            }
        }
        throw_error(std::string{"cannot read member `"} + member + "` of " +
                    type_name(container));
        return value{};
    }

    static value cast_value(const value &item, const type_spec &target)
    {
        switch (target.kind)
        {
        case ast::type_kind::Primitive:
            return cast_primitive(item, target.primitive);
        case ast::type_kind::Named:
        {
            const auto *object = item.get_if<object_ref>();
            if (object != nullptr && (*object)->type_name == target.name)
            {
                return item;
            }
            throw_error("cannot cast " + std::string{type_name(item)} + " to " +
                        target.name);
            return value{};
        }
        case ast::type_kind::Array:
        {
            auto converted = std::make_shared<sequence_object>();
            for (const value &element : elements_of(item))
            {
                converted->items.push_back(
                    cast_value(element, target.elements.front()));
            }
            return value{array_value{converted}};
        }
        case ast::type_kind::Tuple:
        {
            const value_vector &items = elements_of(item);
            if (items.size() != target.elements.size())
            {
                throw_error("cannot cast a " + std::to_string(items.size()) +
                            "-element value to a " +
                            std::to_string(target.elements.size()) +
                            "-element tuple");
                return value{};
            }
            auto converted = std::make_shared<sequence_object>();
            for (size_t index = 0; index < items.size(); ++index)
            {
                converted->items.push_back(
                    cast_value(items[index], target.elements[index]));
            }
            return value{tuple_value{converted}};
        }
        case ast::type_kind::Map:
        case ast::type_kind::Lambda:
            break;
        }
        throw_error("unsupported cast target");
        return value{};
    }

    static value cast_primitive(const value &item, ast::primitive_kind target)
    {
        switch (target)
        {
        case ast::primitive_kind::Int:
        {
            if (const auto *text = try_string(item))
            {
                return value{parse_integer(*text)};
            }
            if (is_numeric(item))
            {
                return value{as_integer(item)};
            }
            break;
        }
        case ast::primitive_kind::Float:
        {
            if (const auto *text = try_string(item))
            {
                return value{parse_number(*text)};
            }
            if (is_numeric(item))
            {
                return value{as_number(item)};
            }
            break;
        }
        case ast::primitive_kind::Bool:
        {
            if (const auto *text = try_string(item))
            {
                if (*text == "true")
                {
                    return value{true};
                }
                if (*text == "false")
                {
                    return value{false};
                }
                throw_error("cannot cast \"" + *text + "\" to bool");
            }
            return value{is_truthy(item)};
        }
        case ast::primitive_kind::String:
            return value{to_display_string(item)};
        case ast::primitive_kind::Character:
        {
            if (const auto *letter = item.get_if<char>())
            {
                return value{*letter};
            }
            if (const auto *text = try_string(item))
            {
                if (text->size() != 1)
                {
                    throw_error("cannot cast a " + std::to_string(text->size()) +
                                "-character string to character");
                }
                return value{text->front()};
            }
            if (is_numeric(item))
            {
                return value{static_cast<char>(as_integer(item) & 0xFF)};
            }
            break;
        }
        case ast::primitive_kind::Void:
            return value{};
        case ast::primitive_kind::Socket:
        case ast::primitive_kind::File:
        case ast::primitive_kind::Term:
            if (std::holds_alternative<io_ref>(item.data))
            {
                return item;
            }
            break;
        case ast::primitive_kind::Exception:
            if (std::holds_alternative<exception_value>(item.data))
            {
                return item;
            }
            return value{exception_value{exception_kind::Error, to_display_string(item)}};
        }
        throw_error(std::string{"cannot cast "} + type_name(item) +
                    " to the requested primitive type");
        return value{};
    }

    static int64_t parse_integer(const std::string &text)
    {
        char *end = nullptr;
        errno = 0;
        const long long parsed = std::strtoll(text.c_str(), &end, 10);
        if (errno != 0 || end == text.c_str())
        {
            throw_error("cannot cast \"" + text + "\" to int");
        }
        size_t consumed = static_cast<size_t>(end - text.c_str());
        while (consumed < text.size() &&
               std::isspace(static_cast<unsigned char>(text[consumed])) != 0)
        {
            ++consumed;
        }
        if (consumed != text.size())
        {
            throw_error("cannot cast \"" + text + "\" to int");
        }
        return static_cast<int64_t>(parsed);
    }

    static double parse_number(const std::string &text)
    {
        char *end = nullptr;
        errno = 0;
        const double parsed = std::strtod(text.c_str(), &end);
        if (errno != 0 || end == text.c_str())
        {
            throw_error("cannot cast \"" + text + "\" to float");
        }
        size_t consumed = static_cast<size_t>(end - text.c_str());
        while (consumed < text.size() &&
               std::isspace(static_cast<unsigned char>(text[consumed])) != 0)
        {
            ++consumed;
        }
        if (consumed != text.size())
        {
            throw_error("cannot cast \"" + text + "\" to float");
        }
        return parsed;
    }

    // -- arithmetic ---------------------------------------------------------

    static value binary_arithmetic(Opcode opcode, const value &left,
                                   const value &right)
    {
        if (opcode == Opcode::ADD)
        {
            if (try_string(left) != nullptr || try_string(right) != nullptr)
            {
                return value{to_display_string(left) + to_display_string(right)};
            }
            if (const auto *array = left.get_if<array_value>())
            {
                auto merged = std::make_shared<sequence_object>();
                merged->items = array->data->items;
                const value_vector &extra = elements_of(right);
                merged->items.insert(merged->items.end(), extra.begin(),
                                     extra.end());
                return value{array_value{merged}};
            }
        }
        if (opcode == Opcode::ADD || opcode == Opcode::SUB || opcode == Opcode::MUL)
        {
            if (const auto *left_simd = left.get_if<simd_value>())
            {
                const auto *right_simd = right.get_if<simd_value>();
                if (right_simd == nullptr)
                {
                    throw_error("SIMD arithmetic requires two SIMD operands");
                }
                ast::binary_op simd_op = ast::binary_op::Add;
                if (opcode == Opcode::SUB)
                {
                    simd_op = ast::binary_op::Sub;
                }
                else if (opcode == Opcode::MUL)
                {
                    simd_op = ast::binary_op::Mul;
                }
                return value{simd_binary_op(*left_simd, *right_simd, simd_op)};
            }
        }
        if (opcode == Opcode::MUL)
        {
            if (const auto *array = left.get_if<array_value>())
            {
                if (is_integral(right))
                {
                    return value{repeat_array(*array, as_integer(right))};
                }
            }
        }
        if (!is_numeric(left) || !is_numeric(right))
        {
            throw_error(std::string{"cannot apply arithmetic to "} +
                        type_name(left) + " and " + type_name(right));
        }
        if (is_integral(left) && is_integral(right))
        {
            const int64_t a = as_integer(left);
            const int64_t b = as_integer(right);
            switch (opcode)
            {
            case Opcode::ADD:
                return value{checked_int_add(a, b)};
            case Opcode::SUB:
                return value{checked_int_sub(a, b)};
            case Opcode::MUL:
                return value{checked_int_mul(a, b)};
            case Opcode::DIV:
                if (b == 0)
                {
                    throw_division_by_zero("integer division");
                }
                return value{a / b};
            case Opcode::MOD:
                if (b == 0)
                {
                    throw_division_by_zero("integer modulo");
                }
                return value{a % b};
            default:
                break;
            }
        }
        const double a = as_number(left);
        const double b = as_number(right);
        switch (opcode)
        {
        case Opcode::ADD:
            return value{a + b};
        case Opcode::SUB:
            return value{a - b};
        case Opcode::MUL:
            return value{a * b};
        case Opcode::DIV:
            if (b == 0.0)
            {
                throw_division_by_zero("float division");
            }
            return value{a / b};
        case Opcode::MOD:
            if (b == 0.0)
            {
                throw_division_by_zero("float modulo");
            }
            return value{std::fmod(a, b)};
        default:
            break;
        }
        throw_error("unsupported arithmetic opcode");
        return value{};
    }

    static value compare(Opcode opcode, const value &left, const value &right)
    {
        int ordering = 0;
        if (is_numeric(left) && is_numeric(right))
        {
            const double a = as_number(left);
            const double b = as_number(right);
            ordering = a < b ? -1 : (a > b ? 1 : 0);
        }
        else if (try_string(left) != nullptr && try_string(right) != nullptr)
        {
            const auto &a = string_data(left);
            const auto &b = string_data(right);
            ordering = a < b ? -1 : (a > b ? 1 : 0);
        }
        else
        {
            throw_error(std::string{"cannot order "} + type_name(left) +
                        " against " + type_name(right));
            return value{};
        }
        switch (opcode)
        {
        case Opcode::LT:
            return value{ordering < 0};
        case Opcode::GT:
            return value{ordering > 0};
        case Opcode::LE:
            return value{ordering <= 0};
        case Opcode::GE:
            return value{ordering >= 0};
        default:
            break;
        }
        throw_error("unsupported comparison opcode");
        return value{};
    }

    static value bitwise(Opcode opcode, const value &left, const value &right)
    {
        if (!is_integral(left) || !is_integral(right))
        {
            throw_error(std::string{"bitwise operators need integers, got "} +
                        type_name(left) + " and " + type_name(right));
        }
        const int64_t a = as_integer(left);
        const int64_t b = as_integer(right);
        switch (opcode)
        {
        case Opcode::BITWISE_AND:
            return value{a & b};
        case Opcode::BITWISE_OR:
            return value{a | b};
        case Opcode::BITWISE_XOR:
            return value{a ^ b};
        default:
            break;
        }
        throw_error("unsupported bitwise opcode");
        return value{};
    }

    // -- frame helpers ------------------------------------------------------

    static value pop(frame &current)
    {
        if (current.stack.empty())
        {
            throw_error("operand stack underflow in " + current.description);
            return value{};
        }
        value item = std::move(current.stack.back());
        current.stack.pop_back();
        return item;
    }

    uint8_t read_u8(frame &current) const
    {
        if (current.pc >= current.code.size())
        {
            throw_error("bytecode ended mid-instruction in " +
                        current.description);
        }
        return std::to_integer<uint8_t>(current.code[current.pc++]);
    }

    template <typename T>
    T read_scalar(frame &current) const
    {
        if (current.pc + sizeof(T) > current.code.size())
        {
            throw_error("bytecode ended mid-operand in " + current.description);
        }
        T item{};
        std::memcpy(&item, current.code.data() + current.pc, sizeof(T));
        current.pc += sizeof(T);
        return item;
    }

    std::string read_name(frame &current) const
    {
        const auto offset = read_scalar<uint32_t>(current);
        const auto length = read_scalar<uint32_t>(current);
        if (offset > image_.strings.size() ||
            length > image_.strings.size() - offset)
        {
            throw_error("string operand is outside the string table");
        }
        return std::string{image_.strings.substr(offset, length)};
    }

    type_spec read_type(frame &current) const
    {
        type_spec target;
        target.kind = static_cast<ast::type_kind>(read_u8(current));
        switch (target.kind)
        {
        case ast::type_kind::Primitive:
            target.primitive = static_cast<ast::primitive_kind>(read_u8(current));
            break;
        case ast::type_kind::Named:
            target.name = read_name(current);
            break;
        case ast::type_kind::Array:
            target.elements.push_back(read_type(current));
            break;
        case ast::type_kind::Tuple:
        {
            const auto count = read_scalar<uint32_t>(current);
            for (uint32_t index = 0; index < count; ++index)
            {
                target.elements.push_back(read_type(current));
            }
            break;
        }
        case ast::type_kind::Map:
            target.elements.push_back(read_type(current));
            target.elements.push_back(read_type(current));
            break;
        case ast::type_kind::Lambda:
        {
            const auto count = read_scalar<uint32_t>(current);
            for (uint32_t index = 0; index < count; ++index)
            {
                target.elements.push_back(read_type(current));
            }
            target.elements.push_back(read_type(current));
            break;
        }
        }
        return target;
    }

    /// Resolve @p name against the frame's locals, then the package globals.
    value load_name(const frame &current, const std::string &name)
    {
        if (!current.global_scope)
        {
            const auto found = current.locals.find(name);
            if (found != current.locals.end())
            {
                return found->second;
            }
        }
        value item;
        if (globals_.try_get(name, item))
        {
            return item;
        }
        throw_error("undefined name `" + name + "`");
        return value{};
    }

    /// Assign @p name, preferring an existing local, then an existing global.
    void store_name(frame &current, const std::string &name, value &item)
    {
        if (current.global_scope)
        {
            globals_.set(name, item);
            return;
        }
        const auto found = current.locals.find(name);
        if (found != current.locals.end())
        {
            found->second = std::move(item);
            return;
        }
        if (globals_.assign_existing(name, item))
        {
            return;
        }
        current.locals.emplace(name, std::move(item));
    }

    // -- calls --------------------------------------------------------------

    value call_function(const function_def &definition, value_vector &arguments)
    {
        if (arguments.size() != definition.parameters.size())
        {
            throw_error(definition.name + " expects " +
                        std::to_string(definition.parameters.size()) +
                        " argument(s), got " + std::to_string(arguments.size()));
            return value{};
        }

        frame invocation;
        invocation.description = "function " + definition.name;
        invocation.code = definition.code;
        invocation.debug_map = std::span<const debug_loc_entry>(definition.debug_map);
        for (const std::string &parameter : definition.parameters)
        {
            invocation.locals.emplace(parameter, value{});
        }
        for (value &argument : arguments)
        {
            invocation.stack.push_back(std::move(argument));
        }

        static thread_local size_t depth = 0;
        if (++depth > call_depth_limit)
        {
            --depth;
            throw_error("call stack overflow in " + definition.name);
            return value{};
        }
        push_call_stack(invocation);
        const value result = execute(invocation);
        pop_call_stack();
        --depth;
        return result;
    }

    /// Register a constructor for a `DEFINE_OBJECT` declaration.
    void define_object_type(frame &current, const std::string &name,
                            const std::vector<std::string> &fields)
    {
        auto constructor = std::make_shared<builtin_object>();
        constructor->name = name;
        constructor->invoke = [name, fields](virtual_machine &,
                                             value_vector &arguments) {
            if (arguments.size() != fields.size())
            {
                throw_error(name + " expects " + std::to_string(fields.size()) +
                            " field(s), got " +
                            std::to_string(arguments.size()));
            }
            auto instance = std::make_shared<object_instance>();
            instance->type_name = name;
            instance->field_names = fields;
            instance->fields = arguments;
            return value{instance};
        };
        value constructor_value{constructor};
        store_name(current, name, constructor_value);
    }

    static type_spec from_decoded_type(const jit::decoded_type &source)
    {
        type_spec target;
        target.kind = source.kind;
        target.primitive = source.primitive;
        target.name = source.name;
        target.elements.reserve(source.elements.size());
        for (const jit::decoded_type &element : source.elements)
        {
            target.elements.push_back(from_decoded_type(element));
        }
        return target;
    }

    std::shared_ptr<jit::compiled_unit> get_jit_unit(std::span<const std::byte> source);

    value execute_jit(frame &current);

    // -- interpreter --------------------------------------------------------

    /// Run @p current until RET or the end of its code blob.
    /// @return The returned value, or null for an initializer.
    value execute(frame &current)
    {
        jit::execution_profile &profile =
            jit::profile_registry::instance().get(current.code);

        if (jit::jit_enabled())
        {
            const unsigned warmup = jit::jit_warmup_invocations();
            if (warmup > 0 && profile.interpret_runs < warmup)
            {
                profile.note_interpret_run();
                const value result = execute_interpret(current, profile);
                jit::branch_predictor::instance().seed_from_profile(profile);
                if (profile.interpret_runs < warmup)
                {
                    return result;
                }
            }
            return execute_jit(current);
        }

        return execute_interpret(current, profile);
    }

    value execute_interpret(frame &current, jit::execution_profile &profile)
    {
        vm_dispatch_scope dispatch{*this};
        jit::branch_predictor &predictor = jit::branch_predictor::instance();
        while (current.pc < current.code.size())
        {
            const size_t instruction_pc = current.pc;
            sync_call_stack_pc(instruction_pc);
            profile.record_block(instruction_pc);

            const auto opcode = static_cast<Opcode>(read_u8(current));
                switch (opcode)
                {
                case Opcode::PUSH_INT:
                    current.stack.push_back(value{read_scalar<int64_t>(current)});
                    break;
                case Opcode::PUSH_FLOAT:
                    current.stack.push_back(value{read_scalar<double>(current)});
                    break;
                case Opcode::PUSH_STRING:
                    current.stack.push_back(value{read_name(current)});
                    break;
                case Opcode::PUSH_CHAR:
                    current.stack.push_back(
                        value{static_cast<char>(read_u8(current))});
                    break;
                case Opcode::PUSH_BOOL:
                    current.stack.push_back(value{read_u8(current) != 0});
                    break;
                case Opcode::PUSH_NULL:
                    current.stack.push_back(value{});
                    break;
                case Opcode::PUSH_REGEX:
                    current.stack.push_back(value{regex_value{read_name(current)}});
                    break;
                case Opcode::PUSH_ENUM:
                {
                    std::string type = read_name(current);
                    std::string member = read_name(current);
                    current.stack.push_back(
                        value{enum_value{std::move(type), std::move(member)}});
                    break;
                }
                case Opcode::PUSH_FUNC:
                {
                    const std::string name = read_name(current);
                    const auto found = functions_.find(name);
                    if (found == functions_.end())
                    {
                        throw_error("unknown function `" + name + "`");
                    }
                    current.stack.push_back(value{function_value{found->second}});
                    break;
                }
                case Opcode::POP:
                    pop(current);
                    break;
                case Opcode::DUP:
                {
                    if (current.stack.empty())
                    {
                        throw_error("operand stack underflow in " +
                                    current.description);
                    }
                    current.stack.push_back(current.stack.back());
                    break;
                }
                case Opcode::SWAP:
                {
                    if (current.stack.size() < 2)
                    {
                        throw_error("operand stack underflow in " +
                                    current.description);
                    }
                    std::iter_swap(current.stack.end() - 1,
                                   current.stack.end() - 2);
                    break;
                }
                case Opcode::LOAD:
                    current.stack.push_back(
                        load_name(current, read_name(current)));
                    break;
                case Opcode::STORE:
                {
                    const std::string name = read_name(current);
                    value item = pop(current);
                    store_name(current, name, item);
                    break;
                }
                case Opcode::ADD:
                case Opcode::SUB:
                case Opcode::MUL:
                case Opcode::DIV:
                case Opcode::MOD:
                {
                    const value right = pop(current);
                    const value left = pop(current);
                    current.stack.push_back(
                        binary_arithmetic(opcode, left, right));
                    break;
                }
                case Opcode::NEG:
                {
                    const value operand = pop(current);
                    if (is_integral(operand))
                    {
                        current.stack.push_back(value{checked_int_neg(as_integer(operand))});
                    }
                    else
                    {
                        current.stack.push_back(value{-as_number(operand)});
                    }
                    break;
                }
                case Opcode::EQ:
                case Opcode::NE:
                {
                    const value right = pop(current);
                    const value left = pop(current);
                    const bool equal = values_equal(left, right);
                    current.stack.push_back(
                        value{opcode == Opcode::EQ ? equal : !equal});
                    break;
                }
                case Opcode::LT:
                case Opcode::GT:
                case Opcode::LE:
                case Opcode::GE:
                {
                    const value right = pop(current);
                    const value left = pop(current);
                    current.stack.push_back(compare(opcode, left, right));
                    break;
                }
                case Opcode::NOT:
                    current.stack.push_back(value{!is_truthy(pop(current))});
                    break;
                case Opcode::BITWISE_AND:
                case Opcode::BITWISE_OR:
                case Opcode::BITWISE_XOR:
                {
                    const value right = pop(current);
                    const value left = pop(current);
                    current.stack.push_back(bitwise(opcode, left, right));
                    break;
                }
                case Opcode::BITWISE_NOT:
                {
                    const value operand = pop(current);
                    if (!is_integral(operand))
                    {
                        throw_error(std::string{"`~` needs an integer, got "} +
                                    type_name(operand));
                    }
                    current.stack.push_back(value{~as_integer(operand)});
                    break;
                }
                case Opcode::JMP:
                    current.pc = jump_target(current, read_scalar<uint32_t>(current));
                    break;
                case Opcode::JMP_IF_FALSE:
                {
                    const auto target = read_scalar<uint32_t>(current);
                    const size_t branch_pc = instruction_pc;
                    const size_t fallthrough_pc = current.pc;
                    const size_t jump_pc = jump_target(current, target);
                    const bool predicted =
                        predictor.predict(branch_pc, jump_pc, fallthrough_pc);
                    (void)predicted;
                    const bool actually_taken = !is_truthy(pop(current));
                    predictor.train(branch_pc, actually_taken, jump_pc);
                    profile.record_branch(branch_pc, actually_taken);
                    if (actually_taken)
                    {
                        current.pc = jump_pc;
                    }
                    break;
                }
                case Opcode::JMP_IF_TRUE:
                {
                    const auto target = read_scalar<uint32_t>(current);
                    const size_t branch_pc = instruction_pc;
                    const size_t fallthrough_pc = current.pc;
                    const size_t jump_pc = jump_target(current, target);
                    const bool predicted =
                        predictor.predict(branch_pc, jump_pc, fallthrough_pc);
                    (void)predicted;
                    const bool actually_taken = is_truthy(pop(current));
                    predictor.train(branch_pc, actually_taken, jump_pc);
                    profile.record_branch(branch_pc, actually_taken);
                    if (actually_taken)
                    {
                        current.pc = jump_pc;
                    }
                    break;
                }
                case Opcode::CALL:
                {
                    const uint8_t argument_count = read_u8(current);
                    value_vector arguments(argument_count);
                    for (size_t index = argument_count; index > 0; --index)
                    {
                        arguments[index - 1] = pop(current);
                    }
                    const value callee = pop(current);
                    current.stack.push_back(call_value(callee, arguments));
                    break;
                }
                case Opcode::RET:
                    return pop(current);
                case Opcode::HALT:
                    return value{};
                case Opcode::MAKE_ARRAY:
                case Opcode::MAKE_TUPLE:
                {
                    const auto count = read_scalar<uint32_t>(current);
                    auto items = std::make_shared<sequence_object>();
                    items->items.resize(count);
                    for (size_t index = count; index > 0; --index)
                    {
                        items->items[index - 1] = pop(current);
                    }
                    if (opcode == Opcode::MAKE_ARRAY)
                    {
                        current.stack.push_back(value{array_value{items}});
                    }
                    else
                    {
                        current.stack.push_back(value{tuple_value{items}});
                    }
                    break;
                }
                case Opcode::MAKE_MAP:
                {
                    const auto count = read_scalar<uint32_t>(current);
                    auto map = std::make_shared<map_object>();
                    for (size_t index = count; index > 0; --index)
                    {
                        value item = pop(current);
                        value key = pop(current);
                        map_store_entry(*map, key, item);
                    }
                    current.stack.push_back(value{map_value{std::move(map)}});
                    break;
                }
                case Opcode::INDEX_GET:
                {
                    const value index = pop(current);
                    const value container = pop(current);
                    current.stack.push_back(index_value(container, index));
                    break;
                }
                case Opcode::MEMBER_GET:
                {
                    const std::string member = read_name(current);
                    const value container = pop(current);
                    current.stack.push_back(member_value(container, member));
                    break;
                }
                case Opcode::UNPACK:
                {
                    const uint8_t count = read_u8(current);
                    const value aggregate = pop(current);
                    const value_vector &items = elements_of(aggregate);
                    if (items.size() < count)
                    {
                        throw_error("cannot destructure " +
                                    std::to_string(items.size()) +
                                    " value(s) into " + std::to_string(count) +
                                    " target(s)");
                    }
                    // Element 0 must end up on top for source-order binding.
                    for (size_t index = count; index > 0; --index)
                    {
                        current.stack.push_back(items[index - 1]);
                    }
                    break;
                }
                case Opcode::CAST:
                {
                    const value operand = pop(current);
                    const type_spec target = read_type(current);
                    current.stack.push_back(cast_value(operand, target));
                    break;
                }
                case Opcode::ALLOC:
                {
                    const auto count = read_scalar<uint32_t>(current);
                    auto buffer = std::make_shared<buffer_object>();
                    buffer->items.resize(count);
                    for (size_t index = count; index > 0; --index)
                    {
                        buffer->items[index - 1] = pop(current);
                    }
                    buffer->capacity = as_integer(pop(current));
                    if (buffer->capacity < 0)
                    {
                        throw_error("alloc capacity must not be negative");
                    }
                    current.stack.push_back(value{buffer});
                    break;
                }
                case Opcode::CLONE_OBJECT:
                    current.stack.push_back(clone_for_return(pop(current)));
                    break;
                case Opcode::MAKE_SIMD:
                {
                    const value array_value_item = pop(current);
                    const auto *array = array_value_item.get_if<array_value>();
                    if (array == nullptr)
                    {
                        throw_error("MAKE_SIMD expects an array operand");
                    }
                    current.stack.push_back(value{make_simd_from_array(*array)});
                    break;
                }
                case Opcode::SIMD_TO_ARRAY:
                {
                    const value simd_item = pop(current);
                    const auto *simd = simd_item.get_if<simd_value>();
                    if (simd == nullptr)
                    {
                        throw_error("SIMD_TO_ARRAY expects a SIMD operand");
                    }
                    current.stack.push_back(value{simd_to_array(*simd)});
                    break;
                }
                case Opcode::FREE:
                {
                    const std::string name = read_name(current);
                    const value target = load_name(current, name);
                    if (const auto *buffer = target.get_if<buffer_ref>())
                    {
                        release_buffer(*buffer, name);
                    }
                    current.stack.push_back(value{});
                    break;
                }
                case Opcode::PIPE_INSERT:
                {
                    const std::string name = read_name(current);
                    value item = pop(current);
                    expect_pipe(load_name(current, name), name)->insert(item);
                    current.stack.push_back(value{});
                    break;
                }
                case Opcode::PIPE_EXTRACT:
                {
                    const std::string name = read_name(current);
                    current.stack.push_back(
                        expect_pipe(load_name(current, name), name)->extract());
                    break;
                }
                case Opcode::DEFINE_ENUM:
                {
                    const std::string name = read_name(current);
                    const auto count = read_scalar<uint32_t>(current);
                    std::vector<std::string> members;
                    members.reserve(count);
                    for (uint32_t index = 0; index < count; ++index)
                    {
                        members.push_back(read_name(current));
                    }
                    enums_[name] = std::move(members);
                    break;
                }
                case Opcode::DEFINE_OBJECT:
                {
                    const std::string name = read_name(current);
                    const auto count = read_scalar<uint32_t>(current);
                    std::vector<std::string> fields;
                    fields.reserve(count);
                    for (uint32_t index = 0; index < count; ++index)
                    {
                        fields.push_back(read_name(current));
                    }
                    define_object_type(current, name, std::move(fields));
                    break;
                }
                case Opcode::LOCK_CREATE:
                {
                    const std::string name = read_name(current);
                    auto guard = std::make_shared<lock_object>();
                    guard->name = name;
                    value guard_value{guard};
                    store_name(current, name, guard_value);
                    break;
                }
                case Opcode::LOCK_ACQUIRE:
                {
                    const std::string name = read_name(current);
                    expect_lock(load_name(current, name), name)->acquire();
                    break;
                }
                case Opcode::LOCK_RELEASE:
                {
                    const std::string name = read_name(current);
                    expect_lock(load_name(current, name), name)->release();
                    break;
                }
                case Opcode::MONITOR_ENTER:
                {
                    const auto handler = read_scalar<uint32_t>(current);
                    current.monitors.push_back(
                        monitor_state{current.pc, jump_target(current, handler),
                                      current.stack.size()});
                    break;
                }
                case Opcode::MONITOR_EXIT:
                    if (!current.monitors.empty())
                    {
                        current.monitors.pop_back();
                    }
                    break;
                }

            if (runtime_fault_pending())
            {
                if (dispatch_runtime_trap(current, instruction_pc))
                {
                    continue;
                }
                return value{};
            }
        }
        return value{};
    }

    size_t jump_target(const frame &current, uint32_t target) const
    {
        if (target > current.code.size())
        {
            throw_error("jump target " + std::to_string(target) +
                        " is outside " + current.description);
        }
        return target;
    }

    static pipe_ref expect_pipe(const value &item, const std::string &name)
    {
        if (const auto *channel = item.get_if<pipe_ref>())
        {
            return *channel;
        }
        throw_error("`" + name + "` is not a pipe (it is " +
                    type_name(item) + ")");
        static const pipe_ref k_empty{};
        return k_empty;
    }

    static lock_ref expect_lock(const value &item, const std::string &name)
    {
        if (const auto *guard = item.get_if<lock_ref>())
        {
            return *guard;
        }
        throw_error("`" + name + "` is not a lock (it is " +
                    type_name(item) + ")");
        static const lock_ref k_empty{};
        return k_empty;
    }
};

} // namespace munx::vm

#include "jit/jit_compiler_impl.hpp"

namespace munx::vm
{

/// Load and execute the `.mxb` image at @p path.
/// @param arguments Values exposed to the program as `argv`.
/// @return Process exit status.
inline int run_bytecode_file(const std::filesystem::path &path,
                             const std::vector<std::string> &arguments)
{
#if MUNX_VM_HAS_SOCKETS
    munx::winsock_session winsock;
    if (!winsock.ok())
    {
        std::cerr << "munx: failed to initialize Winsock\n";
        return 1;
    }
#endif
#if MUNX_VM_HAS_NAMED_PIPES
    pipe_hub::client::session hub_session;
#endif
    virtual_machine machine{load_program(path), arguments};
    return machine.run();
}

} // namespace munx::vm
