#pragma once

#include "pipe_hub_protocol.hpp"
#include "pipe_hub_transport.hpp"
#include "platform.hpp"
#include "vm_value.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <unordered_map>

#if MUNX_PLATFORM_POSIX
#include <cerrno>
#include <poll.h>
#endif

namespace munx::vm::pipe_hub
{

namespace detail
{

inline std::string default_executable_path()
{
    return munx::platform_executable_path();
}

struct delivery_queue
{
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::vector<std::byte>> items;
    bool closed{false};
};

struct queue_key
{
    std::string channel;
    attachment_mode mode{attachment_mode::QueueIn};

    bool operator==(const queue_key &other) const
    {
        return channel == other.channel && mode == other.mode;
    }
};

struct queue_key_hash
{
    size_t operator()(const queue_key &key) const
    {
        return std::hash<std::string>{}(key.channel) ^
               (static_cast<size_t>(key.mode) << 1);
    }
};

} // namespace detail

/// Process-local pipe hub client; one connection shared by all pipe handles.
class client
{
    std::mutex connection_mutex_;
    std::mutex queues_mutex_;
    transport::hub_connection connection_;
    std::thread reader_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> shutdown_{false};
    std::atomic<bool> reader_paused_{false};
    std::string executable_path_{detail::default_executable_path()};
    std::unordered_map<detail::queue_key, std::shared_ptr<detail::delivery_queue>,
                       detail::queue_key_hash>
        queues_;
    std::vector<std::byte> input_;
    size_t frame_size_{0};
    bool have_header_{false};
    std::condition_variable ack_ready_;
    std::mutex ack_mutex_;
    bool waiting_for_ack_{false};
    bool ack_received_{false};
    std::string ack_error_;
    bool waiting_for_channel_{false};
    bool channel_reply_received_{false};
    opcode channel_reply_{opcode::Ack};

public:
    static client &instance()
    {
        static client singleton;
        return singleton;
    }

    void set_executable_path(const std::string &path)
    {
        if (!path.empty())
        {
            executable_path_ = std::move(path);
        }
    }

    void ensure_connected()
    {
        if (!hub_enabled())
        {
            return;
        }
        bool start_reader = false;
        {
            std::lock_guard<std::mutex> guard{connection_mutex_};
            if (connected_)
            {
                return;
            }
            connect_locked();
            if (!connected_)
            {
                return;
            }
            const std::vector<std::byte> frame =
                encode_hello(munx::platform_process_id(), client_kind::Vm);
            (void)connection_.write_all(frame);
            running_ = true;
            start_reader = true;
        }
        if (start_reader)
        {
            reader_ = std::thread([this] { reader_loop(); });
        }
    }

    void disconnect()
    {
        // Trip the reader loop before taking connection_mutex_, so it cannot
        // sit in poll while disconnect waits on the same mutex.
        shutdown_ = true;
        running_ = false;
        reader_paused_ = false;

        std::thread reader;
        {
            std::lock_guard<std::mutex> guard{connection_mutex_};
            if (!connected_ && !reader_.joinable())
            {
                shutdown_ = false;
                return;
            }
            if (connection_.valid())
            {
                const std::vector<std::byte> frame = encode_bye();
                (void)connection_.write_all(frame);
                connection_.shutdown_rw();
                connection_.close();
            }
            connected_ = false;
            reader = std::move(reader_);
        }
        {
            std::lock_guard<std::mutex> ack_guard{ack_mutex_};
            ack_ready_.notify_all();
        }
        close_all_queues();
        if (reader.joinable())
        {
            reader.join();
        }
        shutdown_ = false;
    }

