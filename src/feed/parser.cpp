#include "parser.h"

#include <string_view>
#include <cstring>

namespace kalshi {

bool OrderbookParser::parse_orderbook_delta(simdjson::padded_string_view input, OrderbookDelta& out) {

    simdjson::ondemand::document doc;
    if (parser_.iterate(input).get(doc)) {
        return false;
    }    
    if (doc.find_field_unordered("sid").get(out.sid)) {
        return false;
    }
    if (doc.find_field_unordered("seq").get(out.seq)) {
        return false;
    }

    simdjson::ondemand::object msg;
    if (doc.find_field_unordered("msg").get(msg)) {
        return false;
    }

    std::string_view ticker_sv;
    if (msg.find_field_unordered("market_ticker").get(ticker_sv)) {
        return false;
    }
    if (ticker_sv.size() > 31) return false;
    std::memcpy(out.ticker, ticker_sv.data(), ticker_sv.size());
    out.ticker[ticker_sv.size()] = '\0';

    uint64_t price_raw;
    if (msg.find_field_unordered("price").get(price_raw)) {
        return false;
    }
    if (price_raw < 1 || price_raw > 99) return false;
    out.price = static_cast<uint8_t>(price_raw);

    int64_t delta_raw;
    if (msg.find_field_unordered("delta").get(delta_raw)) {
        return false;
    }
    if (delta_raw < INT32_MIN || delta_raw > INT32_MAX) {
        return false;
    }
    out.delta = static_cast<int32_t>(delta_raw);

    if (msg.find_field_unordered("ts").get(out.ts)) {
        return false;
    }


    std::string_view side_sv;
    if (msg.find_field_unordered("side").get(side_sv)) {
        return false;
    }
    if (side_sv == "yes") out.side = Side::YES;
    else if (side_sv == "no") out.side = Side::NO;
    else return false;

    return true;

}

}