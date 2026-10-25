// A socket vocabulary the test stubs can be written in once.
//
// The relays and forward proxy in test_proxy.cpp are raw sockets on purpose:
// they have to do things httplib::Server cannot — accept and then say nothing,
// hang up with a RST instead of an empty 200, splice a tunnel — and those are
// exactly the failures the proxy's header gate and failover logic exist for.
//
// Written against POSIX they were portable to Linux and macOS and nothing else,
// so three groups were skipped on Windows: the hanging relay, the request
// deadline, and connection reuse. Those are the failover and pooling paths, so
// the platform with the most to go wrong was the one with no coverage of them.
//
// This header is the smallest thing that fixes that: the same call names, the
// same shapes, and on Windows the Winsock spelling behind them. Everything here
// is test-side only; the product itself never includes it.
//
// Two POSIX behaviours have no Winsock equivalent and are spelled out rather
// than papered over:
//
//   * `MSG_NOSIGNAL` does not exist. Its job on POSIX is to stop a write to a
//     closed peer from raising SIGPIPE and killing the process. Windows never
//     raises SIGPIPE, so the flag is simply absent there — `lrSend` drops it.
//   * `close()` on a socket is `closesocket()`, and the two are not
//     interchangeable: `close()` on a SOCKET leaves the handle open.
//
// What is deliberately NOT abstracted: the error model. Every call here returns
// the same thing it returns on POSIX (0 or a byte count, -1 on failure), and a
// failing `lrClose` on an already-closed descriptor is still the caller's
// problem. The stubs already ignore those results, which is correct on both.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#if defined(_WIN32)

// Guarded the way the rest of this repository guards windows.h: libstdc++'s
// os_defines.h already defines NOMINMAX on mingw, and a bare #define warns.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>

// Winsock has to be started before any call, and it is reference counted, so a
// single process-wide initialiser is the whole story. A static in a header is
// safe here because the tests are single-binary and single-threaded at the
// point of first use.
namespace lr_test {
namespace detail {
inline int winsockRefs() {
    static std::atomic<int> refs{0};
    return refs.load();
}
inline void ensureWinsock() {
    static const bool started = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
}
} // namespace detail
} // namespace lr_test

using lr_socket_t = SOCKET;
using lr_socklen_t = int;
using lr_linger = ::linger;
constexpr lr_socket_t lr_invalid_socket = INVALID_SOCKET;

// Winsock spells the non-blocking switch as an ioctlsocket call, not fcntl.
inline int lrSetNonBlocking(lr_socket_t socket) {
    u_long mode = 1;
    return ::ioctlsocket(socket, FIONBIO, &mode);
}

inline int lrClose(lr_socket_t socket) {
    return ::closesocket(socket);
}

// POSIX's shutdown() and Winsock's take the same argument, but the constant
// names differ for the write-half case; SHUT_RDWR exists on both.
inline int lrShutdownBoth(lr_socket_t socket) {
    return ::shutdown(socket, SD_BOTH);
}

// `poll` on a socket is `WSAPoll` on Winsock, and the structures are the same
// layout. WSAPoll takes an int count.
struct lr_pollfd {
    lr_socket_t fd;
    short events;
    short revents;
};

inline int lrPoll(lr_pollfd *descriptors, int count, int timeout_ms) {
    static_assert(sizeof(lr_pollfd) == sizeof(WSAPOLLFD),
                  "lr_pollfd must alias WSAPOLLFD");
    return ::WSAPoll(reinterpret_cast<WSAPOLLFD *>(descriptors), static_cast<ULONG>(count),
                     timeout_ms);
}

constexpr short lr_pollin = POLLRDNORM;

inline int lrSetReuseAddr(lr_socket_t socket) {
    const int one = 1;
    return ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                        reinterpret_cast<const char *>(&one), sizeof(one));
}

