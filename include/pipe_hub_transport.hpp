#pragma once

#include "pipe_hub_protocol.hpp"
#include "platform.hpp"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#if MUNX_PLATFORM_POSIX
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#elif MUNX_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace munx::vm::pipe_hub
{

namespace transport
{

#if MUNX_PLATFORM_WINDOWS
inline std::string hub_pipe_name()
{
    const std::string directory = pipe_directory().string();
    const size_t hash = std::hash<std::string>{}(directory);
    return "\\\\.\\pipe\\munx-hub-" + std::to_string(hash);
}
#endif

inline bool hub_endpoint_ready()
{
#if MUNX_PLATFORM_POSIX
    std::error_code error;
    return std::filesystem::exists(hub_socket_path(), error);
#else
    return WaitNamedPipeA(hub_pipe_name().c_str(), 0) != 0 ||
           GetLastError() == ERROR_PIPE_BUSY;
#endif
}

#if MUNX_PLATFORM_POSIX
inline void set_cloexec_nonblock(int fd)
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
    {
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    flags = ::fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
    {
        (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}
#endif

/// Connected hub endpoint. On POSIX this is an AF_UNIX SOCK_STREAM socket.
class hub_connection
{
public:
    hub_connection() = default;
    ~hub_connection() { close(); }

    hub_connection(const hub_connection &) = delete;
    hub_connection &operator=(const hub_connection &) = delete;

    hub_connection(hub_connection &&other) noexcept { move_from(std::move(other)); }
    hub_connection &operator=(hub_connection &&other) noexcept
    {
        if (this != &other)
        {
            close();
            move_from(std::move(other));
        }
        return *this;
    }

#if MUNX_PLATFORM_POSIX
    explicit hub_connection(int fd) : fd_{fd} {}
    [[nodiscard]] int native_fd() const { return fd_; }
#elif MUNX_PLATFORM_WINDOWS
    explicit hub_connection(HANDLE handle) : handle_{handle} {}
    [[nodiscard]] HANDLE native_handle() const { return handle_; }
#endif

    [[nodiscard]] bool valid() const
    {
#if MUNX_PLATFORM_POSIX
        return fd_ >= 0;
#else
        return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
#endif
    }

    void close()
    {
#if MUNX_PLATFORM_POSIX
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
#else
        if (valid())
        {
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#endif
    }

    void shutdown_rw()
    {
#if MUNX_PLATFORM_POSIX
        if (fd_ >= 0)
        {
            ::shutdown(fd_, SHUT_RDWR);
        }
#else
        if (valid())
        {
            ::FlushFileBuffers(handle_);
        }
#endif
    }

    [[nodiscard]] bool write_all(std::span<const std::byte> data) const
    {
        if (!valid())
        {
            return false;
        }
        size_t written = 0;
        while (written < data.size())
        {
#if MUNX_PLATFORM_POSIX
            const ssize_t chunk =
                ::write(fd_, data.data() + written, data.size() - written);
            if (chunk < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    pollfd out{fd_, POLLOUT, 0};
                    if (::poll(&out, 1, 500) <= 0)
                    {
                        return false;
                    }
                    continue;
                }
                return false;
            }
            if (chunk == 0)
            {
                return false;
            }
            written += static_cast<size_t>(chunk);
#else
            DWORD chunk = 0;
            if (!::WriteFile(handle_, data.data() + written,
                             static_cast<DWORD>(data.size() - written), &chunk,
                             nullptr))
            {
                return false;
            }
            if (chunk == 0)
            {
                return false;
            }
            written += chunk;
#endif
        }
        return true;
    }

    /// @return >0 bytes read; 0 = would-block / interrupt (retry); -1 = EOF or error.
    [[nodiscard]] ssize_t read_some(void *buffer, size_t length) const
    {
        if (!valid() || length == 0)
        {
            return -1;
        }
#if MUNX_PLATFORM_POSIX
        const ssize_t chunk = ::read(fd_, buffer, length);
        if (chunk < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return 0;
            }
            return -1;
        }
        if (chunk == 0)
        {
            return -1; // peer closed
        }
        return chunk;
#else
        DWORD received = 0;
        if (!::ReadFile(handle_, buffer, static_cast<DWORD>(length), &received,
                        nullptr))
        {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE)
            {
                return -1;
            }
            if (error == ERROR_NO_DATA)
            {
                return 0;
            }
            return -1;
        }
        if (received == 0)
        {
            return -1;
        }
        return static_cast<ssize_t>(received);
#endif
    }

#if MUNX_PLATFORM_POSIX
    [[nodiscard]] bool set_nonblocking() const
    {
        if (!valid())
        {
            return false;
        }
        set_cloexec_nonblock(fd_);
        return true;
    }
#endif

    [[nodiscard]] bool poll_readable(int timeout_ms) const
    {
#if MUNX_PLATFORM_POSIX
        if (!valid())
        {
            return false;
        }
        pollfd descriptor{fd_, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, timeout_ms);
        if (ready <= 0)
        {
            return false;
        }
        // POLLHUP/ERR must wake the reader so it can observe EOF and exit;
        // otherwise disconnect() can hang joining a reader stuck in poll.
        return (descriptor.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) !=
               0;
#else
        (void)timeout_ms;
        return valid();
#endif
    }

private:
    void move_from(hub_connection &&other) noexcept
    {
#if MUNX_PLATFORM_POSIX
        fd_ = other.fd_;
        other.fd_ = -1;
#else
        handle_ = other.handle_;
        other.handle_ = INVALID_HANDLE_VALUE;
#endif
    }

#if MUNX_PLATFORM_POSIX
    int fd_{-1};
#else
    HANDLE handle_{INVALID_HANDLE_VALUE};
#endif
};

class hub_listener
{
public:
    hub_listener() = default;
    ~hub_listener() { close(); }

    hub_listener(const hub_listener &) = delete;
    hub_listener &operator=(const hub_listener &) = delete;

    [[nodiscard]] bool open()
    {
#if MUNX_PLATFORM_POSIX
        std::error_code error;
        std::filesystem::remove(hub_socket_path(), error);

        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
        {
            return false;
        }

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const std::string path = hub_socket_path().string();
        if (path.size() >= sizeof(address.sun_path))
        {
            ::close(fd);
            return false;
        }
        std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

        if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0 ||
            ::listen(fd, 64) != 0)
        {
            ::close(fd);
            return false;
        }
        set_cloexec_nonblock(fd);
        listen_fd_ = fd;
        return true;
#else
        pipe_name_ = hub_pipe_name();
        active_ = true;
        return true;
#endif
    }

    /// Accept a client connection. @p timeout_ms: -1 = block, 0 = try once, >0 = wait up to N ms.
    [[nodiscard]] std::optional<hub_connection> accept(int timeout_ms = -1) const
    {
#if MUNX_PLATFORM_POSIX
        (void)timeout_ms;
        if (listen_fd_ < 0)
        {
            return std::nullopt;
        }
        const int client_fd = ::accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0)
        {
            return std::nullopt;
        }
        hub_connection connection{client_fd};
        (void)connection.set_nonblocking();
        return connection;
#else
        if (!active_)
        {
            return std::nullopt;
        }
        const HANDLE handle = ::CreateNamedPipeA(
            pipe_name_.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
            65536, 65536, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            return std::nullopt;
        }

        OVERLAPPED overlapped{};
        overlapped.hEvent = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (overlapped.hEvent == nullptr)
        {
            ::CloseHandle(handle);
            return std::nullopt;
        }

        const BOOL connect_started = ::ConnectNamedPipe(handle, &overlapped);
        const DWORD connect_error = ::GetLastError();
        bool connected = connect_started != FALSE;
        if (!connected && connect_error == ERROR_PIPE_CONNECTED)
        {
            connected = true;
        }
        else if (!connected && connect_error == ERROR_IO_PENDING)
        {
            const DWORD wait_ms =
                timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms);
            const DWORD wait_result =
                ::WaitForSingleObject(overlapped.hEvent, wait_ms);
            if (wait_result == WAIT_OBJECT_0)
            {
                DWORD transferred = 0;
                connected = ::GetOverlappedResult(handle, &overlapped, &transferred,
                                                  FALSE) != FALSE;
            }
            else
            {
                ::CancelIo(handle);
                ::CloseHandle(handle);
                ::CloseHandle(overlapped.hEvent);
                return std::nullopt;
            }
        }
        else if (!connected)
        {
            ::CloseHandle(handle);
            ::CloseHandle(overlapped.hEvent);
            return std::nullopt;
        }

        ::CloseHandle(overlapped.hEvent);
        return hub_connection{handle};
#endif
    }

    void close()
    {
#if MUNX_PLATFORM_POSIX
        if (listen_fd_ >= 0)
        {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
#else
        active_ = false;
#endif
    }

#if MUNX_PLATFORM_POSIX
    [[nodiscard]] int native_fd() const { return listen_fd_; }
#endif

private:
#if MUNX_PLATFORM_POSIX
    int listen_fd_{-1};
#else
    std::string pipe_name_;
    bool active_{false};
#endif
};

