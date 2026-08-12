#pragma once

#include "errors.hpp"
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

/// Runtime value model for the munx stack virtual machine.
namespace munx::vm
{

class virtual_machine;
struct value;
struct function_def;

struct sequence_object;
struct object_instance;
struct buffer_object;
struct builtin_object;
struct namespace_object;
struct io_object;
struct pipe_object;
struct lock_object;
struct thread_object;
struct library_object;
struct foreign_callable_object;

struct map_object;

using value_vector = std::vector<value>;
using sequence_ref = std::shared_ptr<sequence_object>;
using object_ref = std::shared_ptr<object_instance>;
using buffer_ref = std::shared_ptr<buffer_object>;
using builtin_ref = std::shared_ptr<builtin_object>;
using namespace_ref = std::shared_ptr<namespace_object>;
using io_ref = std::shared_ptr<io_object>;
using pipe_ref = std::shared_ptr<pipe_object>;
using lock_ref = std::shared_ptr<lock_object>;
using thread_ref = std::shared_ptr<thread_object>;
using library_ref = std::shared_ptr<library_object>;
using foreign_callable_ref = std::shared_ptr<foreign_callable_object>;
using map_ref = std::shared_ptr<map_object>;

/// `Enum::Member` as produced by PUSH_ENUM.
struct enum_value
{
    std::string type_name;
    std::string member;
};

/// Regex literal; the pattern is compiled lazily by the builtins that use it.
struct regex_value
{
    std::string pattern;
};

/// `in` / `out`, the direction arguments accepted by `open` and `pipe`.
struct mode_value
{
    std::string name;
};

/// Source location anchor emitted by the compiler for stack traces.
struct debug_loc_entry
{
    uint32_t pc{0};
    uint32_t line{0};
    uint32_t column{0};
    std::string file;
};

/// One frame in a munx runtime stack trace.
struct stack_frame_info
{
    std::string label;
    size_t pc{0};
    uint32_t line{0};
    uint32_t column{0};
    std::string file;
    std::span<const debug_loc_entry> debug_map{};
};

/// Active call stack for the current OS thread (managed by the VM).
struct call_stack_state
{
    std::vector<stack_frame_info> frames;

    void push(stack_frame_info frame) { frames.push_back(std::move(frame)); }

    void pop()
    {
        if (!frames.empty())
        {
            frames.pop_back();
        }
    }

    void update_pc(size_t pc)
    {
        if (!frames.empty())
        {
            frames.back().pc = pc;
        }
    }
};

inline thread_local call_stack_state *active_call_stack = nullptr;

inline std::vector<stack_frame_info> snapshot_call_stack()
{
    if (active_call_stack != nullptr)
    {
        return active_call_stack->frames;
    }
    return {};
};

enum class exception_kind : uint8_t
{
    Error,
    DivisionByZero,
    Overflow,
};

struct exception_value
{
    exception_kind kind{exception_kind::Error};
    std::string message;
};

[[nodiscard]] inline std::string_view exception_kind_name(exception_kind kind) noexcept
{
    switch (kind)
    {
    case exception_kind::Error:
        return "error";
    case exception_kind::DivisionByZero:
        return "division_by_zero";
    case exception_kind::Overflow:
        return "overflow";
    }
    return "error";
}

/// String value; the text heap cell is shared by reference across copies.
struct string_value
{
    std::shared_ptr<std::string> data;
};

/// Array literal or `MAKE_ARRAY` result. Elements are shared by reference.
struct array_value
{
    sequence_ref data;
};

/// Lane element type for SIMD vectors.
enum class simd_lane_kind : uint8_t
{
    Int,
    Float,
    Char,
    Bool,
};

/// SIMD vector of homogeneous primitive lanes (AVX2-backed operations).
struct simd_value
{
    simd_lane_kind kind{simd_lane_kind::Int};
    size_t length{0};
    std::vector<int32_t> i32; ///< int / char / bool lanes (char and bool widened).
    std::vector<float> f32;   ///< float lanes.
};

/// Tuple literal or `MAKE_TUPLE` result.
struct tuple_value
{
    sequence_ref data;
};

/// Handle to a value-keyed map; @ref map_object is defined after @ref value.
struct map_value
{
    map_ref data;
};

/// A compiled function or lambda; the definition lives in the program image.
struct function_value
{
    const function_def *def;
};

using value_data = std::variant<
    std::monostate,
    bool,
    int64_t,
    double,
    char,
    string_value,
    regex_value,
    enum_value,
    mode_value,
    exception_value,
    array_value,
    simd_value,
    tuple_value,
    map_value,
    object_ref,
    buffer_ref,
    function_value,
    builtin_ref,
    namespace_ref,
    io_ref,
    pipe_ref,
    lock_ref,
    thread_ref,
    library_ref,
    foreign_callable_ref>;

/// A dynamically typed munx runtime value.
struct value
{
    value_data data{};

