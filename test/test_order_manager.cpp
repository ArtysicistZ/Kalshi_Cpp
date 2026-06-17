#include <gtest/gtest.h>

#include "feed/parser.h"
#include "strategy/signal.h"
#include "strategy/order_manager.h"

using kalshi::Action;
using kalshi::ActionBundle;
using kalshi::ActionKind;
using kalshi::OrderManager;
using kalshi::Side;
using kalshi::TargetQuote;

namespace {

TargetQuote inactive() {
    return TargetQuote{};
}

TargetQuote active_at(uint8_t yes_price, uint8_t no_price, uint32_t qty = 10) {
    return TargetQuote{.active = true,
                       .yes_price = yes_price,
                       .no_price = no_price,
                       .qty = qty};
}

const Action* find_action(const ActionBundle& b, ActionKind k, Side s) {
    for (uint8_t i = 0; i < b.cnt; ++i) {
        if (b.actions[i].kind == k && b.actions[i].side == s) {
            return &b.actions[i];
        }
    }
    return nullptr;
}

}

TEST(OrderManagerTest, IdleWithNoTargetEmitsNothing) {
    OrderManager om;

    ActionBundle b = om.reconcile(inactive());

    EXPECT_EQ(b.cnt, 0);
}

TEST(OrderManagerTest, ActiveTargetOnFreshManagerEmitsTwoPlaces) {
    OrderManager om;

    ActionBundle b = om.reconcile(active_at(41, 31));

    ASSERT_EQ(b.cnt, 2);

    const Action* yes = find_action(b, ActionKind::PLACE, Side::YES);
    const Action* no  = find_action(b, ActionKind::PLACE, Side::NO);
    ASSERT_NE(yes, nullptr);
    ASSERT_NE(no, nullptr);

    EXPECT_EQ(yes->price, 41);
    EXPECT_EQ(yes->qty, 10u);
    EXPECT_EQ(no->price, 31);
    EXPECT_EQ(no->qty, 10u);

    EXPECT_NE(yes->client_id, no->client_id);
    EXPECT_GT(yes->client_id, 0u);
    EXPECT_GT(no->client_id, 0u);
}

TEST(OrderManagerTest, IdempotentWhenStateMatchesTarget) {
    OrderManager om;

    ActionBundle first = om.reconcile(active_at(41, 31));
    ASSERT_EQ(first.cnt, 2);
    for (uint8_t i = 0; i < first.cnt; ++i) {
        om.on_place_sent(first.actions[i]);
    }

    ActionBundle second = om.reconcile(active_at(41, 31));

    EXPECT_EQ(second.cnt, 0);
}

TEST(OrderManagerTest, PriceChangeEmitsCancelThenPlace) {
    OrderManager om;

    ActionBundle initial = om.reconcile(active_at(41, 31));
    uint64_t old_yes_id = find_action(initial, ActionKind::PLACE, Side::YES)->client_id;
    uint64_t old_no_id  = find_action(initial, ActionKind::PLACE, Side::NO)->client_id;
    for (uint8_t i = 0; i < initial.cnt; ++i) om.on_place_sent(initial.actions[i]);

    ActionBundle b = om.reconcile(active_at(42, 31));

    ASSERT_EQ(b.cnt, 2);

    EXPECT_EQ(b.actions[0].kind, ActionKind::CANCEL);
    EXPECT_EQ(b.actions[0].side, Side::YES);
    EXPECT_EQ(b.actions[0].client_id, old_yes_id);

    EXPECT_EQ(b.actions[1].kind, ActionKind::PLACE);
    EXPECT_EQ(b.actions[1].side, Side::YES);
    EXPECT_EQ(b.actions[1].price, 42);
    EXPECT_NE(b.actions[1].client_id, old_yes_id);
    EXPECT_NE(b.actions[1].client_id, old_no_id);
}

