#pragma once

#include <cstdint>
#include <simdjson.h>

namespace kalshi {

enum class Side : uint8_t { YES = 0, NO = 1};

struct OrderbookDelta {
    char ticker[32];
    int64_t ts;         //exchange timestamp
    uint64_t sid;       //subscription id
    uint64_t seq;       //sequence number
    int32_t delta;      //quantity change
    uint8_t price;
    Side side;
};
static_assert(sizeof(OrderbookDelta) == 64);

class OrderbookParser {
private:
    simdjson::ondemand::parser parser_;

public:
    bool parse_orderbook_delta(simdjson::padded_string_view input, OrderbookDelta& out);

};

}