    void attach(const std::string &channel, attachment_mode mode)
    {
        ensure_connected();
        if (mode == attachment_mode::QueueIn ||
            mode == attachment_mode::BroadcastIn ||
            mode == attachment_mode::ChannelPeer)
        {
            std::lock_guard<std::mutex> guard{queues_mutex_};
            const detail::queue_key key{channel, mode};
            if (!queues_.contains(key))
            {
                queues_.emplace(key, std::make_shared<detail::delivery_queue>());
            }
        }

        const std::vector<std::byte> frame = encode_attach(channel, mode);
        wait_for_hub_reply(frame, "attach");

        if (!ack_error_.empty())
        {
            // Roll back local delivery queue reservation on rejected Attach.
            if (mode == attachment_mode::QueueIn ||
                mode == attachment_mode::BroadcastIn ||
                mode == attachment_mode::ChannelPeer)
            {
                std::lock_guard<std::mutex> guard{queues_mutex_};
                queues_.erase(detail::queue_key{channel, mode});
            }
            throw_error("pipe hub error: " + ack_error_);
        }
    }

    void detach(const std::string &channel, attachment_mode mode)
    {
        {
            std::lock_guard<std::mutex> guard{connection_mutex_};
            if (!connected_)
            {
                return;
            }
            const std::vector<std::byte> frame = encode_detach(channel, mode);
            (void)connection_.write_all(frame);
        }
        if (mode == attachment_mode::QueueIn ||
            mode == attachment_mode::BroadcastIn ||
            mode == attachment_mode::ChannelPeer)
        {
            std::lock_guard<std::mutex> guard{queues_mutex_};
            queues_.erase(detail::queue_key{channel, mode});
        }
    }

    /// Bidirectional channel send: Offer → Accept|Busy, then Publish.
    /// On Busy (collision), back off 1–10 ms and retry (per language spec).
    void channel_send(const std::string &channel, std::span<const std::byte> payload)
    {
        ensure_connected();
        thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> backoff_ms{1, 10};
        for (int attempt = 0; attempt < 1024; ++attempt)
        {
            const opcode reply = wait_for_channel_opcode(
                encode_channel_op(opcode::ChannelOffer, channel),
                {opcode::ChannelAccept, opcode::ChannelBusy}, "channel offer");
            if (reply == opcode::ChannelBusy)
            {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds{backoff_ms(rng)});
                continue;
            }
            wait_for_hub_reply(encode_publish(channel, payload), "channel send");
            if (!ack_error_.empty())
            {
                throw_error("pipe hub error: " + ack_error_);
            }
            return;
        }
        throw_error("channel `" + channel +
                    "` send aborted after too many collisions");
    }

    /// Bidirectional channel receive — signal ready, then block for Deliver.
    std::vector<std::byte> channel_recv(const std::string &channel)
    {
        ensure_connected();
        {
            std::lock_guard<std::mutex> guard{connection_mutex_};
            if (!connected_)
            {
                throw_error("pipe hub is not connected");
            }
            (void)connection_.write_all(
                encode_channel_op(opcode::ChannelRecvReady, channel));
        }

        const detail::queue_key key{channel, attachment_mode::ChannelPeer};
        // Pause the background reader so this thread owns inbound frames for the
        // channel rendezvous (avoids a race where Deliver is decoded on the
        // reader thread against a different queue instance).
        auto send_data_ack = [this, &channel]() {
            std::lock_guard<std::mutex> cguard{connection_mutex_};
            if (connected_)
            {
                (void)connection_.write_all(
                    encode_channel_op(opcode::ChannelDataAck, channel));
            }
        };
        while (true)
        {
            {
                std::vector<std::byte> queued;
                bool closed = false;
                {
                    std::lock_guard<std::mutex> qguard{queues_mutex_};
                    auto found = queues_.find(key);
                    if (found != queues_.end())
                    {
                        std::lock_guard<std::mutex> guard{found->second->mutex};
                        if (!found->second->items.empty())
                        {
                            queued = std::move(found->second->items.front());
                            found->second->items.pop_front();
                        }
                        else
                        {
                            closed = found->second->closed;
                        }
                    }
                }
                if (!queued.empty())
                {
                    send_data_ack();
                    return queued;
                }
                if (closed)
                {
                    return {};
                }
            }

            std::lock_guard<std::mutex> guard{connection_mutex_};
            if (!connected_ || shutdown_)
            {
                return {};
            }
            reader_paused_ = true;
            message decoded;
            if (!connection_.poll_readable(200))
            {
                reader_paused_ = false;
                continue;
            }
            if (!read_frame_locked(decoded))
            {
                reader_paused_ = false;
                return {};
            }
            if (decoded.kind == opcode::Deliver && decoded.channel == channel)
            {
                reader_paused_ = false;
                (void)connection_.write_all(
                    encode_channel_op(opcode::ChannelDataAck, channel));
                return decoded.payload;
            }
            handle_message(decoded);
            reader_paused_ = false;
        }
    }

