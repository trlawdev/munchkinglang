#pragma once

#include "platform.hpp"
#include "vm_value.hpp"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#if MUNX_PLATFORM_POSIX || MUNX_PLATFORM_WINDOWS
#define MUNX_VM_HAS_NAMED_PIPES 1
#else
#define MUNX_VM_HAS_NAMED_PIPES 0
#endif

#if MUNX_VM_HAS_NAMED_PIPES
#include "pipe_hub_client.hpp"
#if MUNX_PLATFORM_POSIX
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#endif

namespace munx::vm
{

namespace detail
{

enum class wire_tag : uint8_t
{
    Null = 0,
    Bool = 1,
    Int = 2,
    Float = 3,
    Char = 4,
    String = 5,
    Regex = 6,
    Enum = 7,
    Array = 8,
    Tuple = 9,
    Object = 10,
    Exception = 11,
};

inline void append_u8(std::vector<std::byte> &buffer, uint8_t byte)
{
    buffer.push_back(static_cast<std::byte>(byte));
}

inline void append_u32(std::vector<std::byte> &buffer, uint32_t word)
{
    append_u8(buffer, static_cast<uint8_t>(word & 0xFF));
    append_u8(buffer, static_cast<uint8_t>((word >> 8) & 0xFF));
    append_u8(buffer, static_cast<uint8_t>((word >> 16) & 0xFF));
    append_u8(buffer, static_cast<uint8_t>((word >> 24) & 0xFF));
}

inline void append_u64(std::vector<std::byte> &buffer, uint64_t word)
{
    for (int shift = 0; shift < 64; shift += 8)
    {
        append_u8(buffer, static_cast<uint8_t>((word >> shift) & 0xFF));
    }
}

inline void append_f64(std::vector<std::byte> &buffer, double number)
{
    uint64_t bits = 0;
    static_assert(sizeof(number) == sizeof(bits));
    std::memcpy(&bits, &number, sizeof bits);
    append_u64(buffer, bits);
}

inline void append_string(std::vector<std::byte> &buffer, std::string_view text)
{
    if (text.size() > std::numeric_limits<uint32_t>::max())
    {
        throw_error("pipe payload string exceeds the 32-bit length limit");
    }
    append_u32(buffer, static_cast<uint32_t>(text.size()));
    for (const char letter : text)
    {
        append_u8(buffer, static_cast<uint8_t>(letter));
    }
}

class wire_reader
{
    std::span<const std::byte> data_;
    size_t cursor_{0};

public:
    explicit wire_reader(std::span<const std::byte> data) : data_(data) {}

    uint8_t u8()
    {
        return static_cast<uint8_t>(read(sizeof(uint8_t))[0]);
    }

    uint32_t u32()
    {
        const auto bytes = read(sizeof(uint32_t));
        return static_cast<uint32_t>(bytes[0]) |
               (static_cast<uint32_t>(bytes[1]) << 8) |
               (static_cast<uint32_t>(bytes[2]) << 16) |
               (static_cast<uint32_t>(bytes[3]) << 24);
    }

    uint64_t u64()
    {
        const auto bytes = read(sizeof(uint64_t));
        uint64_t word = 0;
        for (size_t index = 0; index < bytes.size(); ++index)
        {
            word |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[index]))
                    << (index * 8);
        }
        return word;
    }

    double f64()
    {
        const uint64_t bits = u64();
        double number = 0.0;
        std::memcpy(&number, &bits, sizeof number);
        return number;
    }

