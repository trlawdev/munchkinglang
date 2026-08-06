#pragma once

#include "pipe_hub_protocol.hpp"
#include "pipe_hub_transport.hpp"
#include "platform.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#if MUNX_PLATFORM_POSIX
#include <cerrno>
#include <poll.h>
#endif

namespace munx::vm::pipe_hub
{

namespace detail
{

using client_id = uint64_t;

struct client_state
{
    transport::hub_connection connection;
    uint64_t pid{0};
    std::vector<std::byte> input;
    size_t frame_size{0};
    bool have_header{false};
    std::vector<std::pair<std::string, attachment_mode>> attachments;
};

struct channel_state
{
    std::vector<client_id> queue_readers;
    std::vector<client_id> broadcast_readers;
    std::vector<client_id> writers;
    std::deque<std::vector<std::byte>> pending;
    struct blocked_publish
    {
        client_id writer_id{0};
        std::vector<std::byte> payload;
    };
    std::deque<blocked_publish> blocked;
    size_t queue_cursor{0};
};

inline void remove_client_from_channel(channel_state &channel, client_id id)
{
    auto erase_id = [id](std::vector<client_id> &list) {
        list.erase(std::remove(list.begin(), list.end(), id), list.end());
    };
    erase_id(channel.queue_readers);
    erase_id(channel.broadcast_readers);
    erase_id(channel.writers);
}

inline bool channel_has_reader(const channel_state &channel)
{
    return !channel.queue_readers.empty() || !channel.broadcast_readers.empty();
}

inline bool deliver_to_client(const transport::hub_connection &connection,
                              const std::vector<std::byte> &frame)
{
    return connection.write_all(frame);
}

inline bool deliver_payload(const transport::hub_connection &connection,
                            std::string_view channel,
                            attachment_mode mode,
                            std::span<const std::byte> payload)
{
    const std::vector<std::byte> frame = encode_deliver(channel, mode, payload);
    return deliver_to_client(connection, frame);
}

struct daemon_state
{
    std::mutex mutex;
    std::unordered_map<client_id, client_state> clients;
    std::unordered_map<std::string, channel_state> channels;
    client_id next_client_id{1};
    bool served_clients{false};
    std::atomic<bool> running{true};

    client_id assign_id(const transport::hub_connection &connection)
    {
#if MUNX_PLATFORM_POSIX
        (void)next_client_id;
        return static_cast<client_id>(connection.native_fd());
#else
        return next_client_id++;
#endif
    }

    void close_client(client_id id)
    {
        auto found = clients.find(id);
        if (found == clients.end())
        {
            return;
        }
        for (auto &[channel_name, channel] : channels)
        {
            channel.blocked.erase(
                std::remove_if(channel.blocked.begin(), channel.blocked.end(),
                               [id](const channel_state::blocked_publish &item) {
                                   return item.writer_id == id;
                               }),
                channel.blocked.end());
        }
        for (const auto &[channel, mode] : found->second.attachments)
        {
            auto channel_it = channels.find(channel);
            if (channel_it != channels.end())
            {
                remove_client_from_channel(channel_it->second, id);
                if (!channel_has_reader(channel_it->second) &&
                    channel_it->second.pending.empty() &&
                    channel_it->second.queue_readers.empty() &&
                    channel_it->second.broadcast_readers.empty() &&
                    channel_it->second.writers.empty())
                {
                    channels.erase(channel_it);
                }
            }
        }
        found->second.connection.close();
        clients.erase(found);
    }

    void flush_blocked(const std::string &channel)
    {
        channel_state &state = channels[channel];
        while (!state.blocked.empty() && channel_has_reader(state))
        {
            channel_state::blocked_publish blocked = std::move(state.blocked.front());
            state.blocked.pop_front();

            for (const client_id reader_id : state.broadcast_readers)
            {
                auto reader = clients.find(reader_id);
                if (reader == clients.end())
                {
                    continue;
                }
                if (!deliver_payload(reader->second.connection, channel,
                                     attachment_mode::BroadcastIn, blocked.payload))
                {
                    close_client(reader_id);
                }
            }
            if (!state.queue_readers.empty())
            {
                const size_t index = state.queue_cursor % state.queue_readers.size();
                state.queue_cursor += 1;
                const client_id reader_id = state.queue_readers[index];
                auto reader = clients.find(reader_id);
                if (reader != clients.end() &&
                    !deliver_payload(reader->second.connection, channel,
                                     attachment_mode::QueueIn, blocked.payload))
                {
                    close_client(reader_id);
                }
            }

            auto writer = clients.find(blocked.writer_id);
            if (writer != clients.end())
            {
                const std::vector<std::byte> ack = encode_ack();
                if (!deliver_to_client(writer->second.connection, ack))
                {
                    close_client(blocked.writer_id);
                }
            }
        }
    }

