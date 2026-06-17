#include "strategy/order_manager.h"

namespace kalshi {

void OrderManager::reconcile_helper(
    OrderState& cur, Side s, 
    bool want, uint8_t want_price, uint32_t want_qty,
    ActionBundle& out
) noexcept {

    if (!cur.open && want) {
        out.actions[out.cnt++] = Action{
            ActionKind::PLACE, s, want_price, want_qty, next_client_id_++
        };
    } 
    else if (cur.open && !want) {
        out.actions[out.cnt++] = Action{
            ActionKind::CANCEL, s, 0, 0, cur.client_id
        };
    } 
    else if (cur.open && want) {
        if (want_price != cur.price) {
            out.actions[out.cnt++] = Action{
                ActionKind::CANCEL, s, 0, 0, cur.client_id
            };            
            out.actions[out.cnt++] = Action{
                ActionKind::PLACE, s, want_price, want_qty, next_client_id_++
            };
        }
    }

}

ActionBundle OrderManager::reconcile(
    const TargetQuote& target
) noexcept {

    ActionBundle bundle{};
    reconcile_helper(
        yes_open_, Side::YES, 
        target.active, target.yes_price, target.qty,
        bundle
    );
    reconcile_helper(
        no_open_, Side::NO, 
        target.active, target.no_price, target.qty,
        bundle
    );
    return bundle;

}

void OrderManager::on_place_sent(const Action& a) noexcept {
    OrderState& cur = (a.side == Side::YES ? yes_open_ : no_open_);
    cur.open = true;
    cur.price = a.price;
    cur.qty = a.qty;
    cur.client_id = a.client_id;
}

void OrderManager::on_cancel_sent(const Action& a) noexcept {
    OrderState& cur = (a.side == Side::YES ? yes_open_ : no_open_);
    cur = OrderState{};
}

}