    value() = default;
    template <typename T>
        requires(!std::is_same_v<std::decay_t<T>, value>)
    value(T payload) : data(std::move(payload))
    {}

    /// Construct a string value from a UTF-8 payload (heap-allocated, ref-counted).
    value(const std::string &text)
        : data(string_value{std::make_shared<std::string>(text)})
    {}

    template <typename T>
    const T *get_if() const noexcept
    {
        return std::get_if<T>(&data);
    }

    bool is_null() const noexcept
    {
        return std::holds_alternative<std::monostate>(data);
    }
};

/// Map entries keyed by arbitrary munx values (lookup uses @ref values_equal).
struct map_object
{
    std::vector<std::pair<value, value>> entries;
};

/// Backing store shared by array and tuple values.
struct sequence_object
{
    value_vector items;
};

/// Instance of a `DEFINE_OBJECT` type.
struct object_instance
{
    std::string type_name;
    std::vector<std::string> field_names;
    value_vector fields;
};

/// `alloc [capacity] [values…]` result; `delete` / `free` mark it released.
struct buffer_object
{
    int64_t capacity{0};
    value_vector items;
    bool released{false};
};

/// Captured munx runtime fault (delivered to `trap` handlers when applicable).
struct runtime_fault
{
    bool active{false};
    value payload{};
    std::string message{};
    std::vector<stack_frame_info> stack_trace{};

    void clear() noexcept
    {
        active = false;
        payload = value{};
        message.clear();
        stack_trace.clear();
    }
};

inline thread_local runtime_fault tls_runtime_fault{};
/// Non-zero while `execute` / `execute_jit` is dispatching opcodes.
inline thread_local unsigned vm_dispatch_depth{0};

class virtual_machine;

inline thread_local virtual_machine *active_virtual_machine{nullptr};

inline void capture_runtime_fault(const std::string &message,
                                  exception_kind kind = exception_kind::Error)
{
    tls_runtime_fault.active = true;
    tls_runtime_fault.payload = value{exception_value{kind, message}};
    tls_runtime_fault.message =
        std::get<exception_value>(tls_runtime_fault.payload.data).message;
    tls_runtime_fault.stack_trace = snapshot_call_stack();
}

[[nodiscard]] inline bool runtime_fault_pending() noexcept
{
    return tls_runtime_fault.active;
}

/// Record a runtime fault. When called inside VM dispatch, returns to the caller
/// so the execute loop can deliver traps or propagate the fault.
inline void throw_error(const std::string &message)
{
    capture_runtime_fault(message);
    if (vm_dispatch_depth > 0)
    {
        return;
    }
    std::fprintf(stderr, "runtime error: %s\n", tls_runtime_fault.message.c_str());
    std::abort();
}

inline void throw_division_by_zero(const std::string &context)
{
    capture_runtime_fault(context, exception_kind::DivisionByZero);
    if (vm_dispatch_depth > 0)
    {
        return;
    }
    std::fprintf(stderr, "division by zero: %s\n", context.c_str());
    std::abort();
}

inline void throw_overflow(const std::string &context)
{
    capture_runtime_fault(context, exception_kind::Overflow);
    if (vm_dispatch_depth > 0)
    {
        return;
    }
    std::fprintf(stderr, "overflow: %s\n", context.c_str());
    std::abort();
}

/// RAII: marks nested VM opcode dispatch for fault propagation.
struct vm_dispatch_scope
{
    explicit vm_dispatch_scope(virtual_machine &machine)
    {
        ++vm_dispatch_depth;
        active_virtual_machine = &machine;
    }

