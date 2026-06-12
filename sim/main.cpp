#include "sim/reactor.h"
#include "sim/http/rest_server.h"
#include "sim/http/handlers.h"
#include "sim/domain/market_registry.h"

#include <cstdio>

using namespace kalshi::sim;

int main() {
    Reactor reactor;

    MarketRegistry registry;
    registry.add_market("DEMO-1");

    RestServer server(reactor, 8443);

    server.route("GET", "/exchange/status", [](const Request&) {
        return Response{200, "application/json", R"({"status": "ok"})"};
    });

    server.route("GET", "/portfolio/balance", [](const Request&) {
        return Response{200, "application/json", R"({"balance": 10000})"};
    });

    server.route("POST", "/portfolio/orders", [&registry](const Request& req) {
        return handle_place_order(req, registry);
    });

    if (!server.start()) {
        fprintf(stderr, "server.start() failed\n");
        return 1;
    }

    reactor.run();
    return 0;

}
