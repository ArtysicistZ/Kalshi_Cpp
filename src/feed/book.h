#pragma once

#include <cstdint>
#include "feed/parser.h"

namespace kalshi {

class Book {
private:
    int32_t yes_levels_[100]{};
    int32_t no_levels_[100]{};
    uint8_t best_yes_bid_{0};
    uint8_t best_no_bid_{0};

public:
    void apply(const OrderbookDelta& d);

    uint8_t best_yes_bid() const noexcept {
        return best_yes_bid_;
    }

    uint8_t best_no_bid() const noexcept {
        return best_no_bid_;
    }

    int32_t qty_at(Side s, uint8_t price) const noexcept {
        if (s == Side::YES) return yes_levels_[price];
        else return no_levels_[price];
    }

};

}