inline int lrSetRecvTimeout(lr_socket_t socket, int seconds) {
    // Winsock wants milliseconds in a DWORD, not a timeval.
    const DWORD millis = static_cast<DWORD>(seconds) * 1000;
    return ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char *>(&millis), sizeof(millis));
}

// SO_LINGER with a zero timeout is what turns close() into a RST. The structure
// is the same on both platforms; only the cast differs.
inline int lrSetLingerReset(lr_socket_t socket) {
    ::linger reset{};
    reset.l_onoff = 1;
    reset.l_linger = 0;
    return ::setsockopt(socket, SOL_SOCKET, SO_LINGER,
                        reinterpret_cast<const char *>(&reset), sizeof(reset));
}

inline long long lrSend(lr_socket_t socket, const char *data, std::size_t length) {
    // No MSG_NOSIGNAL: Windows has no SIGPIPE to suppress.
    return ::send(socket, data, static_cast<int>(length), 0);
}

inline long long lrRecv(lr_socket_t socket, char *data, std::size_t length) {
    return ::recv(socket, data, static_cast<int>(length), 0);
}

inline lr_socket_t lrAccept(lr_socket_t listener) {
    return ::accept(listener, nullptr, nullptr);
}

#else

#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>

using lr_socket_t = int;
using lr_socklen_t = socklen_t;
using lr_linger = ::linger;
constexpr lr_socket_t lr_invalid_socket = -1;

inline int lrSetNonBlocking(lr_socket_t socket) {
    return ::fcntl(socket, F_SETFL, ::fcntl(socket, F_GETFL, 0) | O_NONBLOCK);
}

inline int lrClose(lr_socket_t socket) { return ::close(socket); }

inline int lrShutdownBoth(lr_socket_t socket) { return ::shutdown(socket, SHUT_RDWR); }

struct lr_pollfd {
    lr_socket_t fd;
    short events;
    short revents;
};

inline int lrPoll(lr_pollfd *descriptors, int count, int timeout_ms) {
    static_assert(sizeof(lr_pollfd) == sizeof(pollfd), "lr_pollfd must alias pollfd");
    return ::poll(reinterpret_cast<pollfd *>(descriptors), static_cast<nfds_t>(count), timeout_ms);
}

constexpr short lr_pollin = POLLIN;

inline int lrSetReuseAddr(lr_socket_t socket) {
    const int one = 1;
    return ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
}

inline int lrSetRecvTimeout(lr_socket_t socket, int seconds) {
    timeval timeout{};
    timeout.tv_sec = seconds;
    return ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

inline int lrSetLingerReset(lr_socket_t socket) {
    ::linger reset{1, 0};
    return ::setsockopt(socket, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
}

inline long long lrSend(lr_socket_t socket, const char *data, std::size_t length) {
    return ::send(socket, data, length, MSG_NOSIGNAL);
}

inline long long lrRecv(lr_socket_t socket, char *data, std::size_t length) {
    return ::recv(socket, data, length, 0);
}

inline lr_socket_t lrAccept(lr_socket_t listener) { return ::accept(listener, nullptr, nullptr); }

#endif

namespace lr_test {

// Binds a loopback listener on a kernel-chosen port and returns it, or
// lr_invalid_socket. Shared by every stub, so the bind/getname dance exists
// once rather than three times with three chances to drift.
inline lr_socket_t listenLoopback(int *port_out) {
#if defined(_WIN32)
    detail::ensureWinsock();
#endif
    lr_socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == lr_invalid_socket) {
        return lr_invalid_socket;
    }
    lrSetReuseAddr(listener);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    // Unqualified on purpose: Darwin declares htonl/ntohs as function-like
    // macros whose bodies are parenthesised expressions, and `::htonl(..)` does
    // not survive that expansion.
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 64) != 0) {
        lrClose(listener);
        return lr_invalid_socket;
    }
    lr_socklen_t length = sizeof(address);
    if (::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        lrClose(listener);
        return lr_invalid_socket;
    }
    *port_out = ntohs(address.sin_port);
    return listener;
}

} // namespace lr_test