    ~vm_dispatch_scope()
    {
        if (vm_dispatch_depth > 0)
        {
            --vm_dispatch_depth;
        }
        if (vm_dispatch_depth == 0)
        {
            active_virtual_machine = nullptr;
        }
    }
};

inline const char *type_name(const value &item);

/// Release @p buffer when its reference count is exactly one.
inline void release_buffer(const buffer_ref &buffer, const std::string &name)
{
    if (buffer->released)
    {
        throw_error("buffer `" + name + "` has already been released");
    }
    if (buffer.use_count() > 1)
    {
        throw_error("buffer `" + name + "` still has " +
                    std::to_string(buffer.use_count() - 1) + " active reference(s)");
    }
    buffer->released = true;
    buffer->items.clear();
}

/// Native function exposed to munx code by the prelude.
struct builtin_object
{
    std::string name;
    std::function<value(virtual_machine &, value_vector &)> invoke;
};

/// Named bag of members, used for builtin namespaces such as `process`.
struct namespace_object
{
    std::string name;
    std::unordered_map<std::string, value> members;
};

/// A `socket`, `file`, or `term` handle produced by `open`.
struct io_object
{
    enum class handle_kind
    {
        File,
        Term,
        Socket
    };

    handle_kind kind{handle_kind::Term};
    std::string description;
    bool readable{false};
    bool writable{false};
    bool closed{false};
    std::mutex mutex;
    std::unique_ptr<std::fstream> file; ///< File handles.
    std::istream *term_in{nullptr};     ///< Term handles.
    std::ostream *term_out{nullptr};    ///< Term handles.
    intptr_t descriptor{-1};          ///< Socket handles (Winsock-safe storage).
};

/// Blocking named FIFO endpoint; see @ref munx::vm::pipe_object in `vm_pipe.hpp`.
struct pipe_object;

/// `lock` / `acquire` / `release`. Ownership is not tied to a thread, because
/// munx allows a lock to be released by a different function than acquired it.
struct lock_object
{
    std::string name;
    std::mutex mutex;
    std::condition_variable released;
    bool held{false};

    void acquire()
    {
        std::unique_lock<std::mutex> guard{mutex};
        released.wait(guard, [this] { return !held; });
        held = true;
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> guard{mutex};
            held = false;
        }
        released.notify_one();
    }
};

/// Handle returned by `thread(...)` and consumed by `join [...]`.
struct thread_object
{
    std::thread worker;
    std::mutex mutex;
    bool joined{false};

    void join()
    {
        std::lock_guard<std::mutex> guard{mutex};
        if (!joined && worker.joinable())
        {
            worker.join();
            joined = true;
        }
    }
};

/// Handle from `load_library(path)`; closed by `close_library`.
struct library_object
{
    std::string path;
    void *handle{nullptr};
    bool closed{false};
};

/// Symbol from `resolve_callable(library, name)`; callable with integer args.
struct foreign_callable_object
{
    library_ref library;
    std::string symbol;
    void *fn{nullptr};
};

inline const char *type_name(const value &item)
{
    struct namer
    {
        const char *operator()(std::monostate) const { return "null"; }
        const char *operator()(bool) const { return "bool"; }
        const char *operator()(int64_t) const { return "int"; }
        const char *operator()(double) const { return "float"; }
        const char *operator()(char) const { return "character"; }
        const char *operator()(const string_value &) const { return "string"; }
        const char *operator()(const regex_value &) const { return "regex"; }
        const char *operator()(const enum_value &) const { return "enum member"; }
        const char *operator()(const mode_value &) const { return "io mode"; }
        const char *operator()(const exception_value &) const { return "exception"; }
        const char *operator()(const array_value &) const { return "array"; }
        const char *operator()(const simd_value &) const { return "simd"; }
        const char *operator()(const tuple_value &) const { return "tuple"; }
        const char *operator()(const map_value &) const { return "map"; }
        const char *operator()(const object_ref &) const { return "object"; }
        const char *operator()(const buffer_ref &) const { return "buffer"; }
        const char *operator()(const function_value &) const { return "function"; }
        const char *operator()(const builtin_ref &) const { return "builtin"; }
        const char *operator()(const namespace_ref &) const { return "namespace"; }
        const char *operator()(const io_ref &) const { return "io handle"; }
        const char *operator()(const pipe_ref &) const { return "pipe"; }
        const char *operator()(const lock_ref &) const { return "lock"; }
        const char *operator()(const thread_ref &) const { return "thread"; }
        const char *operator()(const library_ref &) const { return "library"; }
        const char *operator()(const foreign_callable_ref &) const
        {
            return "foreign callable";
        }
    };
    return std::visit(namer{}, item.data);
}