    void publish(const std::string &channel, std::span<const std::byte> payload)
    {
        ensure_connected();
        const std::vector<std::byte> frame = encode_publish(channel, payload);
        wait_for_hub_reply(frame, "publish");
        if (!ack_error_.empty())
        {
            throw_error("pipe hub error: " + ack_error_);
        }
    }

    std::vector<std::byte> receive_bytes(const std::string &channel,
                                         attachment_mode mode)
    {
        ensure_connected();
        const detail::queue_key key{channel, mode};
        std::shared_ptr<detail::delivery_queue> queue;
        {
            std::lock_guard<std::mutex> guard{queues_mutex_};
            auto found = queues_.find(key);
            if (found == queues_.end())
            {
                queue = std::make_shared<detail::delivery_queue>();
                queues_.emplace(key, queue);
            }
            else
            {
                queue = found->second;
            }
        }

        std::unique_lock<std::mutex> guard{queue->mutex};
        queue->ready.wait(guard, [&] {
            return !queue->items.empty() || queue->closed;
        });
        if (queue->items.empty())
        {
            return {};
        }
        std::vector<std::byte> payload = std::move(queue->items.front());
        queue->items.pop_front();
        return payload;
    }

    class session
    {
    public:
        /// Lazy: the hub is contacted on first `pipe(...)` / attach, not at VM start.
        session() = default;

        ~session()
        {
            if (hub_enabled())
            {
                client::instance().disconnect();
            }
        }

        session(const session &) = delete;
        session &operator=(const session &) = delete;
    };

private:
    client() = default;

    /// Write @p frame then pump this connection until Accept/Busy/Error.
    /// Safe across processes: the peer's RecvReady uses a different socket.
    opcode wait_for_channel_opcode(const std::vector<std::byte> &frame,
                                   std::initializer_list<opcode> expected,
                                   const char *action)
    {
        {
            std::lock_guard<std::mutex> guard{connection_mutex_};
            if (!connected_)
            {
                throw_error(std::string{"pipe hub is not connected ("} + action +
                            ")");
            }
            reader_paused_ = true;
            {
                std::lock_guard<std::mutex> ack_guard{ack_mutex_};
                waiting_for_channel_ = true;
                channel_reply_received_ = false;
                channel_reply_ = opcode::Ack;
                ack_error_.clear();
            }
            if (!connection_.write_all(frame))
            {
                reader_paused_ = false;
                std::lock_guard<std::mutex> ack_guard{ack_mutex_};
                waiting_for_channel_ = false;
                throw_error(std::string{"pipe hub "} + action + " failed");
            }
            while (true)
            {
                {
                    std::lock_guard<std::mutex> ack_guard{ack_mutex_};
                    if (channel_reply_received_ || !connected_ || shutdown_ ||
                        !ack_error_.empty())
                    {
                        break;
                    }
                }
                message decoded;
                if (!read_frame_locked(decoded))
                {
                    break;
                }
                handle_message(decoded);
            }
            reader_paused_ = false;
        }
        waiting_for_channel_ = false;
        if (!ack_error_.empty())
        {
            throw_error("pipe hub error: " + ack_error_);
        }
        if (!channel_reply_received_)
        {
            throw_error(std::string{"pipe hub "} + action +
                        " did not receive a reply");
        }
        bool ok = false;
        for (opcode expect : expected)
        {
            if (channel_reply_ == expect)
            {
                ok = true;
                break;
            }
        }
        if (!ok)
        {
            throw_error(std::string{"pipe hub "} + action +
                        " got unexpected reply");
        }
        return channel_reply_;
    }

