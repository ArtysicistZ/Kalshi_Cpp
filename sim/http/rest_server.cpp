#include "sim/http/rest_server.h"

#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

namespace {

std::optional<kalshi::sim::Request> parse_request(const std::string& buffer) {

    kalshi::sim::Request request;

    size_t ptr = buffer.find("\r\n\r\n");
    if (ptr == std::string::npos) return std::nullopt;
    ptr += 4;

    size_t method = buffer.find(" ");
    request.method = buffer.substr(0, method);
    method++;

    size_t path = buffer.find(" ", method);
    request.path = buffer.substr(method, path - method);

    size_t header_line = buffer.find("\r\n");
    header_line += 2;
    while (true) {
        size_t nxt_line = buffer.find("\r\n", header_line);
        if (nxt_line == header_line) break;
        std::string line = buffer.substr(header_line, nxt_line - header_line);

        size_t header_name = buffer.find(": ", header_line);
        std::string name = buffer.substr(header_line, header_name - header_line);
        header_name += 2;
        std::string content = buffer.substr(header_name, nxt_line - header_name);

        request.headers[name] = content;
        header_line = nxt_line + 2;

        if (name == "Content-Length") {
            size_t body_length = std::stoi(content);
            if (body_length != buffer.length() - ptr) {
                return std::nullopt;
            }
        }
    }

    request.body = buffer.substr(ptr);
    return request;

}

const char* reason_phrase(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 500:
            return "Internal Server Error";
        default:
            return "Unknown Status";
    }
}

std::string serialize_response(const kalshi::sim::Response& resp) {

    std::string response = "HTTP/1.1 " + std::to_string(resp.status) + " " + reason_phrase(resp.status) + "\r\n";

    response += "Content-Type: " + resp.content_type + "\r\n";
    response += "Content-Length: " + std::to_string(resp.body.length()) + "\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";
    response += resp.body;

    return response;

}

}

namespace kalshi::sim {

RestServer::RestServer(Reactor& reactor, uint16_t port) : reactor_(reactor), port_(port) {}
RestServer::~RestServer() {
    if (listen_fd_ != -1) close(listen_fd_);
    for (auto& [fd, _] : connections_) close(fd);
}

void RestServer::route(
    const std::string& method,
    const std::string& path,
    Handler handler
) {
    routes_[method + " " + path] = std::move(handler);
}

bool RestServer::start() {

    if (!bind_and_listen_()) return false;
    return reactor_.add_fd(listen_fd_, EPOLLIN | EPOLLET,
        [this](int fd, uint32_t events) { this->on_listener_event_(fd, events); });

}

bool RestServer::bind_and_listen_() {

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        perror("socket");
        return false;
    }
    if (fcntl(listen_fd_, F_SETFL, O_NONBLOCK) < 0) {
        perror("fcntl");
        close(listen_fd_);
        return false;
    }

    int yes = 1;
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        perror("setsockopt");
        close(listen_fd_);
        return false;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd_);
        return false;
    }
    if (listen(listen_fd_, 128) < 0) {
        perror("listen");
        close(listen_fd_);
        return false;
    }

    return true;

}

void RestServer::on_listener_event_(int fd, uint32_t events) {

    while (true) {
        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("accept");
            continue;
        }
        if (fcntl(client_fd, F_SETFL, O_NONBLOCK) < 0) {
            perror("fcntl");
            close(client_fd);
            continue;
        }

        if (!reactor_.add_fd(client_fd, EPOLLIN | EPOLLET,
            [this](int f, uint32_t ev) { this->on_connection_event_(f, ev); })) {
            close(client_fd);
            continue;
        }
    }

}

void RestServer::on_connection_event_(int fd, uint32_t events) {

    char buf[4096];
    ssize_t n;
    while (true) {
        n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            connections_[fd].in_buffer.append(buf, n);
            auto optional_request = parse_request(connections_[fd].in_buffer);
            if (optional_request == std::nullopt) continue;

            Request request = std::move(*optional_request);
            Response response = dispatch_(request);
            write_response_(fd, response);
            connections_[fd].in_buffer.clear();
            close_connection_(fd);
            break;

        } else {
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                perror("read");
            }
            close_connection_(fd);
            break;
        }
    }

}

bool RestServer::write_response_(int fd, const kalshi::sim::Response& response) {

    std::string resp = serialize_response(response);
    ssize_t to_send = resp.length();
    ssize_t sent = 0;
    while (sent != to_send) {
        ssize_t n = write(fd, resp.c_str() + sent, to_send - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("write");
            return false;
        }
        sent += n;
    }
    return true;

}

void RestServer::close_connection_(int fd) {
    reactor_.remove_fd(fd);
    close(fd);
    connections_.erase(fd);
}

Response RestServer::dispatch_(const Request& req) {
    auto it = routes_.find(req.method + " " + req.path);
    if (it == routes_.end()) {
        return Response{404, "text/plain", "Not Found"};
    }
    return it->second(req);
}

}
