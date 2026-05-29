#pragma once

#include <string>
#include <unordered_map>

namespace kalshi {

struct Response {
    int status = 0;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

class RestClient {
private:
    std::string host_;
    std::string port_;
    int fd_ = -1;

public:
    RestClient(std::string host, std::string port);
    ~RestClient();

    RestClient(const RestClient&) = delete;
    RestClient& operator=(const RestClient&) = delete;

    bool connect();

    Response get(const std::string& path);
    Response post(
        const std::string& path,
        const std::string& body,
        const std::string& content_type = "application/json"
    );

private:
    Response send_request_(
        const std::string& method,
        const std::string& path,
        const std::string& body,
        const std::string& content_type
    );
    Response recv_response_();

};

}