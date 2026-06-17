#pragma once

#include <cstdint>
#include "feed/book.h"

namespace kalshi {

struct TargetQuote {
    bool active;
    uint8_t yes_price;
    uint8_t no_price;
    uint32_t qty;
};

class Signal {
public:
    TargetQuote evaluate(const Book& book) const noexcept;
    static constexpr uint8_t MIN_SPREAD = 3;
    static constexpr uint32_t DEFAULT_QTY = 10;
};

}