/// @return A ref-counted string value owning @p text.
inline string_value make_string(const std::string &text)
{
    return string_value{std::make_shared<std::string>(text)};
}

/// @return Pointer to the UTF-8 text, or null when @p item is not a string.
inline const std::string *try_string(const value &item) noexcept
{
    if (const auto *text = item.get_if<string_value>())
    {
        return text->data.get();
    }
    return nullptr;
}

/// @return Pointer to SIMD payload, or null when @p item is not a SIMD vector.
inline const simd_value *try_simd(const value &item) noexcept
{
    return item.get_if<simd_value>();
}

inline const map_value *try_map(const value &item) noexcept
{
    return item.get_if<map_value>();
}

inline map_ref expect_map(const value &item, std::string_view context)
{
    if (const auto *map = try_map(item))
    {
        return map->data;
    }
    throw_error(std::string{context} + " expected a map, got " + type_name(item));
    static map_ref k_empty = std::make_shared<map_object>();
    return k_empty;
}

/// @return The UTF-8 text of @p item.
/// @throws runtime_exception when @p item is not a string.
inline const std::string &string_data(const value &item)
{
    if (const auto *text = try_string(item))
    {
        return *text;
    }
    throw_error(std::string{"expected a string, got "} + type_name(item));
    static const std::string k_empty;
    return k_empty;
}

/// Deep-copy a user object for by-value returns.
inline object_ref clone_object(const object_ref &object)
{
    auto copy = std::make_shared<object_instance>();
    copy->type_name = object->type_name;
    copy->field_names = object->field_names;
    copy->fields = object->fields;
    return copy;
}

/// Clone reference-type return values according to the munx value model:
/// user objects are copied; everything else is returned as-is.
inline value clone_for_return(const value &item)
{
    if (const auto *object = item.get_if<object_ref>())
    {
        return value{clone_object(*object)};
    }
    return item;
}

/// Format @p number the way munx renders floats: always with a fraction or
/// exponent so `cast[string](134.0)` reads back as a float.
inline std::string format_float(double number)
{
    if (std::isnan(number))
    {
        return "nan";
    }
    if (std::isinf(number))
    {
        return number < 0 ? "-inf" : "inf";
    }
    std::ostringstream text;
    text << std::setprecision(15) << number;
    std::string formatted = text.str();
    if (formatted.find_first_of(".eE") == std::string::npos)
    {
        formatted += ".0";
    }
    return formatted;
}

/// @return @p item rendered as munx source-like text.
std::string to_display_string(const value &item);

inline std::string sequence_to_string(const value_vector &items, char open,
                                      char close)
{
    std::string text{open};
    for (size_t index = 0; index < items.size(); ++index)
    {
        if (index != 0)
        {
            text += ',';
        }
        text += to_display_string(items[index]);
    }
    text += close;
    return text;
}