TEST(OrderManagerTest, TargetGoesInactiveEmitsTwoCancels) {
    OrderManager om;

    ActionBundle initial = om.reconcile(active_at(41, 31));
    uint64_t yes_id = find_action(initial, ActionKind::PLACE, Side::YES)->client_id;
    uint64_t no_id  = find_action(initial, ActionKind::PLACE, Side::NO)->client_id;
    for (uint8_t i = 0; i < initial.cnt; ++i) om.on_place_sent(initial.actions[i]);

    ActionBundle b = om.reconcile(inactive());

    ASSERT_EQ(b.cnt, 2);

    const Action* cy = find_action(b, ActionKind::CANCEL, Side::YES);
    const Action* cn = find_action(b, ActionKind::CANCEL, Side::NO);
    ASSERT_NE(cy, nullptr);
    ASSERT_NE(cn, nullptr);
    EXPECT_EQ(cy->client_id, yes_id);
    EXPECT_EQ(cn->client_id, no_id);
}

TEST(OrderManagerTest, SidesAreIndependent) {
    OrderManager om;

    ActionBundle initial = om.reconcile(active_at(41, 31));
    for (uint8_t i = 0; i < initial.cnt; ++i) om.on_place_sent(initial.actions[i]);

    ActionBundle b = om.reconcile(active_at(42, 31));

    ASSERT_EQ(b.cnt, 2);
    EXPECT_EQ(b.actions[0].side, Side::YES);
    EXPECT_EQ(b.actions[1].side, Side::YES);
    EXPECT_EQ(find_action(b, ActionKind::CANCEL, Side::NO), nullptr);
    EXPECT_EQ(find_action(b, ActionKind::PLACE, Side::NO), nullptr);
}

TEST(OrderManagerTest, OnCancelSentReopensSlotForFuturePlace) {
    OrderManager om;

    ActionBundle initial = om.reconcile(active_at(41, 31));
    for (uint8_t i = 0; i < initial.cnt; ++i) om.on_place_sent(initial.actions[i]);

    ActionBundle cancelled = om.reconcile(inactive());
    ASSERT_EQ(cancelled.cnt, 2);
    for (uint8_t i = 0; i < cancelled.cnt; ++i) om.on_cancel_sent(cancelled.actions[i]);

    ActionBundle b = om.reconcile(active_at(50, 25));

    ASSERT_EQ(b.cnt, 2);
    EXPECT_EQ(b.actions[0].kind, ActionKind::PLACE);
    EXPECT_EQ(b.actions[1].kind, ActionKind::PLACE);
}

TEST(OrderManagerTest, ClientIdsAreMonotonicAcrossPlaces) {
    OrderManager om;

    ActionBundle b1 = om.reconcile(active_at(41, 31));
    uint64_t id_a = b1.actions[0].client_id;
    uint64_t id_b = b1.actions[1].client_id;
    EXPECT_LT(id_a, id_b);

    for (uint8_t i = 0; i < b1.cnt; ++i) om.on_place_sent(b1.actions[i]);

    ActionBundle b2 = om.reconcile(active_at(42, 32));
    ASSERT_EQ(b2.cnt, 4);

    uint64_t prev = id_b;
    for (uint8_t i = 0; i < b2.cnt; ++i) {
        if (b2.actions[i].kind == ActionKind::PLACE) {
            EXPECT_GT(b2.actions[i].client_id, prev);
            prev = b2.actions[i].client_id;
        }
    }
}

TEST(OrderManagerTest, OnlyOneSideChangesEmitsTwoActions) {
    OrderManager om;

    ActionBundle initial = om.reconcile(active_at(41, 31));
    uint64_t yes_id = find_action(initial, ActionKind::PLACE, Side::YES)->client_id;
    for (uint8_t i = 0; i < initial.cnt; ++i) om.on_place_sent(initial.actions[i]);

    ActionBundle b = om.reconcile(active_at(45, 31));

    ASSERT_EQ(b.cnt, 2);
    EXPECT_EQ(b.actions[0].kind, ActionKind::CANCEL);
    EXPECT_EQ(b.actions[0].side, Side::YES);
    EXPECT_EQ(b.actions[0].client_id, yes_id);
    EXPECT_EQ(b.actions[1].kind, ActionKind::PLACE);
    EXPECT_EQ(b.actions[1].side, Side::YES);
    EXPECT_EQ(b.actions[1].price, 45);
}

TEST(OrderManagerTest, FullChurnEmitsFourActions) {
    OrderManager om;

    ActionBundle initial = om.reconcile(active_at(41, 31));
    for (uint8_t i = 0; i < initial.cnt; ++i) om.on_place_sent(initial.actions[i]);

    ActionBundle b = om.reconcile(active_at(42, 32));

    EXPECT_EQ(b.cnt, 4);
}