    void deliver_to_readers(const std::string &channel,
                            std::span<const std::byte> payload)
    {
        channel_state &state = channels[channel];
        for (const client_id reader_id : state.broadcast_readers)
        {
            auto reader = clients.find(reader_id);
            if (reader == clients.end())
            {
                continue;
            }
            if (!deliver_payload(reader->second.connection, channel,
                                 attachment_mode::BroadcastIn, payload))
            {
                close_client(reader_id);
            }
        }
        if (!state.queue_readers.empty())
        {
            const size_t index = state.queue_cursor % state.queue_readers.size();
            state.queue_cursor += 1;
            const client_id reader_id = state.queue_readers[index];
            auto reader = clients.find(reader_id);
            if (reader != clients.end() &&
                !deliver_payload(reader->second.connection, channel,
                                 attachment_mode::QueueIn, payload))
            {
                close_client(reader_id);
            }
        }
    }

    void register_attachment(client_id id, const std::string &channel,
                             attachment_mode mode)
    {
        client_state &client = clients.at(id);
        client.attachments.emplace_back(channel, mode);
        channel_state &state = channels[channel];
        switch (mode)
        {
        case attachment_mode::Writer:
            state.writers.push_back(id);
            break;
        case attachment_mode::BroadcastIn:
            state.broadcast_readers.push_back(id);
            flush_blocked(channel);
            break;
        case attachment_mode::QueueIn:
            state.queue_readers.push_back(id);
            while (!state.pending.empty() && !state.queue_readers.empty())
            {
                const std::vector<std::byte> payload = std::move(state.pending.front());
                state.pending.pop_front();
                const size_t index = state.queue_cursor % state.queue_readers.size();
                state.queue_cursor += 1;
                const client_id reader_id = state.queue_readers[index];
                auto reader = clients.find(reader_id);
                if (reader != clients.end() &&
                    !deliver_payload(reader->second.connection, channel,
                                     attachment_mode::QueueIn, payload))
                {
                    close_client(reader_id);
                }
            }
            flush_blocked(channel);
            break;
        }
    }

    void unregister_attachment(client_id id, const std::string &channel,
                               attachment_mode mode)
    {
        client_state &client = clients.at(id);
        client.attachments.erase(
            std::remove(client.attachments.begin(), client.attachments.end(),
                        std::pair<std::string, attachment_mode>{channel, mode}),
            client.attachments.end());
        auto channel_it = channels.find(channel);
        if (channel_it != channels.end())
        {
            remove_client_from_channel(channel_it->second, id);
        }
    }

    bool publish_message(client_id writer_id, const std::string &channel,
                         const std::vector<std::byte> &payload)
    {
        channel_state &state = channels[channel];
        if (!channel_has_reader(state))
        {
            state.blocked.push_back(channel_state::blocked_publish{writer_id, payload});
            auto writer = clients.find(writer_id);
            if (writer != clients.end())
            {
                const std::vector<std::byte> ack = encode_ack();
                if (!deliver_to_client(writer->second.connection, ack))
                {
                    close_client(writer_id);
                    return false;
                }
            }
            return true;
        }

        deliver_to_readers(channel, payload);
        auto writer = clients.find(writer_id);
        if (writer == clients.end())
        {
            return false;
        }
        const std::vector<std::byte> ack = encode_ack();
        if (!deliver_to_client(writer->second.connection, ack))
        {
            close_client(writer_id);
            return false;
        }
        return true;
    }

    bool handle_message(client_id id, const message &decoded)
    {
        switch (decoded.kind)
        {
        case opcode::Hello:
            clients.at(id).pid = decoded.pid;
            return true;
        case opcode::Bye:
            close_client(id);
            return false;
        case opcode::Attach:
            register_attachment(id, decoded.channel, decoded.mode);
            return true;
        case opcode::Detach:
            unregister_attachment(id, decoded.channel, decoded.mode);
            return true;
        case opcode::Publish:
            return publish_message(id, decoded.channel, std::move(decoded.payload));
        default:
            return true;
        }
    }

    void process_client_input(client_id id)
    {
        client_state &client = clients.at(id);
        while (true)
        {
            if (!client.have_header)
            {
                if (client.input.size() < sizeof(uint32_t))
                {
                    return;
                }
                uint32_t length = 0;
                std::memcpy(&length, client.input.data(), sizeof length);
                client.frame_size = length;
                client.input.erase(client.input.begin(),
                                   client.input.begin() + sizeof(uint32_t));
                client.have_header = true;
            }
            if (client.input.size() < client.frame_size)
            {
                return;
            }
            const std::span<const std::byte> body{client.input.data(), client.frame_size};
            protocol_error_flag = false;
            message decoded = decode_message(body);
            if (protocol_error_flag)
            {
                close_client(id);
                return;
            }
            client.input.erase(client.input.begin(),
                               client.input.begin() +
                                   static_cast<std::ptrdiff_t>(client.frame_size));
            client.have_header = false;
            client.frame_size = 0;
            if (!handle_message(id, decoded))
            {
                return;
            }
        }
    }

