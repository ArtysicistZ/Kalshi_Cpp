#include "sim/matching_engine.h"

#include <chrono>
#include <algorithm>

namespace {

uint64_t ts_now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

}

namespace kalshi::sim {

PlaceResult MatchingEngine::place_limit(
    uint64_t client_id,
    Side side,
    uint8_t price,
    uint32_t qty
) {
    if (price < 1 || price > 99) return {0, {}, 0, false};
    if (qty == 0) return {0, {}, 0, false};

    PriceLevel* matching_wall;
    PriceLevel* resting_wall;
    uint8_t scan_start;
    int8_t scan_dir;

    if (side == Side::YES) {
        matching_wall = asks_;
        resting_wall = bids_;
        scan_start = best_ask_;
        scan_dir = 1;
    } else {
        matching_wall = bids_;
        resting_wall = asks_;
        scan_start = best_bid_;
        scan_dir = -1;
    }

    Order* incoming = pool_.allocate();
    if (incoming == nullptr) return {0, {}, 0, false};
    incoming->order_id = next_order_id_++;
    incoming->client_id = client_id;
    incoming->side = side;
    incoming->price = price;
    incoming->qty_remaining = qty;
    incoming->ts_received = ts_now();
    incoming->next_in_level = nullptr;
    incoming->prev_in_level = nullptr;

    PlaceResult result;
    result.accepted = true;
    result.order_id = incoming->order_id;
    
    uint8_t ptr = scan_start;
    while ((scan_dir == 1 ? ptr <= price : ptr >= price) && incoming->qty_remaining != 0) {
        
        Order* cur = matching_wall[ptr].head;
        while (cur != nullptr && incoming->qty_remaining > 0) {
            uint32_t traded = std::min(cur->qty_remaining, incoming->qty_remaining);

            result.fills.push_back({
                cur->order_id, 
                incoming->order_id, 
                ptr, traded, incoming->ts_received
            });
            cur->qty_remaining -= traded;
            matching_wall[ptr].total_qty -= traded;
            incoming->qty_remaining -= traded;
            if (cur->qty_remaining == 0) {
                Order* next = cur->next_in_level;
                if (cur->prev_in_level) {
                    cur->prev_in_level->next_in_level = next;
                } else {
                    matching_wall[ptr].head = next;
                }
                
                if (next) {
                    next->prev_in_level = cur->prev_in_level;
                } else {
                    matching_wall[ptr].tail = cur->prev_in_level;
                }
                orders_.erase(cur->order_id);
                pool_.deallocate(cur);
                cur = next;
            } else break;
        }
        
        ptr += scan_dir;
    }

    if (incoming->qty_remaining > 0) {

        result.qty_resting = incoming->qty_remaining;

        orders_.insert(incoming->order_id, incoming);
        Order* old_tail = resting_wall[incoming->price].tail;
        resting_wall[incoming->price].tail = incoming;
        if (!old_tail) {
            resting_wall[incoming->price].head = incoming;
        } else {
            incoming->prev_in_level = old_tail;
            old_tail->next_in_level = incoming;
        }
        resting_wall[incoming->price].total_qty += incoming->qty_remaining;

        if (side == Side::YES) {
            best_bid_ = std::max(best_bid_, incoming->price);
        } else {
            best_ask_ = std::min(best_ask_, incoming->price);
        }

    } else {
        result.qty_resting = 0;
        pool_.deallocate(incoming);
    }

    while (bids_[best_bid_].total_qty == 0 && best_bid_ != 0) best_bid_--;
    while (asks_[best_ask_].total_qty == 0 && best_ask_ != 100) best_ask_++;

    return result;

}

bool MatchingEngine::cancel(uint64_t order_id, uint64_t client_id) {

    Order** slot = orders_.find(order_id);
    if (slot == nullptr) return false;
    Order* cur = *slot;
    if (cur->client_id != client_id) return false;

    uint8_t price = cur->price;
    PriceLevel* cur_level;
    if (cur->side == Side::YES) cur_level = bids_;
    else cur_level = asks_;

    if (!cur->next_in_level) {
        cur_level[price].tail = cur->prev_in_level;
    } else {
        cur->next_in_level->prev_in_level = cur->prev_in_level;
    }
    if (!cur->prev_in_level) {
        cur_level[price].head = cur->next_in_level;
    } else {
        cur->prev_in_level->next_in_level = cur->next_in_level;
    }

    cur_level[price].total_qty -= cur->qty_remaining;
    orders_.erase(order_id);
    pool_.deallocate(cur);

    while (bids_[best_bid_].total_qty == 0 && best_bid_ != 0) best_bid_--;
    while (asks_[best_ask_].total_qty == 0 && best_ask_ != 100) best_ask_++;

    return true;

}

uint8_t MatchingEngine::best_bid() const { return best_bid_; }
uint8_t MatchingEngine::best_ask() const { return best_ask_; }

uint32_t MatchingEngine::depth_at(Side side, uint8_t price) const {
    if (price < 1 || price > 99) return 0;
    if (side == Side::YES) return bids_[price].total_qty;
    return asks_[price].total_qty;
}

}