#pragma once

#include <string>
#include <string_view>
#include <openssl/evp.h>

namespace kalshi {

class Auth {
public:
    struct SignedHeaders {
        std::string key_id;
        std::string timestamp;
        std::string signature;
    };

    Auth(std::string key_id, const std::string& pem_path) {

    }
    ~Auth() = default;

    Auth(const Auth&) = delete;
    Auth& operator=(const Auth&) = delete;
    Auth(Auth&&) = delete;
    Auth& operator=(const Auth&&) = delete;

    SignedHeaders sign(std::string_view method, std::string_view path) const {

    }

private:
    std::string key_id_;
    EVP_PKEY* pkey_;

};

}