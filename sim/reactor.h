#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace kalshi::sim {

using ReactorCallback = std::function<void(int fd, uint32_t events)>;

class Reactor {
private:
    int epfd_ = -1;
    bool running_ = false;
    std::unordered_map<int, ReactorCallback> callbacks_;
    std::vector<int> pending_removes_;


public:
    Reactor();
    ~Reactor();

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    bool add_fd(int fd, uint32_t events, ReactorCallback cb);
    bool mod_fd(int fd, uint32_t events);
    void remove_fd(int fd);

    void run();
    void stop();

};

}