    std::string string()
    {
        const uint32_t length = u32();
        const auto bytes = read(length);
        return std::string{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
    }

private:
    std::span<const std::byte> read(size_t count)
    {
        if (cursor_ + count > data_.size())
        {
            throw_error("truncated pipe payload");
        }
        const std::span<const std::byte> slice{data_.data() + cursor_, count};
        cursor_ += count;
        return slice;
    }
};

inline void encode_value(std::vector<std::byte> &buffer, const value &item);

inline void encode_sequence(std::vector<std::byte> &buffer,
                            const value_vector &items)
{
    if (items.size() > std::numeric_limits<uint32_t>::max())
    {
        throw_error("pipe payload sequence exceeds the 32-bit length limit");
    }
    append_u32(buffer, static_cast<uint32_t>(items.size()));
    for (const value &element : items)
    {
        encode_value(buffer, element);
    }
}

inline void encode_value(std::vector<std::byte> &buffer, const value &item)
{
    struct encoder
    {
        std::vector<std::byte> &out;

        void operator()(std::monostate) const
        {
            append_u8(out, static_cast<uint8_t>(wire_tag::Null));
        }
        void operator()(bool flag) const
        {
            append_u8(out, static_cast<uint8_t>(wire_tag::Bool));
            append_u8(out, flag ? 1 : 0);
        }
        void operator()(int64_t number) const
        {
            append_u8(out, static_cast<uint8_t>(wire_tag::Int));
            append_u64(out, static_cast<uint64_t>(number));
        }
        void operator()(double number) const
        {
            append_u8(out, static_cast<uint8_t>(wire_tag::Float));
            append_f64(out, number);
        }
        void operator()(char letter) const
        {
            append_u8(out, static_cast<uint8_t>(wire_tag::Char));
            append_u8(out, static_cast<uint8_t>(letter));
        }
        void operator()(const string_value &text) const
        {
            append_u8(out, static_cast<uint8_t>(wire_tag::String));
            append_string(out, *text.data);
        }
        void operator()(const regex_value &regex) const
        {
            append_u8(out, static_cast<uint8_t>(wire_tag::Regex));
            append_string(out, regex.pattern);
        }
        void operator()(const enum_value &member) const
        {
            append_u8(out, static_cast<uint8_t>(wire_tag::Enum));
            append_string(out, member.type_name);
            append_string(out, member.member);
        }
        void operator()(const mode_value &) const
        {
            throw_error("io modes cannot be sent through a pipe");
        }
        void operator()(const exception_value &error) const
        {
            append_u8(out, static_cast<uint8_t>(wire_tag::Exception));
            append_string(out, error.message);
        }
        void operator()(const array_value &array) const
        {
            append_u8(out, static_cast<uint8_t>(wire_tag::Array));
            encode_sequence(out, array.data->items);
        }
        void operator()(const simd_value &) const
        {
            throw_error("SIMD vectors cannot be sent through a pipe");
        }
        void operator()(const tuple_value &tuple) const
        {
            append_u8(out, static_cast<uint8_t>(wire_tag::Tuple));
            encode_sequence(out, tuple.data->items);
        }
        void operator()(const map_value &) const
        {
            throw_error("maps cannot be sent through a pipe");
        }
        void operator()(const object_ref &object) const
        {
            append_u8(out, static_cast<uint8_t>(wire_tag::Object));
            append_string(out, object->type_name);
            if (object->fields.size() > std::numeric_limits<uint32_t>::max())
            {
                throw_error("pipe payload object exceeds the 32-bit field limit");
            }
            append_u32(out, static_cast<uint32_t>(object->fields.size()));
            for (size_t index = 0; index < object->fields.size(); ++index)
            {
                append_string(out, object->field_names[index]);
                encode_value(out, object->fields[index]);
            }
        }
        void operator()(const buffer_ref &) const
        {
            throw_error("buffers cannot be sent through a pipe");
        }
        void operator()(const function_value &) const
        {
            throw_error("functions cannot be sent through a pipe");
        }
        void operator()(const builtin_ref &) const
        {
            throw_error("builtins cannot be sent through a pipe");
        }
        void operator()(const namespace_ref &) const
        {
            throw_error("namespaces cannot be sent through a pipe");
        }
        void operator()(const io_ref &) const
        {
            throw_error("io handles cannot be sent through a pipe");
        }
        void operator()(const pipe_ref &) const
        {
            throw_error("pipe handles cannot be sent through a pipe");
        }
        void operator()(const lock_ref &) const
        {
            throw_error("locks cannot be sent through a pipe");
        }
        void operator()(const thread_ref &) const
        {
            throw_error("threads cannot be sent through a pipe");
        }
        void operator()(const library_ref &) const
        {
            throw_error("library handles cannot be sent through a pipe");
        }
        void operator()(const foreign_callable_ref &) const
        {
            throw_error("foreign callables cannot be sent through a pipe");
        }
    };
    std::visit(encoder{buffer}, item.data);
}

inline value decode_value(wire_reader &input)
{
    switch (static_cast<wire_tag>(input.u8()))
    {
    case wire_tag::Null:
        return value{};
    case wire_tag::Bool:
        return value{input.u8() != 0};
    case wire_tag::Int:
        return value{static_cast<int64_t>(input.u64())};
    case wire_tag::Float:
        return value{input.f64()};
    case wire_tag::Char:
        return value{static_cast<char>(input.u8())};
    case wire_tag::String:
        return value{input.string()};
    case wire_tag::Regex:
        return value{regex_value{input.string()}};
    case wire_tag::Enum:
        return value{enum_value{input.string(), input.string()}};
    case wire_tag::Exception:
        return value{exception_value{exception_kind::Error, input.string()}};
    case wire_tag::Array:
    {
        const uint32_t count = input.u32();
        auto items = std::make_shared<sequence_object>();
        items->items.reserve(count);
        for (uint32_t index = 0; index < count; ++index)
        {
            items->items.push_back(decode_value(input));
        }
        return value{array_value{items}};
    }
    case wire_tag::Tuple:
    {
        const uint32_t count = input.u32();
        auto items = std::make_shared<sequence_object>();
        items->items.reserve(count);
        for (uint32_t index = 0; index < count; ++index)
        {
            items->items.push_back(decode_value(input));
        }
        return value{tuple_value{items}};
    }
    case wire_tag::Object:
    {
        const std::string type_name = input.string();
        const uint32_t field_count = input.u32();
        auto instance = std::make_shared<object_instance>();
        instance->type_name = type_name;
        instance->field_names.reserve(field_count);
        instance->fields.reserve(field_count);
        for (uint32_t index = 0; index < field_count; ++index)
        {
            instance->field_names.push_back(input.string());
            instance->fields.push_back(decode_value(input));
        }
        return value{instance};
    }
    }
    throw_error("unsupported pipe payload tag");
    return value{};
}

inline std::vector<std::byte> serialize_value(const value &item)
{
    std::vector<std::byte> payload;
    encode_value(payload, item);
    return payload;
}

inline value deserialize_value(std::span<const std::byte> payload)
{
    wire_reader input{payload};
    return decode_value(input);
}

} // namespace detail

