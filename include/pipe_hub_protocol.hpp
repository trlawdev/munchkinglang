#pragma once

#include "platform.hpp"
#include "vm_value.hpp"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if MUNX_PLATFORM_POSIX
#include <cerrno>
#include <unistd.h>
#endif

namespace munx::vm::pipe_hub
{

inline thread_local bool protocol_error_flag{false};

inline void fail_protocol(const char *message)
{
    (void)message;
    protocol_error_flag = true;
}

enum class opcode : uint8_t
{
    Hello = 1,
    Bye = 2,
    Attach = 3,
    Detach = 4,
    Publish = 5,
    Deliver = 6,
    Ack = 7,
    Error = 8,
    /// Bidirectional channel handshake (exactly two peers per channel id).
    ChannelOffer = 9,     ///< Peer wants to send; hub replies Accept or Busy.
    ChannelAccept = 10,   ///< Hub → offerer: peer is ready for payload.
    ChannelBusy = 11,     ///< Collision: both peers offered; retry after backoff.
    ChannelRecvReady = 12,///< Peer is blocked in extract waiting for data.
    ChannelDataAck = 13,  ///< Receiver acknowledged payload delivery.
};

enum class attachment_mode : uint8_t
{
    Writer = 0,
    QueueIn = 1,
    BroadcastIn = 2,
    ChannelPeer = 3, ///< Bidirectional channel endpoint (max 2 per pipe id).
};

/// Identifies the process class that opened a hub connection.
/// Optional trailing field on Hello; absent ⇒ Unknown (legacy clients).
enum class client_kind : uint8_t
{
    Unknown = 0,
    Vm = 1,     ///< munx bytecode VM / JIT process
    Native = 2, ///< natively compiled munx executable
    Tool = 3,   ///< external tooling (Python probes, etc.)
};

/// Resolve the host directory for pipes and the hub socket.
inline std::filesystem::path pipe_directory()
{
    if (const char *configured = std::getenv("MUNX_PIPE_DIR"))
    {
        if (configured[0] != '\0')
        {
            return configured;
        }
    }
    return std::filesystem::temp_directory_path() / "munx-pipes";
}

inline std::filesystem::path hub_socket_path()
{
    return pipe_directory() / "hub.sock";
}

inline std::filesystem::path hub_lock_path()
{
    return pipe_directory() / "hub.lock";
}

inline std::filesystem::path hub_pid_path()
{
    return pipe_directory() / "hub.pid";
}

inline bool hub_enabled()
{
    if (const char *configured = std::getenv("MUNX_PIPE_HUB"))
    {
        return configured[0] != '0' || configured[1] != '\0';
    }
    return true;
}

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

inline void append_string(std::vector<std::byte> &buffer, std::string_view text)
{
    if (text.size() > std::numeric_limits<uint32_t>::max())
    {
        fail_protocol("pipe hub string exceeds the 32-bit length limit");
    }
    append_u32(buffer, static_cast<uint32_t>(text.size()));
    for (const char letter : text)
    {
        append_u8(buffer, static_cast<uint8_t>(letter));
    }
}

inline void append_bytes(std::vector<std::byte> &buffer,
                         std::span<const std::byte> bytes)
{
    if (bytes.size() > std::numeric_limits<uint32_t>::max())
    {
        fail_protocol("pipe hub payload exceeds the 32-bit length limit");
    }
    append_u32(buffer, static_cast<uint32_t>(bytes.size()));
    buffer.insert(buffer.end(), bytes.begin(), bytes.end());
}

class reader
{
    std::span<const std::byte> data_;
    size_t cursor_{0};

public:
    explicit reader(std::span<const std::byte> data) : data_(data) {}

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