    /// Write @p frame then block until the hub replies with Ack or Error.
    void wait_for_hub_reply(const std::vector<std::byte> &frame,
                            const char *action)
    {
        {
            std::lock_guard<std::mutex> guard{connection_mutex_};
            if (!connected_)
            {
                throw_error(std::string{"pipe hub is not connected ("} + action +
                            ")");
            }
            reader_paused_ = true;
            input_.clear();
            have_header_ = false;
            frame_size_ = 0;

            {
                std::lock_guard<std::mutex> ack_guard{ack_mutex_};
                waiting_for_ack_ = true;
                ack_received_ = false;
                ack_error_.clear();
            }

            if (!connection_.write_all(frame))
            {
                reader_paused_ = false;
                std::lock_guard<std::mutex> ack_guard{ack_mutex_};
                waiting_for_ack_ = false;
                throw_error(std::string{"pipe hub "} + action + " failed");
            }
            pump_inbound_until_ack_locked();
            reader_paused_ = false;
        }

        {
            std::lock_guard<std::mutex> ack_guard{ack_mutex_};
            if (!ack_received_ && connected_ && !shutdown_)
            {
                waiting_for_ack_ = false;
                throw_error(std::string{"pipe hub "} + action +
                            " did not receive acknowledgement");
            }
        }

        if (!connected_ || shutdown_)
        {
            waiting_for_ack_ = false;
            throw_error(std::string{"pipe hub disconnected during "} + action);
        }
        waiting_for_ack_ = false;
    }

    void send_hello()
    {
        std::lock_guard<std::mutex> guard{connection_mutex_};
        if (!connected_)
        {
            return;
        }
        const std::vector<std::byte> frame =
            encode_hello(munx::platform_process_id(), client_kind::Vm);
        (void)connection_.write_all(frame);
    }

