#pragma once

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
    uint16_t port_;
    int listen_fd_ = -1;
    int epfd_ = -1;

    struct Connection {
        std::string in_buffer;
    };

    std::unordered_map<std::string, Handler> routes_;
    std::unordered_map<int, Connection> connections_;


public:
    explicit RestServer(uint16_t port);
    ~RestServer();

    RestServer(const RestServer&) = delete;
    RestServer& operator=(const RestServer&) = delete;

    void route(
        const std::string& method,
        const std::string& path,
        Handler handler
    );

    bool run();

private:
    bool bind_and_listen_();
    void on_accept_();
    void on_readable_(int fd);
    bool write_response_(int fd, kalshi::sim::Response& response);
    void close_connection_(int fd);

    Response dispatch_(const Request& req);

};

}