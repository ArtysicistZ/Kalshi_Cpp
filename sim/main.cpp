#include "sim/http/rest_server.h"
#include "sim/http/handlers.h"
#include "sim/domain/market_registry.h"

#include <cstdio>

using namespace kalshi::sim;

int main() {
    RestServer server(8443);

    MarketRegistry registry;
    registry.add_market("DEMO-1");

    server.route("GET", "/exchange/status", [](const Request&) {
        return Response{200, "application/json", R"({"status": "ok"})"};
    });

    server.route("GET", "/portfolio/balance", [](const Request&) {
        return Response{200, "application/json", R"({"balance": 10000})"};
    });

    server.route("POST", "/portfolio/orders", [&registry](const Request& req) {
        return handle_place_order(req, registry);
    });

    if (!server.run()) {
        fprintf(stderr, "server.run() failed\n");
        return 1;
    }
    return 0;

}