inline std::string to_display_string(const value &item)
{
    struct printer
    {
        std::string operator()(std::monostate) const { return "null"; }
        std::string operator()(bool flag) const { return flag ? "true" : "false"; }
        std::string operator()(int64_t number) const { return std::to_string(number); }
        std::string operator()(double number) const { return format_float(number); }
        std::string operator()(char letter) const { return std::string(1, letter); }
        std::string operator()(const string_value &text) const { return *text.data; }
        std::string operator()(const regex_value &regex) const { return regex.pattern; }
        std::string operator()(const enum_value &member) const
        {
            return member.type_name + "::" + member.member;
        }
        std::string operator()(const mode_value &mode) const { return mode.name; }
        std::string operator()(const exception_value &error) const { return error.message; }
        std::string operator()(const array_value &array) const
        {
            return sequence_to_string(array.data->items, '[', ']');
        }
        std::string operator()(const simd_value &simd) const
        {
            std::string kind_name;
            switch (simd.kind)
            {
            case simd_lane_kind::Int:
                kind_name = "int";
                break;
            case simd_lane_kind::Float:
                kind_name = "float";
                break;
            case simd_lane_kind::Char:
                kind_name = "char";
                break;
            case simd_lane_kind::Bool:
                kind_name = "bool";
                break;
            }
            std::string text = "simd<" + kind_name + ">[";
            for (size_t index = 0; index < simd.length; ++index)
            {
                if (index != 0)
                {
                    text += ',';
                }
                if (simd.kind == simd_lane_kind::Float)
                {
                    text += format_float(static_cast<double>(simd.f32[index]));
                }
                else if (simd.kind == simd_lane_kind::Char)
                {
                    text += std::string(1, static_cast<char>(simd.i32[index]));
                }
                else if (simd.kind == simd_lane_kind::Bool)
                {
                    text += simd.i32[index] != 0 ? "true" : "false";
                }
                else
                {
                    text += std::to_string(simd.i32[index]);
                }
            }
            text += ']';
            return text;
        }
        std::string operator()(const tuple_value &tuple) const
        {
            return sequence_to_string(tuple.data->items, '{', '}');
        }
        std::string operator()(const map_value &map) const
        {
            std::string text = "map{";
            bool first = true;
            for (const auto &[key, entry] : map.data->entries)
            {
                if (!first)
                {
                    text += ", ";
                }
                first = false;
                text += to_display_string(key);
                text += ": ";
                text += to_display_string(entry);
            }
            text += '}';
            return text;
        }
        std::string operator()(const object_ref &object) const
        {
            std::string text = object->type_name + "{";
            for (size_t index = 0; index < object->fields.size(); ++index)
            {
                if (index != 0)
                {
                    text += ", ";
                }
                text += object->field_names[index];
                text += ": ";
                text += to_display_string(object->fields[index]);
            }
            text += '}';
            return text;
        }
        std::string operator()(const buffer_ref &buffer) const
        {
            return "buffer[" + std::to_string(buffer->capacity) + "]" +
                   sequence_to_string(buffer->items, '[', ']');
        }
        std::string operator()(const function_value &) const { return "<function>"; }
        std::string operator()(const builtin_ref &builtin) const
        {
            return "<builtin " + builtin->name + ">";
        }
        std::string operator()(const namespace_ref &space) const
        {
            return "<namespace " + space->name + ">";
        }
        std::string operator()(const io_ref &handle) const
        {
            return "<" + handle->description + ">";
        }
        std::string operator()(const pipe_ref &) const { return "<pipe>"; }
        std::string operator()(const lock_ref &lock) const
        {
            return "<lock " + lock->name + ">";
        }
        std::string operator()(const thread_ref &) const { return "<thread>"; }
        std::string operator()(const library_ref &library) const
        {
            return "<library " + library->path + ">";
        }
        std::string operator()(const foreign_callable_ref &fn) const
        {
            return "<foreign " + fn->symbol + ">";
        }
    };
    return std::visit(printer{}, item.data);
}

/// @return True when @p item is truthy for `if`, `loop`, `&&`, and `||`.
inline bool is_truthy(const value &item)
{
    if (const auto *flag = item.get_if<bool>())
    {
        return *flag;
    }
    if (const auto *number = item.get_if<int64_t>())
    {
        return *number != 0;
    }
    if (const auto *number = item.get_if<double>())
    {
        return *number != 0.0;
    }
    if (const auto *text = try_string(item))
    {
        return !text->empty();
    }
    if (const auto *letter = item.get_if<char>())
    {
        return *letter != '\0';
    }
    return !item.is_null();
}

