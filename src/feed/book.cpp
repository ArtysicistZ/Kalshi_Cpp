#include "feed/book.h"

namespace kalshi {

void Book::apply(const OrderbookDelta& d) {

    auto& levels = (d.side == Side::YES ? yes_levels_ : no_levels_);
    uint8_t& best = (d.side == Side::YES ? best_yes_bid_ : best_no_bid_);

    levels[d.price] += d.delta;
    if (levels[d.price] > 0 && d.price > best) {
        best = d.price;
    } else if (d.price == best && levels[d.price] == 0) {
        while (best != 0 && levels[best] == 0) best--;
    }

}

}