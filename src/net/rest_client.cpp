#include "net/rest_client.h"

#include <cstring>
#include <cstdio>
#include <algorithm>
#include <netdb.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

namespace kalshi {

RestClient::RestClient(std::string host, std::string port) 
    : host_(std::move(host)), port_(std::move(port)) {}

RestClient::~RestClient() {
    if (fd_ != -1) close(fd_);
}

bool RestClient::connect() {

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    int rc = getaddrinfo(host_.c_str(), port_.c_str(), &hints, &result);
    if (rc != 0) {
        perror("getaddrinfo");
        return false;
    }

    fd_ = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd_ < 0) {
        perror("socket");
        freeaddrinfo(result);
        return false;
    }
    if (::connect(fd_, result->ai_addr, result->ai_addrlen) < 0) {
        perror("connect");
        freeaddrinfo(result);
        close(fd_);
        return false;
    }
    freeaddrinfo(result);
    return true;

}

Response RestClient::get(const std::string& path) {
    return send_request_("GET", path, "", "");
}

Response RestClient::post(
    const std::string& path,
    const std::string& body,
    const std::string& content_type
) {
    return send_request_("POST", path, body, content_type);
}

Response RestClient::send_request_(
    const std::string& method,
    const std::string& path,
    const std::string& body,
    const std::string& content_type
) {
    std::string req = method + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + host_ + "\r\n";
    req += "Connection: close\r\n";
    if (!body.empty()) {
        req += "Content-Type: " + content_type + "\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;
    
    size_t tot_size = req.length();
    size_t sent = 0;
    while (sent != tot_size) {
        ssize_t n = write(fd_, req.c_str() + sent, tot_size - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("write");
            return Response{};
        }
        sent += n;
    }

    return recv_response_();

}

Response RestClient::recv_response_() {

    std::string header;
    std::string body;
    char buf[4096];
    ssize_t n = 1;
    size_t tot_size = 0;
    while (true) {
        n = read(fd_, buf, sizeof(buf));
        if (n < 0) {
            if (n < 0 && errno == EAGAIN) continue;
            perror("read");
            return Response{};
        }
        if (n == 0) break;
        header.append(buf, n);
        size_t ptr = header.find("\r\n\r\n", std::max(size_t{3}, tot_size) - 3);
        if (ptr != std::string::npos) {
            body = header.substr(ptr + 4);
            header.resize(ptr + 4);
            break;
        }
        tot_size += n;
    }
    
    size_t ptr = 0;
    Response response;

    // status line
    while (header[ptr] != ' ') ptr++;
    ptr++;
    std::string status;
    while (header[ptr] != ' ') {
        status += header[ptr];
        ptr++;
    }
    response.status = std::stoi(status);
    ptr = header.find("\r\n", ptr);
    if (ptr == std::string::npos) {
        fprintf(stderr, "fatal: reading incomplete");
        return Response{};
    }
    ptr += 2;

    // headers
    while (true) {
        size_t nxt = header.find("\r\n", ptr);
        if (nxt == std::string::npos) {
            fprintf(stderr, "fatal: reading incomplete");
            return Response{};
        }
        if (nxt == ptr) break;
        std::string header_line = header.substr(ptr, nxt - ptr);
        ptr = nxt + 2;

        size_t space = header_line.find(": ", 0);
        if (space == std::string::npos) {
            fprintf(stderr, "fatal: format error");
            return Response{};
        }
        std::string header_fis = header_line.substr(0, space);
        std::string header_sec = header_line.substr(space + 2);
        response.headers.insert({header_fis, header_sec});
    }

    // body parse
    while (true) {
        n = read(fd_, buf, sizeof(buf));
        if (n < 0) {
            if (n < 0 && errno == EAGAIN) continue;
            perror("read");
            return Response{};
        }
        if (n == 0) break;
        body.append(buf, n);
    }

    response.body = std::move(body);
    return response;

}

}