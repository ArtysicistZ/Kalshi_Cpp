#pragma once

#include <cstdint>
#include <vector>

#include "core/pool_alloc.h"
#include "core/flat_hash_map.h"

namespace kalshi::sim {

enum class Side : uint8_t { YES, NO };

struct Fill {
    uint64_t maker_order_id;
    uint64_t taker_order_id;
    uint8_t price;
    uint32_t qty;
    uint64_t ts;
};

struct Order {
    uint64_t order_id;
    uint64_t client_id;
    Side side;
    uint8_t price;
    uint32_t qty_remaining;
    uint64_t ts_received;

    Order* prev_in_level = nullptr;
    Order* next_in_level = nullptr;
};

struct PriceLevel {
    Order* head = nullptr;
    Order* tail = nullptr;
    uint32_t total_qty = 0;
};

struct PlaceResult {
    uint64_t order_id;
    std::vector<Fill> fills;
    uint32_t qty_resting;
    bool accepted = true;
};

class MatchingEngine {
private:
    kalshi::Pool<Order, 2048> pool_;

    PriceLevel bids_[101];
    PriceLevel asks_[101];

    kalshi::FlatHashMap<uint64_t, Order*, 4096> orders_;

    uint8_t best_bid_ = 0;
    uint8_t best_ask_ = 100;
    uint64_t next_order_id_ = 1;

public:
    MatchingEngine() = default;
    ~MatchingEngine() = default;

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    PlaceResult place_limit(
        uint64_t client_id,
        Side side,
        uint8_t price,
        uint32_t qty
    );

    bool cancel(uint64_t order_id, uint64_t client_id);

    uint8_t best_bid() const;
    uint8_t best_ask() const;
    uint32_t depth_at(Side side, uint8_t price) const;

};

}