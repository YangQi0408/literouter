// cpp-httplib waits with WSAPoll on Windows. A local shutdown does not reliably
// wake WSAPoll, so joining an upstream reader can retain its full read timeout
// after the gateway's header deadline. Winsock select observes local shutdown.
// Include immediately before httplib.h and undefine WSAPoll immediately after.
#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

inline int lrInterruptiblePoll(WSAPOLLFD *fds, ULONG count, int timeout) {
    if (count > FD_SETSIZE) return ::WSAPoll(fds, count, timeout);
    fd_set reads, writes, errors;
    FD_ZERO(&reads);
    FD_ZERO(&writes);
    FD_ZERO(&errors);
    for (ULONG i = 0; i < count; ++i) {
        fds[i].revents = 0;
        if (fds[i].events & POLLIN) FD_SET(fds[i].fd, &reads);
        if (fds[i].events & POLLOUT) FD_SET(fds[i].fd, &writes);
        FD_SET(fds[i].fd, &errors);
    }
    timeval wait{timeout / 1000, (timeout % 1000) * 1000};
    const int result = ::select(0, &reads, &writes, &errors, timeout < 0 ? nullptr : &wait);
    if (result <= 0) return result;
    int ready = 0;
    for (ULONG i = 0; i < count; ++i) {
        if (FD_ISSET(fds[i].fd, &reads)) fds[i].revents |= POLLIN;
        if (FD_ISSET(fds[i].fd, &writes)) fds[i].revents |= POLLOUT;
        if (FD_ISSET(fds[i].fd, &errors)) fds[i].revents |= POLLERR;
        if (fds[i].revents) ++ready;
    }
    return ready;
}

#define WSAPoll lrInterruptiblePoll
#endif