/// Native named pipe / channel endpoint for `pipe(...)`, `channel(...)`,
/// `->` / `<-`, and `:=>` / `<=:`.
struct pipe_object
{
    std::string id;
    std::string path;
    int read_fd{-1};
    int write_fd{-1};
    bool readable{false};
    bool writable{false};
    bool broadcast{false};
    bool use_hub{false};
    pipe_hub::attachment_mode hub_mode{pipe_hub::attachment_mode::Writer};
    std::mutex read_mutex;
    std::mutex write_mutex;

    ~pipe_object() { close(); }

    pipe_object(const pipe_object &) = delete;
    pipe_object &operator=(const pipe_object &) = delete;

    pipe_object() = default;
    pipe_object(pipe_object &&other) noexcept { swap(other); }
    pipe_object &operator=(pipe_object &&other) noexcept
    {
        if (this != &other)
        {
            close();
            swap(other);
        }
        return *this;
    }

    /// Resolve the host directory that stores named FIFOs and the hub socket.
    static std::filesystem::path pipe_directory()
    {
        return pipe_hub::pipe_directory();
    }

    /// Map a munx pipe id to a safe on-disk FIFO name.
    static std::string sanitize_id(std::string_view id)
    {
        std::string safe;
        safe.reserve(id.size());
        for (const char letter : id)
        {
            if (letter == '/' || letter == '\\' || letter == '\0')
            {
                safe.push_back('_');
            }
            else
            {
                safe.push_back(letter);
            }
        }
        if (safe.empty())
        {
            safe = "anonymous";
        }
        return safe;
    }

    static void ensure_directory()
    {
        std::error_code error;
        std::filesystem::create_directories(pipe_directory(), error);
        if (error)
        {
            throw_error("could not create pipe directory " +
                        pipe_directory().string() + ": " + error.message());
        }
    }