class hub_lock
{
public:
    hub_lock() = default;

    [[nodiscard]] bool try_acquire()
    {
        release();
#if MUNX_PLATFORM_POSIX
        const int fd = ::open(hub_lock_path().string().c_str(), O_CREAT | O_RDWR, 0666);
        if (fd < 0)
        {
            return false;
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
        {
            ::close(fd);
            return false;
        }
        lock_fd_ = fd;
        return true;
#else
        const size_t directory_hash =
            std::hash<std::string>{}(pipe_directory().string());
        const std::string name =
            "Global\\munx-hub-lock-" + std::to_string(directory_hash);
        HANDLE handle = ::CreateMutexA(nullptr, FALSE, name.c_str());
        if (handle == nullptr)
        {
            return false;
        }
        if (::WaitForSingleObject(handle, 0) != WAIT_OBJECT_0)
        {
            ::CloseHandle(handle);
            return false;
        }
        mutex_ = handle;
        return true;
#endif
    }

    void release()
    {
#if MUNX_PLATFORM_POSIX
        if (lock_fd_ >= 0)
        {
            ::flock(lock_fd_, LOCK_UN);
            ::close(lock_fd_);
            lock_fd_ = -1;
        }
#else
        if (mutex_ != nullptr)
        {
            ::ReleaseMutex(mutex_);
            ::CloseHandle(mutex_);
            mutex_ = nullptr;
        }
#endif
    }

    ~hub_lock() { release(); }

private:
#if MUNX_PLATFORM_POSIX
    int lock_fd_{-1};
#else
    HANDLE mutex_{nullptr};
#endif
};

inline bool platform_process_alive(uint64_t pid)
{
    if (pid == 0)
    {
        return false;
    }
#if MUNX_PLATFORM_WINDOWS
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                   static_cast<DWORD>(pid));
    if (process == nullptr)
    {
        return false;
    }
    DWORD exit_code = 0;
    const BOOL ok = ::GetExitCodeProcess(process, &exit_code);
    ::CloseHandle(process);
    return ok && exit_code == STILL_ACTIVE;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

inline bool hub_process_alive()
{
    std::error_code error;
    const std::filesystem::path pid_path = hub_pid_path();
    if (!std::filesystem::exists(pid_path, error))
    {
        return false;
    }
    std::ifstream pid_file{pid_path};
    uint64_t pid = 0;
    pid_file >> pid;
    return platform_process_alive(pid);
}

inline bool spawn_hub_daemon(const std::string &executable)
{
#if MUNX_PLATFORM_POSIX
    const char *arg0 = executable.c_str();
    const char *arg1 = "--pipe-hub";
    char *const argv[] = {
        const_cast<char *>(arg0),
        const_cast<char *>(arg1),
        nullptr,
    };
    pid_t pid = 0;
    const int status = ::posix_spawn(&pid, executable.c_str(), nullptr, nullptr, argv,
                                     munx::platform_environ());
    return status == 0;
#else
    std::vector<char> command_line{'"'};
    command_line.insert(command_line.end(), executable.begin(), executable.end());
    const char suffix[] = "\" --pipe-hub";
    command_line.insert(command_line.end(), suffix, suffix + sizeof suffix - 1);
    command_line.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessA(executable.c_str(), command_line.data(), nullptr, nullptr,
                          FALSE, DETACHED_PROCESS, nullptr, nullptr, &startup,
                          &process))
    {
        return false;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
#endif
}

inline std::optional<hub_connection> connect_hub()
{
#if MUNX_PLATFORM_POSIX
    if (!hub_endpoint_ready())
    {
        return std::nullopt;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return std::nullopt;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path = hub_socket_path().string();
    if (path.size() >= sizeof(address.sun_path))
    {
        ::close(fd);
        return std::nullopt;
    }
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0)
    {
        ::close(fd);
        if ((errno == ECONNREFUSED || errno == ENOENT || errno == EPROTOTYPE) &&
            !hub_process_alive())
        {
            std::error_code remove_error;
            std::filesystem::remove(hub_socket_path(), remove_error);
        }
        return std::nullopt;
    }
    hub_connection connection{fd};
    (void)connection.set_nonblocking();
    return connection;
#else
    const std::string pipe_name = hub_pipe_name();
    if (!::WaitNamedPipeA(pipe_name.c_str(), 2000))
    {
        return std::nullopt;
    }
    const HANDLE handle =
        ::CreateFileA(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                      OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return std::nullopt;
    }
    DWORD mode = PIPE_READMODE_BYTE;
    if (!::SetNamedPipeHandleState(handle, &mode, nullptr, nullptr))
    {
        ::CloseHandle(handle);
        return std::nullopt;
    }
    return hub_connection{handle};
#endif
}

inline void cleanup_hub_endpoint()
{
#if MUNX_PLATFORM_POSIX
    std::error_code error;
    std::filesystem::remove(hub_socket_path(), error);
#else
    (void)0;
#endif
}

} // namespace transport

inline bool hub_process_alive()
{
    return transport::hub_process_alive();
}

} // namespace munx::vm::pipe_hub
