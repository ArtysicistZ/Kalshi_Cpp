#include "sim/reactor.h"

#include <cstdio>
#include <cerrno>
#include "sim/compat/epoll_shim.h"
#include <unistd.h>

namespace kalshi::sim {

Reactor::Reactor() {
    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) {
        perror("epoll_create1");
    }
}

Reactor::~Reactor() {
    if (epfd_ != -1) close(epfd_);
}

bool Reactor::add_fd(int fd, uint32_t events, ReactorCallback cb) {

    struct epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl ADD");
        return false;
    }
    callbacks_[fd] = std::move(cb);
    return true;

}

bool Reactor::mod_fd(int fd, uint32_t events) {

    struct epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        perror("epoll_ctl MOD");
        return false;
    }
    return true;

}

void Reactor::remove_fd(int fd) {
    if (epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        perror("epoll_ctl DEL");
    }
    pending_removes_.push_back(fd);
}

void Reactor::run() {

    running_ = true;
    while (running_) {
        struct epoll_event events[64];
        int n = epoll_wait(epfd_, events, 64, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            return;
        }
        for (int i = 0; i < n; i++) {
            auto it = callbacks_.find(events[i].data.fd);
            if (it != callbacks_.end()) {
                it->second(events[i].data.fd, events[i].events);
            }
        }
        for (int fd : pending_removes_) {
            callbacks_.erase(fd);
        }
        pending_removes_.clear();
    }

}

void Reactor::stop() {
    running_ = false;
}

}
