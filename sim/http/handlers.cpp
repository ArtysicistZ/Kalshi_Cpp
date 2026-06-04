#include "sim/http/handlers.h"
#include "sim/domain/matching_engine.h"

#include <simdjson.h>
#include <string>
#include <string_view>
#include <sstream>
#include <charconv>

namespace kalshi::sim {

namespace sj = simdjson;

namespace {
    Response make_error(int status, std::string_view msg) {
        Response r;
        r.status = status;
        r.content_type = "application/json";
        r.body = std::string("{\"error\":\"") + std::string(msg) + "\"}";
        return r;
    }
}

Response handle_place_order(const Request& req, MarketRegistry& reg) {

    uint64_t client_id = 1;

    sj::padded_string padded(req.body);
    sj::ondemand::parser parser;
    sj::ondemand::document doc;
    if (parser.iterate(padded).get(doc)) {
        return make_error(400, "malformed JSON");
    }

    std::string_view ticker;
    if (doc.find_field_unordered("ticker").get_string().get(ticker)) {
        return make_error(400, "missing or invalid 'ticker'");
    }
    if (ticker.empty()) {
        return make_error(400, "ticker empty");
    }

    std::string_view side_str;
    if (doc.find_field_unordered("side").get_string().get(side_str)) {
        return make_error(400, "missing or invalid 'side'");
    }
    Side side;
    if (side_str == "YES") side = Side::YES;
    else if (side_str == "NO") side = Side::NO;
    else return make_error(400, "side must be 'YES' or 'NO'");

    int64_t price_raw;
    if (doc.find_field_unordered("price").get_int64().get(price_raw)) {
        return make_error(400, "missing or invalid 'price'");
    }
    if (price_raw < 1 || price_raw > 99) {
        return make_error(400, "price out of range (1-99)");
    }
    uint8_t price = static_cast<uint8_t>(price_raw);
    
    int64_t qty_raw;
    if (doc.find_field_unordered("qty").get_int64().get(qty_raw)) {
        return make_error(400, "missing or invalid 'qty'");
    }
    if (qty_raw <= 0 || qty_raw > 1000000) {
        return make_error(400, "qty out of range");
    }
    uint32_t qty = static_cast<uint32_t>(qty_raw);

    MatchingEngine* engine = reg.get_engine(std::string(ticker));
    if (engine == nullptr) {
        return make_error(404, "no such market");
    }

    PlaceResult result = engine->place_limit(client_id, side, price, qty);

    std::ostringstream out;
    out << "{\"order_id\":" << result.order_id
        << ",\"qty_resting\":" << result.qty_resting
        << ",\"accepted\":" << (result.accepted ? "true" : "false")
        << ",\"fills\": [";

    for (size_t i = 0; i < result.fills.size(); i++) {
        const Fill& f = result.fills[i];
        if (i > 0) out << ',';
        out << "{\"maker_order_id\":" << f.maker_order_id
            << ",\"taker_order_id\":" << f.taker_order_id
            << ",\"price\":" << static_cast<int>(f.price)
            << ",\"qty\":" << f.qty
            << ",\"ts\":" << f.ts << '}';
    }

    out << "]}";
    Response r;
    r.status = 200;
    r.content_type = "application/json";
    r.body = out.str();
    return r;

}

Response handle_get_orderbook(const Request& req, MarketRegistry& reg) {
    return make_error(501, "endpoint developing");
}

Response handle_cancel_order(const Request& req, MarketRegistry& reg) {
    return make_error(501, "endpoint developing");
}
    
}