#include "sim/rest_server.h"

#include <cstdio>

using namespace kalshi::sim;

int main() {
    RestServer server(8443);

    server.route("GET", "/exchange/status", [](const Request&) {
        return Response{200, "application/json", R"({"status": "ok"})"};
    });

    server.route("GET", "/portfolio/balance", [](const Request&) {
        return Response{200, "application/json", R"({"balance": 10000})"};
    });

    server.route("POST", "/portfolio/orders", [](const Request& req) {
        return Response{200, "application/json", req.body};
    });

    if (!server.run()) {
        fprintf(stderr, "server.run() failed\n");
        return 1;
    }
    return 0;

}