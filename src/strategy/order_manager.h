#pragma once

#include <cstdint>
#include "strategy/signal.h"

namespace kalshi {

enum class ActionKind : uint8_t { PLACE = 0, CANCEL = 1 };

struct Action {
    ActionKind kind;
    Side side;
    uint8_t price;
    uint32_t qty;
    uint64_t client_id;
};

struct ActionBundle {
    Action actions[4];
    uint8_t cnt;
};

struct OrderState {
    bool open;
    uint8_t price;
    uint32_t qty;
    uint64_t client_id;
};

class OrderManager {
private:
    OrderState yes_open_{};
    OrderState no_open_{};
    uint64_t next_client_id_{1};

public:
    ActionBundle reconcile(const TargetQuote& target) noexcept;

    void on_place_sent(const Action& a) noexcept;
    void on_cancel_sent(const Action& a) noexcept;

private:
    void reconcile_helper(
        OrderState& cur, Side s, 
        bool want, uint8_t want_price, uint32_t want_qty,
        ActionBundle& out
    ) noexcept;

};

}