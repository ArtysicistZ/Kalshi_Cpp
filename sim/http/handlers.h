#pragma once

#include "sim/http/rest_server.h"
#include "sim/domain/market_registry.h"

namespace kalshi::sim {

Response handle_place_order(const Request& req, MarketRegistry& reg);

Response handle_get_orderbook(const Request& req, MarketRegistry& reg);

Response handle_cancel_order(const Request& req, MarketRegistry& reg);
    
}