    /// Create (if needed) and open one end of a named pipe.
    static pipe_ref open(const std::string &id, bool reading, bool writing,
                         bool subscribing = false)
    {
#if MUNX_VM_HAS_NAMED_PIPES
        if (reading == writing)
        {
            throw_error("pipe expects exactly one of `in`, `out`, or `subscribe`");
        }
        if (subscribing && !reading)
        {
            throw_error("pipe `subscribe` mode requires a read attachment");
        }

        auto handle = std::make_shared<pipe_object>();
        handle->id = id;
        handle->readable = reading;
        handle->writable = writing;
        handle->broadcast = subscribing;
        handle->use_hub = pipe_hub::hub_enabled();

        if (handle->use_hub)
        {
            if (writing)
            {
                handle->hub_mode = pipe_hub::attachment_mode::Writer;
            }
            else if (subscribing)
            {
                handle->hub_mode = pipe_hub::attachment_mode::BroadcastIn;
            }
            else
            {
                handle->hub_mode = pipe_hub::attachment_mode::QueueIn;
            }
            pipe_hub::client::instance().attach(handle->id, handle->hub_mode);
            return handle;
        }

#if MUNX_PLATFORM_WINDOWS
        throw_error("named pipes on Windows require the pipe hub "
                    "(unset MUNX_PIPE_HUB=0 to disable hub mode)");
#else
        ensure_directory();
        handle->path = (pipe_directory() / sanitize_id(handle->id)).string();

        if (::mkfifo(handle->path.c_str(), 0666) != 0 && errno != EEXIST)
        {
            throw_error(std::string{"mkfifo("} + handle->path +
                        ") failed: " + std::strerror(errno));
        }

        if (reading)
        {
            handle->read_fd = ::open(handle->path.c_str(), O_RDONLY);
            if (handle->read_fd < 0)
            {
                throw_error(std::string{"open("} + handle->path +
                            ") for read failed: " + std::strerror(errno));
            }
        }
        else
        {
            // O_RDWR avoids open/deadlock when the reader connects later in the
            // same address space; cross-process readers use `pipe(name, in)`.
            handle->write_fd = ::open(handle->path.c_str(), O_RDWR);
            if (handle->write_fd < 0)
            {
                throw_error(std::string{"open("} + handle->path +
                            ") for write failed: " + std::strerror(errno));
            }
        }
        return handle;
#endif
#else
        (void)id;
        (void)reading;
        (void)writing;
        (void)subscribing;
        throw_error("named pipes are not supported on this platform");
#endif
    }

    void close()
    {
#if MUNX_VM_HAS_NAMED_PIPES
        if (use_hub)
        {
            if (!id.empty())
            {
                pipe_hub::client::instance().detach(id, hub_mode);
                id.clear();
            }
            return;
        }
        std::scoped_lock guard{read_mutex, write_mutex};
        if (read_fd >= 0)
        {
            ::close(read_fd);
            read_fd = -1;
        }
        if (write_fd >= 0)
        {
            ::close(write_fd);
            write_fd = -1;
        }
#endif
    }

    void insert(value &item)
    {
#if MUNX_VM_HAS_NAMED_PIPES
        if (!writable)
        {
            throw_error("write to a read-only pipe `" + id + "`");
        }

        const std::vector<std::byte> payload = detail::serialize_value(item);
        if (payload.size() > std::numeric_limits<uint32_t>::max())
        {
            throw_error("pipe payload exceeds the 32-bit length limit");
        }

        if (use_hub)
        {
            std::lock_guard<std::mutex> guard{write_mutex};
            pipe_hub::client::instance().publish(id, payload);
            return;
        }

        if (write_fd < 0)
        {
            throw_error("write to a closed pipe `" + id + "`");
        }

        const uint32_t length = static_cast<uint32_t>(payload.size());
        std::vector<std::byte> frame;
        frame.reserve(sizeof(uint32_t) + payload.size());
        frame.push_back(static_cast<std::byte>(length & 0xFF));
        frame.push_back(static_cast<std::byte>((length >> 8) & 0xFF));
        frame.push_back(static_cast<std::byte>((length >> 16) & 0xFF));
        frame.push_back(static_cast<std::byte>((length >> 24) & 0xFF));
        frame.insert(frame.end(), payload.begin(), payload.end());

        // One write per message so competing readers on the same FIFO cannot
        // interleave header and payload bytes (POSIX FIFOs are byte streams).
        std::lock_guard<std::mutex> guard{write_mutex};
        write_all(write_fd, frame.data(), frame.size());
#else
        (void)item;
        throw_error("named pipes are not supported on this platform");
#endif
    }

