#pragma once

#include <cstddef>
#include "strategy/order_manager.h"

namespace kalshi {

size_t serialize_action(
    const Action& a,
    const char* ticker,
    char* out,
    size_t cap
) noexcept;

}