/// @return True when @p item holds `int`, `float`, `bool`, or `character`.
inline bool is_numeric(const value &item)
{
    return std::holds_alternative<int64_t>(item.data) ||
           std::holds_alternative<double>(item.data) ||
           std::holds_alternative<bool>(item.data) ||
           std::holds_alternative<char>(item.data);
}

/// @return True when @p item is an integral value (no float involved).
inline bool is_integral(const value &item)
{
    return std::holds_alternative<int64_t>(item.data) ||
           std::holds_alternative<bool>(item.data) ||
           std::holds_alternative<char>(item.data);
}

/// @return @p item widened to `int64_t`.
inline int64_t as_integer(const value &item)
{
    if (const auto *number = item.get_if<int64_t>())
    {
        return *number;
    }
    if (const auto *flag = item.get_if<bool>())
    {
        return *flag ? 1 : 0;
    }
    if (const auto *letter = item.get_if<char>())
    {
        return static_cast<int64_t>(static_cast<unsigned char>(*letter));
    }
    if (const auto *number = item.get_if<double>())
    {
        return static_cast<int64_t>(*number);
    }
    throw_error(std::string{"expected a number, got "} + type_name(item));
    return 0;
}

/// @return @p item widened to `double`.
inline double as_number(const value &item)
{
    if (const auto *number = item.get_if<double>())
    {
        return *number;
    }
    return static_cast<double>(as_integer(item));
}

/// @return Structural equality, used by `==`, `!=`, and `match`.
inline bool values_equal(const value &left, const value &right)
{
    if (left.is_null() || right.is_null())
    {
        return left.is_null() && right.is_null();
    }
    if (is_numeric(left) && is_numeric(right))
    {
        if (is_integral(left) && is_integral(right))
        {
            return as_integer(left) == as_integer(right);
        }
        return as_number(left) == as_number(right);
    }
    if (const auto *text = try_string(left))
    {
        const auto *other = try_string(right);
        return other != nullptr && *text == *other;
    }
    if (const auto *member = left.get_if<enum_value>())
    {
        const auto *other = right.get_if<enum_value>();
        return other != nullptr && member->type_name == other->type_name &&
               member->member == other->member;
    }
    if (const auto *mode = left.get_if<mode_value>())
    {
        const auto *other = right.get_if<mode_value>();
        return other != nullptr && mode->name == other->name;
    }
    if (const auto *array = left.get_if<array_value>())
    {
        const auto *other = right.get_if<array_value>();
        return other != nullptr && array->data == other->data;
    }
    if (const auto *tuple = left.get_if<tuple_value>())
    {
        const auto *other = right.get_if<tuple_value>();
        return other != nullptr && tuple->data == other->data;
    }
    if (const auto *object = left.get_if<object_ref>())
    {
        const auto *other = right.get_if<object_ref>();
        return other != nullptr && *object == *other;
    }
    if (const auto *handle = left.get_if<io_ref>())
    {
        const auto *other = right.get_if<io_ref>();
        return other != nullptr && *handle == *other;
    }
    if (const auto *map = left.get_if<map_value>())
    {
        const auto *other = right.get_if<map_value>();
        return other != nullptr && map->data == other->data;
    }
    return false;
}

inline value *map_find_entry(map_object &map, const value &key)
{
    for (auto &[entry_key, entry_value] : map.entries)
    {
        if (values_equal(entry_key, key))
        {
            return &entry_value;
        }
    }
    return nullptr;
}

inline const value *map_find_entry(const map_object &map, const value &key)
{
    for (const auto &[entry_key, entry_value] : map.entries)
    {
        if (values_equal(entry_key, key))
        {
            return &entry_value;
        }
    }
    return nullptr;
}

inline void map_store_entry(map_object &map, value &key, value &item)
{
    if (value *existing = map_find_entry(map, key))
    {
        *existing = std::move(item);
        return;
    }
    map.entries.emplace_back(std::move(key), std::move(item));
}

inline void map_merge(map_object &target, const map_object &patch)
{
    for (const auto &[key, item] : patch.entries)
    {
        if (value *existing = map_find_entry(target, key))
        {
            *existing = item;
            continue;
        }
        target.entries.emplace_back(key, item);
    }
}

} // namespace munx::vm