    std::string string()
    {
        const uint32_t length = u32();
        const auto bytes = read(length);
        return std::string{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
    }

    std::vector<std::byte> bytes()
    {
        const uint32_t length = u32();
        const auto slice = read(length);
        return std::vector<std::byte>{slice.begin(), slice.end()};
    }

    std::span<const std::byte> remaining() const
    {
        return data_.subspan(cursor_);
    }

private:
    std::span<const std::byte> read(size_t count)
    {
        if (cursor_ + count > data_.size())
        {
            fail_protocol("truncated pipe hub message");
        }
        const std::span<const std::byte> slice{data_.data() + cursor_, count};
        cursor_ += count;
        return slice;
    }
};

inline std::vector<std::byte> encode_frame(std::span<const std::byte> body)
{
    if (body.size() > std::numeric_limits<uint32_t>::max())
    {
        fail_protocol("pipe hub frame exceeds the 32-bit length limit");
    }
    std::vector<std::byte> frame;
    frame.reserve(sizeof(uint32_t) + body.size());
    const uint32_t length = static_cast<uint32_t>(body.size());
    append_u32(frame, length);
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

inline std::vector<std::byte> encode_hello(uint64_t pid,
                                           client_kind kind = client_kind::Unknown)
{
    std::vector<std::byte> body;
    append_u8(body, static_cast<uint8_t>(opcode::Hello));
    append_u64(body, pid);
    append_u8(body, static_cast<uint8_t>(kind));
    return encode_frame(body);
}

inline std::vector<std::byte> encode_bye()
{
    std::vector<std::byte> body;
    append_u8(body, static_cast<uint8_t>(opcode::Bye));
    return encode_frame(body);
}

inline std::vector<std::byte> encode_attach(std::string_view channel,
                                            attachment_mode mode)
{
    std::vector<std::byte> body;
    append_u8(body, static_cast<uint8_t>(opcode::Attach));
    append_string(body, channel);
    append_u8(body, static_cast<uint8_t>(mode));
    return encode_frame(body);
}

inline std::vector<std::byte> encode_detach(std::string_view channel,
                                            attachment_mode mode)
{
    std::vector<std::byte> body;
    append_u8(body, static_cast<uint8_t>(opcode::Detach));
    append_string(body, channel);
    append_u8(body, static_cast<uint8_t>(mode));
    return encode_frame(body);
}

inline std::vector<std::byte> encode_publish(std::string_view channel,
                                             std::span<const std::byte> payload)
{
    std::vector<std::byte> body;
    append_u8(body, static_cast<uint8_t>(opcode::Publish));
    append_string(body, channel);
    append_bytes(body, payload);
    return encode_frame(body);
}

inline std::vector<std::byte> encode_deliver(std::string_view channel,
                                             attachment_mode mode,
                                             std::span<const std::byte> payload)
{
    std::vector<std::byte> body;
    append_u8(body, static_cast<uint8_t>(opcode::Deliver));
    append_string(body, channel);
    append_u8(body, static_cast<uint8_t>(mode));
    append_bytes(body, payload);
    return encode_frame(body);
}

inline std::vector<std::byte> encode_ack()
{
    std::vector<std::byte> body;
    append_u8(body, static_cast<uint8_t>(opcode::Ack));
    return encode_frame(body);
}

inline std::vector<std::byte> encode_error(std::string_view message)
{
    std::vector<std::byte> body;
    append_u8(body, static_cast<uint8_t>(opcode::Error));
    append_string(body, message);
    return encode_frame(body);
}

inline std::vector<std::byte> encode_channel_op(opcode kind, std::string_view channel)
{
    std::vector<std::byte> body;
    append_u8(body, static_cast<uint8_t>(kind));
    append_string(body, channel);
    return encode_frame(body);
}

struct message
{
    opcode kind{opcode::Ack};
    uint64_t pid{0};
    client_kind client{client_kind::Unknown};
    std::string channel;
    attachment_mode mode{attachment_mode::Writer};
    std::vector<std::byte> payload;
    std::string text;
};

inline message decode_message(std::span<const std::byte> body)
{
    reader input{body};
    message decoded;
    decoded.kind = static_cast<opcode>(input.u8());
    switch (decoded.kind)
    {
    case opcode::Hello:
        decoded.pid = input.u64();
        // Optional client_kind for native / tool peers (legacy Hellos omit it).
        if (!input.remaining().empty())
        {
            decoded.client = static_cast<client_kind>(input.u8());
        }
        break;
    case opcode::Bye:
        break;
    case opcode::Attach:
    case opcode::Detach:
        decoded.channel = input.string();
        decoded.mode = static_cast<attachment_mode>(input.u8());
        break;
    case opcode::Publish:
        decoded.channel = input.string();
        decoded.payload = input.bytes();
        break;
    case opcode::Deliver:
        decoded.channel = input.string();
        decoded.mode = static_cast<attachment_mode>(input.u8());
        decoded.payload = input.bytes();
        break;
    case opcode::Ack:
        break;
    case opcode::Error:
        decoded.text = input.string();
        break;
    case opcode::ChannelOffer:
    case opcode::ChannelAccept:
    case opcode::ChannelBusy:
    case opcode::ChannelRecvReady:
    case opcode::ChannelDataAck:
        decoded.channel = input.string();
        break;
    }
    return decoded;
}

namespace io
{

inline bool write_all(int descriptor, const void *data, size_t length)
{
#if MUNX_PLATFORM_POSIX
    const auto *bytes = static_cast<const std::byte *>(data);
    size_t written = 0;
    while (written < length)
    {
        const ssize_t chunk =
            ::write(descriptor, bytes + written, length - written);
        if (chunk <= 0)
        {
            if (chunk < 0 && errno == EINTR)
            {
                continue;
            }
            return false;
        }
        written += static_cast<size_t>(chunk);
    }
    return true;
#else
    (void)descriptor;
    (void)data;
    (void)length;
    return false;
#endif
}

inline bool read_all(int descriptor, void *data, size_t length)
{
#if MUNX_PLATFORM_POSIX
    auto *bytes = static_cast<std::byte *>(data);
    size_t received = 0;
    while (received < length)
    {
        const ssize_t chunk = ::read(descriptor, bytes + received, length - received);
        if (chunk < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (chunk == 0)
        {
            return false;
        }
        received += static_cast<size_t>(chunk);
    }
    return true;
#else
    (void)descriptor;
    (void)data;
    (void)length;
    return false;
#endif
}

} // namespace io

} // namespace munx::vm::pipe_hub