    value extract()
    {
#if MUNX_VM_HAS_NAMED_PIPES
        if (!readable)
        {
            throw_error("read from a write-only pipe `" + id + "`");
        }

        if (use_hub)
        {
            std::lock_guard<std::mutex> guard{read_mutex};
            const std::vector<std::byte> payload =
                pipe_hub::client::instance().receive_bytes(id, hub_mode);
            if (payload.empty())
            {
                return value{};
            }
            return detail::deserialize_value(payload);
        }

        if (read_fd < 0)
        {
            throw_error("read from a closed pipe `" + id + "`");
        }

        std::byte header[4];
        std::lock_guard<std::mutex> guard{read_mutex};
        if (!read_all(read_fd, header, sizeof header))
        {
            return value{};
        }

        const uint32_t length =
            static_cast<uint32_t>(std::to_integer<uint8_t>(header[0])) |
            (static_cast<uint32_t>(std::to_integer<uint8_t>(header[1])) << 8) |
            (static_cast<uint32_t>(std::to_integer<uint8_t>(header[2])) << 16) |
            (static_cast<uint32_t>(std::to_integer<uint8_t>(header[3])) << 24);

        if (length == 0)
        {
            return value{};
        }

        std::vector<std::byte> payload(length);
        if (!read_all(read_fd, payload.data(), payload.size()))
        {
            return value{};
        }
        return detail::deserialize_value(payload);
#else
        throw_error("named pipes are not supported on this platform");
#endif
    }

    /// Open a bidirectional channel peer (`channel("id")`). Hub required.
    static pipe_ref open_channel(const std::string &channel_id)
    {
#if MUNX_VM_HAS_NAMED_PIPES
        if (!pipe_hub::hub_enabled())
        {
            throw_error("channel() requires the pipe hub (unset MUNX_PIPE_HUB=0)");
        }
        auto handle = std::make_shared<pipe_object>();
        handle->id = channel_id;
        handle->readable = true;
        handle->writable = true;
        handle->use_hub = true;
        handle->hub_mode = pipe_hub::attachment_mode::ChannelPeer;
        pipe_hub::client::instance().attach(handle->id, handle->hub_mode);
        return handle;
#else
        (void)channel_id;
        throw_error("channels are not supported on this platform");
#endif
    }

    bool is_channel() const
    {
        return use_hub && hub_mode == pipe_hub::attachment_mode::ChannelPeer;
    }

    /// Channel send (`value :=> name`) with offer/accept + collision backoff.
    void channel_insert(value &item)
    {
#if MUNX_VM_HAS_NAMED_PIPES
        if (!is_channel())
        {
            throw_error("`:=>` requires a channel handle");
        }
        const std::vector<std::byte> payload = detail::serialize_value(item);
        std::lock_guard<std::mutex> guard{write_mutex};
        pipe_hub::client::instance().channel_send(id, payload);
#else
        (void)item;
        throw_error("channels are not supported on this platform");
#endif
    }

    /// Channel extract (`<=: name`) — wait for peer payload.
    value channel_extract()
    {
#if MUNX_VM_HAS_NAMED_PIPES
        if (!is_channel())
        {
            throw_error("`<=:` requires a channel handle");
        }
        std::lock_guard<std::mutex> guard{read_mutex};
        const std::vector<std::byte> payload =
            pipe_hub::client::instance().channel_recv(id);
        if (payload.empty())
        {
            return value{};
        }
        return detail::deserialize_value(payload);
#else
        throw_error("channels are not supported on this platform");
#endif
    }

private:
    void swap(pipe_object &other) noexcept
    {
        id.swap(other.id);
        path.swap(other.path);
        std::swap(read_fd, other.read_fd);
        std::swap(write_fd, other.write_fd);
        std::swap(readable, other.readable);
        std::swap(writable, other.writable);
        std::swap(broadcast, other.broadcast);
        std::swap(use_hub, other.use_hub);
        std::swap(hub_mode, other.hub_mode);
    }

#if MUNX_VM_HAS_NAMED_PIPES
    void write_all(int descriptor, const void *data, size_t length)
    {
        const auto *bytes = static_cast<const std::byte *>(data);
        size_t written = 0;
        while (written < length)
        {
            const ssize_t chunk =
                ::write(descriptor, bytes + written, length - written);
            if (chunk <= 0)
            {
                throw_error(std::string{"pipe write failed: "} +
                            std::strerror(errno));
            }
            written += static_cast<size_t>(chunk);
        }
    }

    bool read_all(int descriptor, void *data, size_t length)
    {
        auto *bytes = static_cast<std::byte *>(data);
        size_t received = 0;
        while (received < length)
        {
            const ssize_t chunk =
                ::read(descriptor, bytes + received, length - received);
            if (chunk < 0)
            {
                throw_error(std::string{"pipe read failed: "} +
                            std::strerror(errno));
            }
            if (chunk == 0)
            {
                return false;
            }
            received += static_cast<size_t>(chunk);
        }
        return true;
    }
#endif
};

} // namespace munx::vm
