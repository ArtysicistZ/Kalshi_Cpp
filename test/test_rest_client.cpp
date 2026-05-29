// Manual smoke test for RestClient (v1, Connection: close mode).
//
// Build (from repo root, on Linux/WSL):
//   g++ -std=c++20 -Wall -Wextra -I src \
//       test/test_rest_client.cpp src/net/rest_client.cpp -o test_rest_client
//
// GET test (Python's built-in server is fine):
//   python3 -m http.server 8900
//   ./test_rest_client 127.0.0.1 8900 /
//
// POST test (needs the echo server, which reflects the body back):
//   python3 script/echo_server.py            # listens on 127.0.0.1:8900
//   ./test_rest_client 127.0.0.1 8900 /echo '{"ticker":"X","count":10}'
//
// Args: <host> <port> <path> [post_body]
//   - no 4th arg  -> GET
//   - 4th arg     -> POST that body (application/json)

#include "net/rest_client.h"

#include <cstdio>

using kalshi::RestClient;
using kalshi::Response;

static void dump(const Response& r) {
    printf("===== STATUS =====\n%d\n", r.status);
    printf("===== HEADERS (%zu) =====\n", r.headers.size());
    for (const auto& [key, value] : r.headers) {
        printf("[%s] = [%s]\n", key.c_str(), value.c_str());
    }
    printf("===== BODY (%zu bytes) =====\n", r.body.size());
    fwrite(r.body.data(), 1, r.body.size(), stdout);
    printf("\n===== END =====\n");
}

int main(int argc, char** argv) {
    const char* host      = argc > 1 ? argv[1] : "127.0.0.1";
    const char* port      = argc > 2 ? argv[2] : "8900";
    const char* path      = argc > 3 ? argv[3] : "/";
    const char* post_body = argc > 4 ? argv[4] : nullptr;  // present -> POST

    RestClient client(host, port);
    if (!client.connect()) {
        fprintf(stderr, "connect() failed\n");
        return 1;
    }

    Response r = post_body ? client.post(path, post_body)
                           : client.get(path);
    dump(r);
    return 0;
}
