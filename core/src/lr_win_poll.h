// Winsock does not reliably wake WSAPoll/select on local shutdown. Track local
// cancellation and bound each poll slice, without shortening post-header stream
// timeouts. This shim is private to the common cpp-httplib include.
#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <mutex>
#include <unordered_set>

inline std::mutex lrCancelledSocketsMutex;
inline std::unordered_set<SOCKET> lrCancelledSockets;

inline int lrCancelSocket(SOCKET socket, int how) {
    {
        std::scoped_lock lock{lrCancelledSocketsMutex};
        lrCancelledSockets.insert(socket);
    }
    return ::shutdown(socket, how);
}

inline int lrCloseSocket(SOCKET socket) {
    // Erase before closing: only after closesocket can another socket reuse the
    // handle, and clearing then could erase that new socket's cancellation.
    {
        std::scoped_lock lock{lrCancelledSocketsMutex};
        lrCancelledSockets.erase(socket);
    }
    return ::closesocket(socket);
}

inline int lrInterruptiblePoll(WSAPOLLFD *fds, ULONG count, int timeout) {
    const auto started = ::GetTickCount64();
    for (;;) {
        {
            std::scoped_lock lock{lrCancelledSocketsMutex};
            for (ULONG i = 0; i < count; ++i) {
                if (lrCancelledSockets.contains(fds[i].fd)) {
                    ::WSASetLastError(WSAESHUTDOWN);
                    return SOCKET_ERROR;
                }
            }
        }
        const auto elapsed = ::GetTickCount64() - started;
        int slice = 100;
        if (timeout >= 0) {
            if (elapsed >= static_cast<ULONGLONG>(timeout)) slice = 0;
            else if (timeout - static_cast<int>(elapsed) < slice)
                slice = timeout - static_cast<int>(elapsed);
        }
        const int result = ::WSAPoll(fds, count, slice);
        if (result != 0) return result;
        if (timeout >= 0 && ::GetTickCount64() - started >= static_cast<ULONGLONG>(timeout)) return 0;
    }
}

#define WSAPoll lrInterruptiblePoll
#define shutdown lrCancelSocket
#define closesocket lrCloseSocket
#endif
