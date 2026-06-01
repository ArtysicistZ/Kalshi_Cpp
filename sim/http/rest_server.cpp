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

RestServer::RestServer(uint16_t port) : port_(port) {}
RestServer::~RestServer() {
    if (epfd_ != -1) close(epfd_);
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

bool RestServer::run() {

    if (!bind_and_listen_()) return false;

    while (true) {
        struct epoll_event events[64];
        int n = epoll_wait(epfd_, events, 64, -1);
        if (n < 0) {
            perror("epoll_wait");
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == listen_fd_) {
                on_accept_();
            } else {
                on_readable_(events[i].data.fd);
            }
        }
    }
    return true;

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

    epfd_ = epoll_create1(0);
    if (epfd_ < 0) {
        perror("epoll_create1");
        close(listen_fd_);
        return false;
    }

    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd_;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
        perror("epoll_ctl");
        close(listen_fd_);
        close(epfd_);
        return false;
    }

    return true;

}

void RestServer::on_accept_() {

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

        struct epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = client_fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            perror("epoll_ctl");
            close(client_fd);
            continue;
        }
    }

}

void RestServer::on_readable_(int fd) {

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
    if (epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        perror("epoll_ctl");
    }
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