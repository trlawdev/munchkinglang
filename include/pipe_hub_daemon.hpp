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
    client_kind kind{client_kind::Unknown};
    std::vector<std::byte> input;
    size_t frame_size{0};
    bool have_header{false};
    std::vector<std::pair<std::string, attachment_mode>> attachments;
};

/// Per pipe_id (channel name) routing + backlog.
/// Publishes with no live readers are stored in @c pending until a reader
/// attaches (readers may only attach while at least one Writer is present).
struct channel_state
{
    static constexpr size_t max_pending = 4096;

    std::vector<client_id> queue_readers;
    std::vector<client_id> broadcast_readers;
    std::vector<client_id> writers;
    /// Bidirectional channel peers (at most two).
    std::vector<client_id> channel_peers;
    /// Backlog indexed by pipe_id (this map key): payloads awaiting readers.
    std::deque<std::vector<std::byte>> pending;
    size_t queue_cursor{0};
    /// Channel handshake bookkeeping.
    client_id offer_from{0};
    bool have_offer{false};
    client_id recv_ready{0};
    bool have_recv_ready{false};
    client_id awaiting_data_ack{0};
    bool have_awaiting_data_ack{false};
};

inline void remove_client_from_channel(channel_state &channel, client_id id)
{
    auto erase_id = [id](std::vector<client_id> &list) {
        list.erase(std::remove(list.begin(), list.end(), id), list.end());
    };
    erase_id(channel.queue_readers);
    erase_id(channel.broadcast_readers);
    erase_id(channel.writers);
    erase_id(channel.channel_peers);
    if (channel.have_offer && channel.offer_from == id)
    {
        channel.have_offer = false;
        channel.offer_from = 0;
    }
    if (channel.have_recv_ready && channel.recv_ready == id)
    {
        channel.have_recv_ready = false;
        channel.recv_ready = 0;
    }
    if (channel.have_awaiting_data_ack && channel.awaiting_data_ack == id)
    {
        channel.have_awaiting_data_ack = false;
        channel.awaiting_data_ack = 0;
    }
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
        (void)connection;
        return next_client_id++;
#endif
    }

    void maybe_erase_channel(const std::string &channel)
    {
        auto channel_it = channels.find(channel);
        if (channel_it == channels.end())
        {
            return;
        }
        const channel_state &state = channel_it->second;
        if (!channel_has_reader(state) && state.pending.empty() &&
            state.writers.empty() && state.channel_peers.empty())
        {
            channels.erase(channel_it);
        }
    }

    void close_client(client_id id)
    {
        auto found = clients.find(id);
        if (found == clients.end())
        {
            return;
        }
        for (const auto &[channel, mode] : found->second.attachments)
        {
            auto channel_it = channels.find(channel);
            if (channel_it != channels.end())
            {
                remove_client_from_channel(channel_it->second, id);
                maybe_erase_channel(channel);
            }
        }
        found->second.connection.close();
        clients.erase(found);
    }

    /// Push queued payloads for @p channel to currently attached readers.
    void flush_pending(const std::string &channel)
    {
        channel_state &state = channels[channel];
        while (!state.pending.empty() && channel_has_reader(state))
        {
            const std::vector<std::byte> payload = std::move(state.pending.front());
            state.pending.pop_front();
            deliver_to_readers(channel, payload);
        }
    }

    bool send_ack(client_id id)
    {
        auto found = clients.find(id);
        if (found == clients.end())
        {
            return false;
        }
        if (!deliver_to_client(found->second.connection, encode_ack()))
        {
            close_client(id);
            return false;
        }
        return true;
    }

    bool send_error(client_id id, std::string_view message)
    {
        auto found = clients.find(id);
        if (found == clients.end())
        {
            return false;
        }
        if (!deliver_to_client(found->second.connection, encode_error(message)))
        {
            close_client(id);
            return false;
        }
        return true;
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

    /// @return true when Attach succeeded (Ack already sent).
    bool register_attachment(client_id id, const std::string &channel,
                             attachment_mode mode)
    {
        channel_state &state = channels[channel];

        // Readers may only join a pipe that already has a live publisher.
        if ((mode == attachment_mode::QueueIn ||
             mode == attachment_mode::BroadcastIn) &&
            state.writers.empty())
        {
            send_error(id, "pipe `" + channel +
                               "` has no publisher; open pipe(name, out) first");
            maybe_erase_channel(channel);
            return false;
        }
        if (mode == attachment_mode::ChannelPeer &&
            state.channel_peers.size() >= 2)
        {
            send_error(id, "channel `" + channel +
                               "` already has two peers");
            maybe_erase_channel(channel);
            return false;
        }

        client_state &client = clients.at(id);
        client.attachments.emplace_back(channel, mode);
        switch (mode)
        {
        case attachment_mode::Writer:
            state.writers.push_back(id);
            break;
        case attachment_mode::BroadcastIn:
            state.broadcast_readers.push_back(id);
            break;
        case attachment_mode::QueueIn:
            state.queue_readers.push_back(id);
            break;
        case attachment_mode::ChannelPeer:
            state.channel_peers.push_back(id);
            break;
        }
        // Ack before draining backlog so attach cannot deadlock behind a full
        // socket buffer while the client is still waiting for Attach's Ack.
        if (!send_ack(id))
        {
            return false;
        }
        if (mode == attachment_mode::BroadcastIn ||
            mode == attachment_mode::QueueIn)
        {
            flush_pending(channel);
        }
        else if (mode == attachment_mode::ChannelPeer)
        {
            while (!state.pending.empty())
            {
                const std::vector<std::byte> payload =
                    std::move(state.pending.front());
                state.pending.pop_front();
                if (!deliver_payload(client.connection, channel,
                                     attachment_mode::ChannelPeer, payload))
                {
                    close_client(id);
                    return false;
                }
            }
        }
        return true;
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
            maybe_erase_channel(channel);
        }
    }

    bool publish_message(client_id writer_id, const std::string &channel,
                         const std::vector<std::byte> &payload)
    {
        channel_state &state = channels[channel];

        // Channel peer data path: deliver to every other peer; backlog if none.
        if (std::find(state.channel_peers.begin(), state.channel_peers.end(),
                      writer_id) != state.channel_peers.end())
        {
            bool delivered = false;
            for (const client_id peer : state.channel_peers)
            {
                if (peer == writer_id)
                {
                    continue;
                }
                auto peer_it = clients.find(peer);
                if (peer_it == clients.end())
                {
                    continue;
                }
                if (deliver_payload(peer_it->second.connection, channel,
                                    attachment_mode::ChannelPeer, payload))
                {
                    delivered = true;
                }
            }
            if (!delivered)
            {
                if (state.pending.size() >= channel_state::max_pending)
                {
                    return send_error(writer_id,
                                      "channel `" + channel +
                                          "` pending queue is full");
                }
                state.pending.push_back(payload);
                // If a peer already signalled RecvReady, push immediately.
                if (state.have_recv_ready && state.recv_ready != writer_id)
                {
                    auto ready = clients.find(state.recv_ready);
                    if (ready != clients.end())
                    {
                        while (!state.pending.empty())
                        {
                            const std::vector<std::byte> item =
                                std::move(state.pending.front());
                            state.pending.pop_front();
                            if (!deliver_payload(ready->second.connection, channel,
                                                 attachment_mode::ChannelPeer,
                                                 item))
                            {
                                break;
                            }
                            delivered = true;
                        }
                    }
                }
            }
            return send_ack(writer_id);
        }

        if (!channel_has_reader(state))
        {
            if (state.pending.size() >= channel_state::max_pending)
            {
                return send_error(writer_id,
                                  "pipe `" + channel + "` pending queue is full");
            }
            state.pending.push_back(payload);
            return send_ack(writer_id);
        }

        deliver_to_readers(channel, payload);
        return send_ack(writer_id);
    }

    client_id other_channel_peer(const channel_state &state, client_id id) const
    {
        for (const client_id candidate : state.channel_peers)
        {
            if (candidate != id)
            {
                return candidate;
            }
        }
        return 0;
    }

    bool handle_channel_offer(client_id id, const std::string &channel)
    {
        channel_state &state = channels[channel];
        if (std::find(state.channel_peers.begin(), state.channel_peers.end(),
                      id) == state.channel_peers.end())
        {
            return send_error(id, "not attached to channel `" + channel + "`");
        }
        if (state.have_offer && state.offer_from != id)
        {
            // Collision: both peers tried to send at once.
            const client_id other = state.offer_from;
            state.have_offer = false;
            state.offer_from = 0;
            (void)deliver_to_client(
                clients.at(id).connection,
                encode_channel_op(opcode::ChannelBusy, channel));
            if (clients.contains(other))
            {
                (void)deliver_to_client(
                    clients.at(other).connection,
                    encode_channel_op(opcode::ChannelBusy, channel));
            }
            return true;
        }
        // Grant when the peer is attached and/or has announced RecvReady.
        // Parking only when the other peer is missing avoids a sender/receiver
        // lockstep stall on the second message of a dialogue.
        const client_id peer = other_channel_peer(state, id);
        if (peer != 0 || (state.have_recv_ready && state.recv_ready != id))
        {
            state.have_recv_ready = false;
            state.recv_ready = 0;
            state.have_offer = false;
            state.offer_from = 0;
            return deliver_to_client(
                clients.at(id).connection,
                encode_channel_op(opcode::ChannelAccept, channel));
        }
        state.have_offer = true;
        state.offer_from = id;
        return true;
    }

    bool handle_channel_recv_ready(client_id id, const std::string &channel)
    {
        channel_state &state = channels[channel];
        if (std::find(state.channel_peers.begin(), state.channel_peers.end(),
                      id) == state.channel_peers.end())
        {
            return send_error(id, "not attached to channel `" + channel + "`");
        }
        if (state.have_offer && state.offer_from != id)
        {
            const client_id offerer = state.offer_from;
            state.have_offer = false;
            state.offer_from = 0;
            (void)deliver_to_client(
                clients.at(offerer).connection,
                encode_channel_op(opcode::ChannelAccept, channel));
        }
        // Drain backlog onto this receiver.
        auto self = clients.find(id);
        while (self != clients.end() && !state.pending.empty())
        {
            const std::vector<std::byte> payload = std::move(state.pending.front());
            state.pending.pop_front();
            if (!deliver_payload(self->second.connection, channel,
                                 attachment_mode::ChannelPeer, payload))
            {
                close_client(id);
                return false;
            }
        }
        state.have_recv_ready = true;
        state.recv_ready = id;
        return true;
    }

    bool handle_channel_data_ack(client_id id, const std::string &channel)
    {
        // Optional receipt ack from the receiver; sender is already Ack'd after
        // Deliver so this is informational / cleanup only.
        (void)id;
        channel_state &state = channels[channel];
        state.have_awaiting_data_ack = false;
        state.awaiting_data_ack = 0;
        return true;
    }

    bool handle_message(client_id id, const message &decoded)
    {
        switch (decoded.kind)
        {
        case opcode::Hello:
            clients.at(id).pid = decoded.pid;
            clients.at(id).kind = decoded.client;
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
            return publish_message(id, decoded.channel, decoded.payload);
        case opcode::ChannelOffer:
            return handle_channel_offer(id, decoded.channel);
        case opcode::ChannelRecvReady:
            return handle_channel_recv_ready(id, decoded.channel);
        case opcode::ChannelDataAck:
            return handle_channel_data_ack(id, decoded.channel);
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
                continue; // would-block
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
            {
                client_state state_client;
                state_client.connection = std::move(*connection);
                state.clients.emplace(id, std::move(state_client));
            }
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
                if (chunk < 0)
                {
                    state.close_client(id);
                    continue;
                }
                if (chunk == 0)
                {
                    continue; // would-block
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

/// Run the pipe hub daemon until every attached client disconnects.
/// Clients may be munx VM processes, natively compiled munx binaries, or
/// external tools that speak the hub wire protocol.
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
