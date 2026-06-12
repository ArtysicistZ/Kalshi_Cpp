#pragma once

#include "sim/reactor.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <functional>
#include <optional>

namespace kalshi::sim {

struct Request {
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct Response {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
};

using Handler = std::function<Response(const Request&)>;

class RestServer {
private:
    Reactor& reactor_;
    uint16_t port_;
    int listen_fd_ = -1;

    struct Connection {
        std::string in_buffer;
    };

    std::unordered_map<std::string, Handler> routes_;
    std::unordered_map<int, Connection> connections_;


public:
    RestServer(Reactor& reactor, uint16_t port);
    ~RestServer();

    RestServer(const RestServer&) = delete;
    RestServer& operator=(const RestServer&) = delete;

    void route(
        const std::string& method,
        const std::string& path,
        Handler handler
    );

    bool start();

private:
    bool bind_and_listen_();
    void on_listener_event_(int fd, uint32_t events);
    void on_connection_event_(int fd, uint32_t events);
    bool write_response_(int fd, const kalshi::sim::Response& response);
    void close_connection_(int fd);

    Response dispatch_(const Request& req);

};

}