    void ingest_bytes(client_id id, const std::byte *data, size_t length)
    {
        client_state &client = clients.at(id);
        client.input.insert(client.input.end(), data, data + length);
        process_client_input(id);
    }
};

#if MUNX_PLATFORM_WINDOWS

inline void run_client_thread(daemon_state *state, client_id id)
{
    while (state->running.load())
    {
        std::byte buffer[4096];
        ssize_t chunk = 0;
        {
            std::lock_guard<std::mutex> guard{state->mutex};
            auto found = state->clients.find(id);
            if (found == state->clients.end())
            {
                return;
            }
            chunk = found->second.connection.read_some(buffer, sizeof buffer);
            if (chunk < 0)
            {
                state->close_client(id);
                return;
            }
            if (chunk == 0)
            {
                continue;
            }
            state->ingest_bytes(id, buffer, static_cast<size_t>(chunk));
        }
    }
}

inline int run_daemon_windows()
{
    daemon_state state;
    transport::hub_listener listener;
    if (!listener.open())
    {
        return 1;
    }

    std::vector<std::thread> client_threads;
    while (state.running.load())
    {
        if (state.served_clients)
        {
            std::lock_guard<std::mutex> guard{state.mutex};
            if (state.clients.empty())
            {
                break;
            }
        }

        auto connection = listener.accept(500);
        if (!connection.has_value())
        {
            continue;
        }

        client_id id = 0;
        {
            std::lock_guard<std::mutex> guard{state.mutex};
            state.served_clients = true;
            id = state.assign_id(*connection);
            state.clients.emplace(id, client_state{std::move(*connection)});
        }
        client_threads.emplace_back(run_client_thread, &state, id);
    }

    state.running = false;
    for (std::thread &thread : client_threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    listener.close();
    return 0;
}

#endif

#if MUNX_PLATFORM_POSIX

inline int run_daemon_posix()
{
    daemon_state state;
    transport::hub_listener listener;
    if (!listener.open())
    {
        return 1;
    }

    while (true)
    {
        std::vector<pollfd> pollfds;
        pollfds.push_back(pollfd{listener.native_fd(), POLLIN, 0});
        for (const auto &[id, client] : state.clients)
        {
            (void)client;
            pollfds.push_back(pollfd{static_cast<int>(id), POLLIN, 0});
        }

        const int ready = ::poll(pollfds.data(),
                                 static_cast<nfds_t>(pollfds.size()), 500);
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        if (pollfds[0].revents & POLLIN)
        {
            auto connection = listener.accept();
            if (connection.has_value())
            {
                const client_id id = state.assign_id(*connection);
                state.served_clients = true;
                client_state client{};
                client.connection = std::move(*connection);
                state.clients.emplace(id, std::move(client));
            }
        }

        for (size_t index = 1; index < pollfds.size(); ++index)
        {
            const client_id id = static_cast<client_id>(pollfds[index].fd);
            if (pollfds[index].revents == 0)
            {
                continue;
            }
            if (pollfds[index].revents & (POLLERR | POLLHUP | POLLNVAL))
            {
                state.close_client(id);
                continue;
            }
            if (pollfds[index].revents & POLLIN)
            {
                auto found = state.clients.find(id);
                if (found == state.clients.end())
                {
                    continue;
                }
                std::byte buffer[4096];
                const ssize_t chunk =
                    found->second.connection.read_some(buffer, sizeof buffer);
                if (chunk <= 0)
                {
                    state.close_client(id);
                    continue;
                }
                state.ingest_bytes(id, buffer, static_cast<size_t>(chunk));
            }
        }

        if (state.served_clients && state.clients.empty())
        {
            break;
        }
    }

    listener.close();
    return 0;
}

#endif

} // namespace detail

/// Run the pipe hub daemon until every client disconnects.
inline int run_daemon()
{
    if (hub_process_alive())
    {
        return 0;
    }

    std::error_code error;
    std::filesystem::create_directories(pipe_directory(), error);
    if (error)
    {
        return 1;
    }

    transport::hub_lock lock;
    if (!lock.try_acquire())
    {
        return 0;
    }

    if (hub_process_alive())
    {
        return 0;
    }

    {
        std::ofstream pid_file{hub_pid_path()};
        if (pid_file)
        {
            pid_file << munx::platform_process_id() << '\n';
        }
    }

    int result = 0;
#if MUNX_PLATFORM_WINDOWS
    result = detail::run_daemon_windows();
#else
    result = detail::run_daemon_posix();
#endif

    std::filesystem::remove(hub_pid_path(), error);
    transport::cleanup_hub_endpoint();
    return result;
}

} // namespace munx::vm::pipe_hub