    void connect_locked()
    {
        std::error_code error;
        std::filesystem::create_directories(pipe_directory(), error);

        for (int attempt = 0; attempt < 400; ++attempt)
        {
            if (try_connect_locked())
            {
                return;
            }
            if (hub_process_alive())
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        for (int attempt = 0; attempt < 400; ++attempt)
        {
            if (try_connect_locked())
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        {
            transport::hub_lock lock;
            if (!lock.try_acquire())
            {
                for (int attempt = 0; attempt < 400; ++attempt)
                {
                    if (try_connect_locked())
                    {
                                                return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
                throw_error("could not connect to pipe hub");
            }

            if (try_connect_locked())
            {
                return;
            }

            for (int attempt = 0; attempt < 40; ++attempt)
            {
                if (try_connect_locked())
                {
                                        return;
                }
                if (hub_process_alive())
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }

            if (!hub_process_alive())
            {
                if (!transport::spawn_hub_daemon(executable_path_))
                {
                    throw_error("could not spawn pipe hub");
                }
            }
        }

        for (int attempt = 0; attempt < 400; ++attempt)
        {
            if (try_connect_locked())
            {
                return;
            }
            if (hub_process_alive())
            {
                for (int wait = 0; wait < 400; ++wait)
                {
                    if (try_connect_locked())
                    {
                                                return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
                throw_error("could not connect to pipe hub");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        throw_error("could not connect to pipe hub");
    }

    bool try_connect_locked()
    {
        auto connection = transport::connect_hub();
        if (!connection.has_value())
        {
            return false;
        }
        connection_ = std::move(*connection);
        connected_ = true;
        return true;
    }

    void start_reader_locked()
    {
        running_ = true;
        reader_ = std::thread([this] { reader_loop(); });
    }

    void reader_loop()
    {
        while (running_ && !shutdown_)
        {
            if (reader_paused_)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // Poll without holding connection_mutex_. Holding the mutex across
            // poll starved publish/attach/disconnect (futex wait vs do_poll).
#if MUNX_PLATFORM_POSIX
            int fd = -1;
            {
                std::lock_guard<std::mutex> guard{connection_mutex_};
                if (!connection_.valid() || shutdown_)
                {
                    break;
                }
                fd = connection_.native_fd();
            }
            pollfd descriptor{fd, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, 100);
            if (shutdown_ || !running_)
            {
                break;
            }
            if (ready < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                break;
            }
            if (ready == 0)
            {
                continue;
            }
#else
            {
                std::lock_guard<std::mutex> guard{connection_mutex_};
                if (!connection_.valid() || shutdown_)
                {
                    break;
                }
                if (!connection_.poll_readable(100))
                {
                    continue;
                }
            }
#endif

            std::byte buffer[4096];
            ssize_t chunk = 0;
            std::vector<message> messages;
            {
                std::lock_guard<std::mutex> guard{connection_mutex_};
                if (reader_paused_ || !connection_.valid() || shutdown_)
                {
                    continue;
                }
#if MUNX_PLATFORM_POSIX
                if (connection_.native_fd() != fd)
                {
                    continue;
                }
#endif
                chunk = connection_.read_some(buffer, sizeof buffer);
                if (chunk < 0)
                {
                    break; // EOF or hard error
                }
                if (chunk == 0)
                {
                    continue; // would-block / EINTR
                }
                input_.insert(input_.end(), buffer, buffer + chunk);
                while (true)
                {
                    auto decoded = try_decode_message_locked();
                    if (!decoded.has_value())
                    {
                        break;
                    }
                    messages.push_back(std::move(*decoded));
                }
            }
            for (const message &decoded : messages)
            {
                handle_message(decoded);
            }
        }

        {
            std::lock_guard<std::mutex> guard{connection_mutex_};
            running_ = false;
            connected_ = false;
        }
        close_all_queues();
        {
            std::lock_guard<std::mutex> ack_guard{ack_mutex_};
            ack_ready_.notify_all();
        }
    }

    std::optional<message> try_decode_message_locked()
    {
        if (!have_header_)
        {
            if (input_.size() < sizeof(uint32_t))
            {
                return std::nullopt;
            }
            // Frame length is a wire uint32; do not memcpy sizeof(size_t).
            uint32_t length = 0;
            std::memcpy(&length, input_.data(), sizeof length);
            frame_size_ = length;
            input_.erase(input_.begin(), input_.begin() + sizeof length);
            have_header_ = true;
        }
        if (input_.size() < frame_size_)
        {
            return std::nullopt;
        }
        const std::span<const std::byte> body{input_.data(), frame_size_};
        message decoded = decode_message(body);
        input_.erase(input_.begin(),
                     input_.begin() + static_cast<std::ptrdiff_t>(frame_size_));
        have_header_ = false;
        frame_size_ = 0;
        return decoded;
    }

    void pump_inbound_until_ack_locked()
    {
        while (true)
        {
            {
                std::lock_guard<std::mutex> ack_guard{ack_mutex_};
                if (ack_received_ || !connected_ || shutdown_)
                {
                    return;
                }
            }

            message decoded;
            if (!read_frame_locked(decoded))
            {
                return;
            }
            handle_message(decoded);

            {
                std::lock_guard<std::mutex> ack_guard{ack_mutex_};
                if (ack_received_ || decoded.kind == opcode::Ack ||
                    decoded.kind == opcode::Error)
                {
                    return;
                }
            }
        }
    }

    bool read_exact_locked(void *data, size_t length)
    {
        auto *bytes = static_cast<std::byte *>(data);
        size_t received = 0;
        while (received < length)
        {
            if (shutdown_ || !connection_.valid() || !connected_)
            {
                return false;
            }
            // When pumping for Ack, wake periodically so a reader-delivered
            // Ack (or Error) can abort a blocking read.
            if (waiting_for_ack_)
            {
                std::lock_guard<std::mutex> ack_guard{ack_mutex_};
                if (ack_received_ || !ack_error_.empty())
                {
                    return false;
                }
            }
            if (!connection_.poll_readable(waiting_for_ack_ ? 50 : 500))
            {
                if (shutdown_ || !connected_)
                {
                    return false;
                }
                continue;
            }
            const ssize_t chunk =
                connection_.read_some(bytes + received, length - received);
            if (chunk < 0)
            {
                connected_ = false;
                return false;
            }
            if (chunk == 0)
            {
                continue; // would-block; poll again
            }
            received += static_cast<size_t>(chunk);
        }
        return true;
    }

    bool read_frame_locked(message &decoded)
    {
        if (!connected_ || shutdown_ || !connection_.valid())
        {
            return false;
        }

        std::byte header[4];
        if (!read_exact_locked(header, sizeof header))
        {
            return false;
        }

        uint32_t length = 0;
        std::memcpy(&length, header, sizeof length);
        if (length == 0)
        {
            decoded = decode_message({});
            return true;
        }

        std::vector<std::byte> body(length);
        if (!read_exact_locked(body.data(), body.size()))
        {
            return false;
        }

        decoded = decode_message(body);
        return true;
    }

    bool read_and_dispatch_inbound_locked()
    {
        if (!connected_ || shutdown_ || !connection_.valid())
        {
            return false;
        }

        std::byte buffer[4096];
        const ssize_t chunk = connection_.read_some(buffer, sizeof buffer);
        if (chunk < 0)
        {
            connected_ = false;
            return false;
        }
        if (chunk == 0)
        {
            return true; // would-block; caller may retry
        }

        input_.insert(input_.end(), buffer, buffer + chunk);
        while (true)
        {
            auto decoded = try_decode_message_locked();
            if (!decoded.has_value())
            {
                break;
            }
            handle_message(*decoded);
        }
        return true;
    }

    void handle_message(const message &decoded)
    {
        if (decoded.kind == opcode::Ack)
        {
            std::lock_guard<std::mutex> guard{ack_mutex_};
            if (!waiting_for_ack_)
            {
                return;
            }
            ack_received_ = true;
            ack_ready_.notify_all();
            return;
        }
        if (decoded.kind == opcode::ChannelAccept ||
            decoded.kind == opcode::ChannelBusy)
        {
            std::lock_guard<std::mutex> guard{ack_mutex_};
            if (!waiting_for_channel_)
            {
                return;
            }
            channel_reply_ = decoded.kind;
            channel_reply_received_ = true;
            ack_ready_.notify_all();
            return;
        }
        if (decoded.kind == opcode::Error)
        {
            std::lock_guard<std::mutex> guard{ack_mutex_};
            if (waiting_for_ack_ || waiting_for_channel_)
            {
                ack_received_ = true;
                channel_reply_received_ = true;
                ack_error_ = decoded.text;
                ack_ready_.notify_all();
            }
            return;
        }
        if (decoded.kind != opcode::Deliver)
        {
            return;
        }

        const detail::queue_key key{decoded.channel, decoded.mode};
        std::shared_ptr<detail::delivery_queue> target;
        {
            std::lock_guard<std::mutex> guard{queues_mutex_};
            auto found = queues_.find(key);
            if (found == queues_.end())
            {
                // Prefer an existing queue for this channel under any inbound mode
                // (defensive: mode mismatch should not drop payloads).
                for (auto &[existing_key, existing_queue] : queues_)
                {
                    if (existing_key.channel == decoded.channel &&
                        (existing_key.mode == attachment_mode::QueueIn ||
                         existing_key.mode == attachment_mode::BroadcastIn ||
                         existing_key.mode == attachment_mode::ChannelPeer))
                    {
                        target = existing_queue;
                        break;
                    }
                }
                if (!target)
                {
                    target = std::make_shared<detail::delivery_queue>();
                    queues_.emplace(key, target);
                }
            }
            else
            {
                target = found->second;
            }
        }

        {
            std::lock_guard<std::mutex> guard{target->mutex};
            target->items.push_back(decoded.payload);
        }
        target->ready.notify_all();
    }

    void close_all_queues()
    {
        std::lock_guard<std::mutex> guard{queues_mutex_};
        for (auto &[key, queue] : queues_)
        {
            (void)key;
            std::lock_guard<std::mutex> queue_guard{queue->mutex};
            queue->closed = true;
        }
        for (auto &[key, queue] : queues_)
        {
            (void)key;
            queue->ready.notify_all();
        }
    }
};

} // namespace munx::vm::pipe_hub
