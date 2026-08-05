#pragma once

#include <cstdint>
#include <ctime>
#include <cstring>
#include <climits>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#define MUNX_PLATFORM_WINDOWS 1
#define MUNX_PLATFORM_MACOS 0
#define MUNX_PLATFORM_POSIX 0
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#define MUNX_VM_HAS_SOCKETS 1
#elif defined(__APPLE__)
#define MUNX_PLATFORM_WINDOWS 0
#define MUNX_PLATFORM_MACOS 1
#define MUNX_PLATFORM_POSIX 1
#define MUNX_VM_HAS_SOCKETS 1
#include <arpa/inet.h>
#include <cerrno>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(__unix__)
#define MUNX_PLATFORM_WINDOWS 0
#define MUNX_PLATFORM_MACOS 0
#define MUNX_PLATFORM_POSIX 1
#define MUNX_VM_HAS_SOCKETS 1
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#else
#define MUNX_PLATFORM_WINDOWS 0
#define MUNX_PLATFORM_MACOS 0
#define MUNX_PLATFORM_POSIX 0
#if defined(__has_include)
#if __has_include(<sys/socket.h>)
#define MUNX_VM_HAS_SOCKETS 1
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#else
#define MUNX_VM_HAS_SOCKETS 0
#endif
#else
#define MUNX_VM_HAS_SOCKETS 0
#endif
#endif

#if MUNX_PLATFORM_POSIX && !MUNX_PLATFORM_MACOS
extern char **environ;
#endif

#if MUNX_PLATFORM_WINDOWS
using ssize_t = SSIZE_T;
#endif

namespace munx
{

#if MUNX_VM_HAS_SOCKETS

#if MUNX_PLATFORM_WINDOWS
using platform_socket = SOCKET;
inline platform_socket platform_invalid_socket() { return INVALID_SOCKET; }
inline bool platform_socket_valid(platform_socket socket)
{
    return socket != INVALID_SOCKET;
}
inline int platform_socket_to_int(platform_socket socket)
{
    return static_cast<int>(socket);
}
inline intptr_t platform_socket_to_descriptor(platform_socket socket)
{
    return static_cast<intptr_t>(socket);
}
inline platform_socket platform_int_to_socket(int descriptor)
{
    return static_cast<platform_socket>(descriptor);
}
inline platform_socket platform_descriptor_to_socket(intptr_t descriptor)
{
    return static_cast<platform_socket>(descriptor);
}
inline bool platform_descriptor_valid(intptr_t descriptor)
{
    return platform_socket_valid(platform_descriptor_to_socket(descriptor));
}
inline void platform_close_socket(int descriptor)
{
    ::closesocket(platform_int_to_socket(descriptor));
}
inline void platform_close_socket_descriptor(intptr_t descriptor)
{
    ::closesocket(platform_descriptor_to_socket(descriptor));
}
#else
using platform_socket = int;
inline platform_socket platform_invalid_socket() { return -1; }
inline bool platform_socket_valid(platform_socket socket) { return socket >= 0; }
inline int platform_socket_to_int(platform_socket socket) { return socket; }
inline intptr_t platform_socket_to_descriptor(platform_socket socket)
{
    return static_cast<intptr_t>(socket);
}
inline platform_socket platform_int_to_socket(int descriptor) { return descriptor; }
inline platform_socket platform_descriptor_to_socket(intptr_t descriptor)
{
    return static_cast<platform_socket>(descriptor);
}
inline bool platform_descriptor_valid(intptr_t descriptor)
{
    return platform_socket_valid(platform_descriptor_to_socket(descriptor));
}
inline void platform_close_socket(int descriptor) { ::close(descriptor); }
inline void platform_close_socket_descriptor(intptr_t descriptor)
{
    ::close(static_cast<int>(descriptor));
}
#endif

/// RAII Winsock startup/shutdown (no-op on POSIX).
class winsock_session
{
public:
    winsock_session()
    {
#if MUNX_PLATFORM_WINDOWS
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            ready_ = false;
        }
#endif
    }

    ~winsock_session()
    {
#if MUNX_PLATFORM_WINDOWS
        if (ready_)
        {
            ::WSACleanup();
        }
#endif
    }

    winsock_session(const winsock_session &) = delete;
    winsock_session &operator=(const winsock_session &) = delete;

    [[nodiscard]] bool ok() const { return ready_; }

private:
    bool ready_{true};
};

inline std::string socket_error_message()
{
#if MUNX_PLATFORM_WINDOWS
    const int error = ::WSAGetLastError();
    if (error != 0)
    {
        char *text = nullptr;
        const DWORD length = ::FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, static_cast<DWORD>(error), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPSTR>(&text), 0, nullptr);
        std::string message =
            length != 0 && text != nullptr ? std::string{text} : "unknown winsock error";
        if (text != nullptr)
        {
            ::LocalFree(text);
        }
        while (!message.empty() &&
               (message.back() == '\n' || message.back() == '\r'))
        {
            message.pop_back();
        }
        return message;
    }
#endif
    return std::strerror(errno);
}

#endif // MUNX_VM_HAS_SOCKETS

inline uint64_t platform_process_id()
{
#if MUNX_PLATFORM_WINDOWS
    return static_cast<uint64_t>(::GetCurrentProcessId());
#elif MUNX_VM_HAS_SOCKETS
    return static_cast<uint64_t>(::getpid());
#else
    return 0;
#endif
}

inline void platform_localtime(std::time_t seconds, std::tm *out)
{
#if MUNX_PLATFORM_WINDOWS
    localtime_s(out, &seconds);
#else
    localtime_r(&seconds, out);
#endif
}

/// Resolve the current process executable path when the OS provides one.
inline std::string platform_executable_path()
{
#if MUNX_PLATFORM_WINDOWS
    char buffer[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
    {
        return std::string{buffer, length};
    }
    return "munxc";
#elif defined(__linux__)
    std::error_code error;
    const std::filesystem::path proc_exe = "/proc/self/exe";
    if (std::filesystem::exists(proc_exe, error))
    {
        return std::filesystem::read_symlink(proc_exe, error).string();
    }
    return "munxc";
#elif MUNX_PLATFORM_MACOS
    char buffer[PATH_MAX];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0)
    {
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(buffer, error);
        if (!error)
        {
            return resolved.string();
        }
        return std::string{buffer};
    }
    return "munxc";
#else
    return "munxc";
#endif
}

#if MUNX_PLATFORM_POSIX
/// Environment pointer for `posix_spawn` (macOS hides `environ` in libc).
inline char **platform_environ()
{
#if MUNX_PLATFORM_MACOS
    extern char ***_NSGetEnviron(void);
    return *_NSGetEnviron();
#else
    return ::environ;
#endif
}
#endif

} // namespace munx
