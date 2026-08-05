#pragma once

#include "pipe_hub_protocol.hpp"
#include "pipe_hub_transport.hpp"
#include "platform.hpp"
#include "vm_value.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

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
        std::lock_guard<std::mutex> guard{connection_mutex_};
        if (connected_)
        {
            return;
        }
        connect_locked();
    }

    void disconnect()
    {
        std::thread reader;
        {
            std::lock_guard<std::mutex> guard{connection_mutex_};
            if (!connected_)
            {
                return;
            }
            shutdown_ = true;
            if (connection_.valid())
            {
                const std::vector<std::byte> frame = encode_bye();
                connection_.write_all(frame);
                connection_.shutdown_rw();
                connection_.close();
            }
            connected_ = false;
            running_ = false;
            reader = std::move(reader_);
        }
        {
            std::lock_guard<std::mutex> ack_guard{ack_mutex_};
            ack_ready_.notify_all();
        }
        if (reader.joinable())
        {
            reader.join();
        }
        close_all_queues();
        shutdown_ = false;
    }

    void attach(const std::string &channel, attachment_mode mode)
    {
        ensure_connected();
        if (mode == attachment_mode::QueueIn ||
            mode == attachment_mode::BroadcastIn)
        {
            std::lock_guard<std::mutex> guard{queues_mutex_};
            const detail::queue_key key{channel, mode};
            if (!queues_.contains(key))
            {
                queues_.emplace(key, std::make_shared<detail::delivery_queue>());
            }
        }
        std::lock_guard<std::mutex> guard{connection_mutex_};
        if (!connected_)
        {
            throw_error("pipe hub is not connected");
        }
        const std::vector<std::byte> frame = encode_attach(channel, mode);
        if (!connection_.write_all(frame))
        {
            throw_error("pipe hub attach failed");
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
            connection_.write_all(frame);
        }
        if (mode == attachment_mode::QueueIn ||
            mode == attachment_mode::BroadcastIn)
        {
            std::lock_guard<std::mutex> guard{queues_mutex_};
            queues_.erase(detail::queue_key{channel, mode});
        }
    }

    void publish(const std::string &channel, std::span<const std::byte> payload)
    {
        ensure_connected();
        const std::vector<std::byte> frame = encode_publish(channel, payload);

        {
            std::lock_guard<std::mutex> guard{connection_mutex_};
            if (!connected_)
            {
                throw_error("pipe hub is not connected");
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
                throw_error("pipe hub publish failed");
            }
            pump_inbound_until_ack_locked();
            reader_paused_ = false;
        }

        {
            std::lock_guard<std::mutex> ack_guard{ack_mutex_};
            if (!ack_received_ && connected_ && !shutdown_)
            {
                waiting_for_ack_ = false;
                throw_error("pipe hub publish did not receive acknowledgement");
            }
        }

        if (!ack_error_.empty())
        {
            throw_error("pipe hub error: " + ack_error_);
        }
        if (!connected_ || shutdown_)
        {
            throw_error("pipe hub disconnected during publish");
        }
        waiting_for_ack_ = false;
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
        session()
        {
            if (hub_enabled())
            {
                client::instance().ensure_connected();
                client::instance().send_hello();
            }
        }

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

    void send_hello()
    {
        std::lock_guard<std::mutex> guard{connection_mutex_};
        if (!connected_)
        {
            return;
        }
        const std::vector<std::byte> frame =
            encode_hello(munx::platform_process_id());
        connection_.write_all(frame);
    }

    void connect_locked()
    {
        std::error_code error;
        std::filesystem::create_directories(pipe_directory(), error);

        for (int attempt = 0; attempt < 400; ++attempt)
        {
            if (try_connect_locked())
            {
                start_reader_locked();
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
                start_reader_locked();
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
                        start_reader_locked();
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
                throw_error("could not connect to pipe hub");
            }

            if (try_connect_locked())
            {
                start_reader_locked();
                return;
            }

            for (int attempt = 0; attempt < 40; ++attempt)
            {
                if (try_connect_locked())
                {
                    start_reader_locked();
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
                start_reader_locked();
                return;
            }
            if (hub_process_alive())
            {
                for (int wait = 0; wait < 400; ++wait)
                {
                    if (try_connect_locked())
                    {
                        start_reader_locked();
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
        while (running_)
        {
            if (reader_paused_)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            std::byte buffer[4096];
            ssize_t chunk = 0;
            std::vector<message> messages;
            {
                std::lock_guard<std::mutex> guard{connection_mutex_};
                if (!connection_.valid())
                {
                    break;
                }
                if (!connection_.poll_readable(200))
                {
                    continue;
                }
                chunk = connection_.read_some(buffer, sizeof buffer);
                if (chunk <= 0)
                {
                    break;
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
            std::memcpy(&frame_size_, input_.data(), sizeof frame_size_);
            input_.erase(input_.begin(), input_.begin() + sizeof(uint32_t));
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
            const ssize_t chunk =
                connection_.read_some(bytes + received, length - received);
            if (chunk <= 0)
            {
                connected_ = false;
                return false;
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
            connected_ = false;
            return false;
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
        if (decoded.kind == opcode::Error)
        {
            std::lock_guard<std::mutex> guard{ack_mutex_};
            if (!waiting_for_ack_)
            {
                return;
            }
            ack_received_ = true;
            ack_error_ = decoded.text;
            ack_ready_.notify_all();
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
                target = std::make_shared<detail::delivery_queue>();
                queues_.emplace(key, target);
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
