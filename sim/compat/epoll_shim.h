#pragma once

// On Linux, use the real epoll(7). On macOS/BSD, provide a minimal shim over
// kqueue(2) covering exactly what kalshi-sim's reactor uses: edge-triggered
// read/write readiness with the fd carried in epoll_event.data.fd.

#if defined(__linux__)

#include <sys/epoll.h>

#else

#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdint>
#include <cerrno>

inline constexpr uint32_t EPOLLIN    = 0x001;
inline constexpr uint32_t EPOLLOUT   = 0x004;
inline constexpr uint32_t EPOLLERR   = 0x008;
inline constexpr uint32_t EPOLLHUP   = 0x010;
inline constexpr uint32_t EPOLLRDHUP = 0x2000;
inline constexpr uint32_t EPOLLET    = 0x80000000u;  // edge-triggered -> EV_CLEAR

inline constexpr int EPOLL_CLOEXEC = 0x80000;
inline constexpr int EPOLL_CTL_ADD = 1;
inline constexpr int EPOLL_CTL_DEL = 2;
inline constexpr int EPOLL_CTL_MOD = 3;

union epoll_data {
    void*    ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
};
typedef union epoll_data epoll_data_t;

struct epoll_event {
    uint32_t     events;
    epoll_data_t data;
};

inline int epoll_create1(int flags) {
    int kq = kqueue();
    if (kq < 0) return -1;
    if (flags & EPOLL_CLOEXEC) fcntl(kq, F_SETFD, FD_CLOEXEC);
    return kq;
}

inline int epoll_ctl(int epfd, int op, int fd, struct epoll_event* ev) {
    if (op == EPOLL_CTL_DEL) {
        struct kevent del[2];
        EV_SET(&del[0], fd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
        EV_SET(&del[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        kevent(epfd, del, 2, nullptr, 0, nullptr);  // ignore ENOENT for unset filters
        return 0;
    }
    if (!ev) { errno = EINVAL; return -1; }

    if (op == EPOLL_CTL_MOD) {  // clear prior filters, then re-add the requested set
        struct kevent del[2];
        EV_SET(&del[0], fd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
        EV_SET(&del[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        kevent(epfd, del, 2, nullptr, 0, nullptr);
    }

    unsigned short flags = EV_ADD | EV_ENABLE;
    if (ev->events & EPOLLET) flags |= EV_CLEAR;

    struct kevent ch[2];
    int n = 0;
    void* tag = reinterpret_cast<void*>(static_cast<intptr_t>(fd));  // recover fd in epoll_wait
    if (ev->events & EPOLLIN)  EV_SET(&ch[n++], fd, EVFILT_READ,  flags, 0, 0, tag);
    if (ev->events & EPOLLOUT) EV_SET(&ch[n++], fd, EVFILT_WRITE, flags, 0, 0, tag);
    if (n == 0) return 0;
    return kevent(epfd, ch, n, nullptr, 0, nullptr);
}

inline int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout) {
    struct timespec ts;
    struct timespec* pts = nullptr;
    if (timeout >= 0) {
        ts.tv_sec  = timeout / 1000;
        ts.tv_nsec = static_cast<long>(timeout % 1000) * 1000000L;
        pts = &ts;
    }

    constexpr int KQ_MAX = 1024;
    if (maxevents > KQ_MAX) maxevents = KQ_MAX;
    struct kevent kev[KQ_MAX];

    int n = kevent(epfd, nullptr, 0, kev, maxevents, pts);
    if (n < 0) return -1;

    for (int i = 0; i < n; i++) {
        uint32_t e = 0;
        if (kev[i].filter == EVFILT_READ)  e |= EPOLLIN;
        if (kev[i].filter == EVFILT_WRITE) e |= EPOLLOUT;
        if (kev[i].flags & EV_EOF)   e |= EPOLLHUP | EPOLLIN;  // surface so read loop sees the close
        if (kev[i].flags & EV_ERROR) e |= EPOLLERR;
        events[i].events  = e;
        events[i].data.fd = static_cast<int>(reinterpret_cast<intptr_t>(kev[i].udata));
    }
    return n;
}

#endif  // __linux__
