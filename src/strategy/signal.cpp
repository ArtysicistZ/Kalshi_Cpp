#include "strategy/signal.h"

namespace kalshi {

TargetQuote Signal::evaluate(const Book& book) const noexcept {
    
    uint8_t y = book.best_yes_bid();
    uint8_t n = book.best_no_bid();
    if (y == 0 || n == 0) return {};

    int spread = 100 - y - n;
    if (spread < MIN_SPREAD) return {};
    return TargetQuote{
        .active = true, 
        .yes_price = static_cast<uint8_t>(y + 1),
        .no_price = static_cast<uint8_t>(n + 1),
        .qty = DEFAULT_QTY
    };

}

}