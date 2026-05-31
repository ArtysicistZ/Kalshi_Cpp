#include <gtest/gtest.h>
#include <cstdint>

#include "sim/matching_engine.h"

using kalshi::sim::MatchingEngine;
using kalshi::sim::Side;

// ─────────────────────────────────────────────────────────────────────────
//  Empty-book / resting basics
// ─────────────────────────────────────────────────────────────────────────

TEST(MatchingEngineTest, EmptyBookObservers) {
    MatchingEngine eng;
    EXPECT_EQ(eng.best_bid(), 0);     // sentinel for "no bids"
    EXPECT_EQ(eng.best_ask(), 100);   // sentinel for "no asks"
}

TEST(MatchingEngineTest, YesBidRestsInEmptyBook) {
    MatchingEngine eng;
    auto r = eng.place_limit(/*client*/ 1, Side::YES, /*price*/ 60, /*qty*/ 50);

    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.fills.size(), 0u);
    EXPECT_EQ(r.qty_resting, 50u);
    EXPECT_NE(r.order_id, 0u);

    EXPECT_EQ(eng.best_bid(), 60);
    EXPECT_EQ(eng.depth_at(Side::YES, 60), 50u);
    EXPECT_EQ(eng.best_ask(), 100);
}

TEST(MatchingEngineTest, NoAskRestsInEmptyBook) {
    MatchingEngine eng;
    auto r = eng.place_limit(1, Side::NO, 70, 50);

    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.fills.size(), 0u);
    EXPECT_EQ(r.qty_resting, 50u);

    EXPECT_EQ(eng.best_ask(), 70);
    EXPECT_EQ(eng.depth_at(Side::NO, 70), 50u);
    EXPECT_EQ(eng.best_bid(), 0);
}

TEST(MatchingEngineTest, NoCrossingRestsOnly) {
    MatchingEngine eng;
    eng.place_limit(1, Side::NO,  70, 50);
    auto bid = eng.place_limit(2, Side::YES, 65, 30);

    EXPECT_EQ(bid.fills.size(), 0u);
    EXPECT_EQ(bid.qty_resting, 30u);
    EXPECT_EQ(eng.best_bid(), 65);
    EXPECT_EQ(eng.best_ask(), 70);
    EXPECT_EQ(eng.depth_at(Side::YES, 65), 30u);
    EXPECT_EQ(eng.depth_at(Side::NO,  70), 50u);
}

// ─────────────────────────────────────────────────────────────────────────
//  Single-level matching
// ─────────────────────────────────────────────────────────────────────────

TEST(MatchingEngineTest, ExactCrossingFullyFills) {
    MatchingEngine eng;
    auto ask = eng.place_limit(1, Side::NO,  70, 100);
    auto bid = eng.place_limit(2, Side::YES, 70, 100);

    ASSERT_EQ(bid.fills.size(), 1u);
    EXPECT_EQ(bid.fills[0].price, 70);
    EXPECT_EQ(bid.fills[0].qty, 100u);
    EXPECT_EQ(bid.fills[0].maker_order_id, ask.order_id);
    EXPECT_EQ(bid.fills[0].taker_order_id, bid.order_id);
    EXPECT_EQ(bid.qty_resting, 0u);

    EXPECT_EQ(eng.depth_at(Side::NO,  70), 0u);
    EXPECT_EQ(eng.depth_at(Side::YES, 70), 0u);
    EXPECT_EQ(eng.best_ask(), 100);
    EXPECT_EQ(eng.best_bid(), 0);
}

TEST(MatchingEngineTest, TakerSmallerThanRestingPartialFill) {
    MatchingEngine eng;
    eng.place_limit(1, Side::NO,  70, 100);
    auto bid = eng.place_limit(2, Side::YES, 70, 30);

    ASSERT_EQ(bid.fills.size(), 1u);
    EXPECT_EQ(bid.fills[0].qty, 30u);
    EXPECT_EQ(bid.qty_resting, 0u);

    EXPECT_EQ(eng.depth_at(Side::NO, 70), 70u);
    EXPECT_EQ(eng.best_ask(), 70);
    EXPECT_EQ(eng.best_bid(), 0);
}

TEST(MatchingEngineTest, TakerLargerThanRestingLeavesRemainderResting) {
    MatchingEngine eng;
    eng.place_limit(1, Side::NO,  70, 30);
    auto bid = eng.place_limit(2, Side::YES, 70, 100);

    ASSERT_EQ(bid.fills.size(), 1u);
    EXPECT_EQ(bid.fills[0].qty, 30u);
    EXPECT_EQ(bid.qty_resting, 70u);

    EXPECT_EQ(eng.depth_at(Side::NO,  70), 0u);
    EXPECT_EQ(eng.depth_at(Side::YES, 70), 70u);
    EXPECT_EQ(eng.best_ask(), 100);
    EXPECT_EQ(eng.best_bid(), 70);
}

TEST(MatchingEngineTest, PriceImprovementMakerPriceWins) {
    MatchingEngine eng;
    eng.place_limit(1, Side::NO,  60, 50);             // resting ask at 60
    auto bid = eng.place_limit(2, Side::YES, 70, 50);  // willing to pay up to 70

    ASSERT_EQ(bid.fills.size(), 1u);
    EXPECT_EQ(bid.fills[0].price, 60);                 // trades at maker's price
}

TEST(MatchingEngineTest, NoTakerMatchesAgainstYesBids) {
    MatchingEngine eng;
    auto bid = eng.place_limit(1, Side::YES, 60, 50);  // resting YES bid
    auto ask = eng.place_limit(2, Side::NO,  55, 30);  // crossing NO ask

    ASSERT_EQ(ask.fills.size(), 1u);
    EXPECT_EQ(ask.fills[0].price, 60);                 // maker's price
    EXPECT_EQ(ask.fills[0].qty, 30u);
    EXPECT_EQ(ask.fills[0].maker_order_id, bid.order_id);
    EXPECT_EQ(ask.fills[0].taker_order_id, ask.order_id);
}

// ─────────────────────────────────────────────────────────────────────────
//  Multi-level sweeps
// ─────────────────────────────────────────────────────────────────────────

TEST(MatchingEngineTest, MultiLevelSweep) {
    MatchingEngine eng;
    eng.place_limit(1, Side::NO, 60, 20);
    eng.place_limit(2, Side::NO, 62, 25);

    auto bid = eng.place_limit(3, Side::YES, 63, 40);

    ASSERT_EQ(bid.fills.size(), 2u);
    EXPECT_EQ(bid.fills[0].price, 60);
    EXPECT_EQ(bid.fills[0].qty, 20u);
    EXPECT_EQ(bid.fills[1].price, 62);
    EXPECT_EQ(bid.fills[1].qty, 20u);
    EXPECT_EQ(bid.qty_resting, 0u);

    EXPECT_EQ(eng.depth_at(Side::NO, 60), 0u);
    EXPECT_EQ(eng.depth_at(Side::NO, 62), 5u);
    EXPECT_EQ(eng.best_ask(), 62);
    EXPECT_EQ(eng.best_bid(), 0);
}

TEST(MatchingEngineTest, RestsRemainderWhenLimitTooLowForNextLevel) {
    MatchingEngine eng;
    eng.place_limit(1, Side::NO, 60, 20);
    eng.place_limit(2, Side::NO, 65, 50);              // beyond bid's limit

    auto bid = eng.place_limit(3, Side::YES, 62, 100);

    ASSERT_EQ(bid.fills.size(), 1u);
    EXPECT_EQ(bid.fills[0].price, 60);
    EXPECT_EQ(bid.fills[0].qty, 20u);
    EXPECT_EQ(bid.qty_resting, 80u);

    EXPECT_EQ(eng.depth_at(Side::NO,  60), 0u);
    EXPECT_EQ(eng.depth_at(Side::NO,  65), 50u);       // untouched
    EXPECT_EQ(eng.depth_at(Side::YES, 62), 80u);       // remainder rested

    EXPECT_EQ(eng.best_ask(), 65);
    EXPECT_EQ(eng.best_bid(), 62);
}

TEST(MatchingEngineTest, NoSideSweepsBidsDownward) {
    MatchingEngine eng;
    eng.place_limit(1, Side::YES, 60, 20);             // bid at 60
    eng.place_limit(2, Side::YES, 58, 25);             // bid at 58

    auto ask = eng.place_limit(3, Side::NO, 57, 40);

    ASSERT_EQ(ask.fills.size(), 2u);
    EXPECT_EQ(ask.fills[0].price, 60);                 // best bid first
    EXPECT_EQ(ask.fills[0].qty, 20u);
    EXPECT_EQ(ask.fills[1].price, 58);
    EXPECT_EQ(ask.fills[1].qty, 20u);
    EXPECT_EQ(ask.qty_resting, 0u);

    EXPECT_EQ(eng.depth_at(Side::YES, 60), 0u);
    EXPECT_EQ(eng.depth_at(Side::YES, 58), 5u);
    EXPECT_EQ(eng.best_bid(), 58);
}

// ─────────────────────────────────────────────────────────────────────────
//  Time priority within a level (FIFO)
// ─────────────────────────────────────────────────────────────────────────

TEST(MatchingEngineTest, TimePriorityFIFOAtSamePrice) {
    MatchingEngine eng;
    auto a = eng.place_limit(1, Side::YES, 60, 10);    // arrives first
    auto b = eng.place_limit(2, Side::YES, 60, 20);
    auto c = eng.place_limit(3, Side::YES, 60, 30);

    // Crossing ask consumes all three in arrival order
    auto ask = eng.place_limit(4, Side::NO, 60, 1000);

    ASSERT_EQ(ask.fills.size(), 3u);
    EXPECT_EQ(ask.fills[0].maker_order_id, a.order_id);
    EXPECT_EQ(ask.fills[1].maker_order_id, b.order_id);
    EXPECT_EQ(ask.fills[2].maker_order_id, c.order_id);
    EXPECT_EQ(ask.qty_resting, 940u);
}

// ─────────────────────────────────────────────────────────────────────────
//  Cancel: head / middle / tail / sole — verifies the intrusive list
//  stays consistent in every position.
// ─────────────────────────────────────────────────────────────────────────

TEST(MatchingEngineTest, CancelSoleOrderResetsSentinel) {
    MatchingEngine eng;
    auto r = eng.place_limit(1, Side::YES, 60, 50);
    ASSERT_EQ(eng.best_bid(), 60);

    EXPECT_TRUE(eng.cancel(r.order_id, 1));
    EXPECT_EQ(eng.depth_at(Side::YES, 60), 0u);
    EXPECT_EQ(eng.best_bid(), 0);
}

TEST(MatchingEngineTest, CancelHeadPreservesFIFO) {
    MatchingEngine eng;
    auto a = eng.place_limit(1, Side::YES, 60, 10);    // head
    auto b = eng.place_limit(2, Side::YES, 60, 20);    // middle
    auto c = eng.place_limit(3, Side::YES, 60, 30);    // tail

    ASSERT_TRUE(eng.cancel(a.order_id, 1));
    EXPECT_EQ(eng.depth_at(Side::YES, 60), 50u);

    // Crossing ask should now match b first, then c
    auto ask = eng.place_limit(4, Side::NO, 60, 25);
    ASSERT_EQ(ask.fills.size(), 2u);
    EXPECT_EQ(ask.fills[0].maker_order_id, b.order_id);
    EXPECT_EQ(ask.fills[0].qty, 20u);
    EXPECT_EQ(ask.fills[1].maker_order_id, c.order_id);
    EXPECT_EQ(ask.fills[1].qty, 5u);
}

TEST(MatchingEngineTest, CancelMiddlePreservesFIFOOfRest) {
    MatchingEngine eng;
    auto a = eng.place_limit(1, Side::YES, 60, 10);
    auto b = eng.place_limit(2, Side::YES, 60, 20);
    auto c = eng.place_limit(3, Side::YES, 60, 30);

    ASSERT_TRUE(eng.cancel(b.order_id, 2));
    EXPECT_EQ(eng.depth_at(Side::YES, 60), 40u);

    auto ask = eng.place_limit(4, Side::NO, 60, 1000);
    ASSERT_EQ(ask.fills.size(), 2u);
    EXPECT_EQ(ask.fills[0].maker_order_id, a.order_id);
    EXPECT_EQ(ask.fills[1].maker_order_id, c.order_id);
}

TEST(MatchingEngineTest, CancelTailAllowsCleanAppend) {
    MatchingEngine eng;
    auto a = eng.place_limit(1, Side::YES, 60, 10);
    auto b = eng.place_limit(2, Side::YES, 60, 20);
    auto c = eng.place_limit(3, Side::YES, 60, 30);

    ASSERT_TRUE(eng.cancel(c.order_id, 3));
    auto d = eng.place_limit(4, Side::YES, 60, 40);    // new tail
    EXPECT_EQ(eng.depth_at(Side::YES, 60), 70u);

    auto ask = eng.place_limit(5, Side::NO, 60, 1000);
    ASSERT_EQ(ask.fills.size(), 3u);
    EXPECT_EQ(ask.fills[0].maker_order_id, a.order_id);
    EXPECT_EQ(ask.fills[1].maker_order_id, b.order_id);
    EXPECT_EQ(ask.fills[2].maker_order_id, d.order_id);
}

// ─────────────────────────────────────────────────────────────────────────
//  Cancel: authorization & error paths
// ─────────────────────────────────────────────────────────────────────────

TEST(MatchingEngineTest, CancelWrongClientFails) {
    MatchingEngine eng;
    auto r = eng.place_limit(1, Side::YES, 60, 50);

    EXPECT_FALSE(eng.cancel(r.order_id, /*wrong client*/ 999));
    EXPECT_EQ(eng.depth_at(Side::YES, 60), 50u);       // book unchanged
    EXPECT_EQ(eng.best_bid(), 60);
}

TEST(MatchingEngineTest, CancelUnknownOrderFails) {
    MatchingEngine eng;
    EXPECT_FALSE(eng.cancel(/*never existed*/ 12345, 1));

    // After placing one real order, an unrelated id still fails.
    auto r = eng.place_limit(1, Side::YES, 60, 50);
    EXPECT_FALSE(eng.cancel(r.order_id + 9999, 1));
    EXPECT_EQ(eng.depth_at(Side::YES, 60), 50u);
}

// ─────────────────────────────────────────────────────────────────────────
//  Validation
// ─────────────────────────────────────────────────────────────────────────

TEST(MatchingEngineTest, RejectsInvalidPriceOrQty) {
    MatchingEngine eng;

    auto r0 = eng.place_limit(1, Side::YES,   0, 50);
    EXPECT_FALSE(r0.accepted);
    EXPECT_EQ(r0.order_id, 0u);

    auto r1 = eng.place_limit(1, Side::YES, 100, 50);
    EXPECT_FALSE(r1.accepted);

    auto r2 = eng.place_limit(1, Side::YES,  50,  0);
    EXPECT_FALSE(r2.accepted);

    // Engine state untouched
    EXPECT_EQ(eng.best_bid(), 0);
    EXPECT_EQ(eng.best_ask(), 100);
}

TEST(MatchingEngineTest, DepthAtBoundaryReturnsZero) {
    MatchingEngine eng;
    EXPECT_EQ(eng.depth_at(Side::YES, 0),   0u);
    EXPECT_EQ(eng.depth_at(Side::YES, 100), 0u);
    EXPECT_EQ(eng.depth_at(Side::NO,  0),   0u);
    EXPECT_EQ(eng.depth_at(Side::NO,  100), 0u);
}

// ─────────────────────────────────────────────────────────────────────────
//  Pool recycling — the regression test for Bug 2 (uninitialized list
//  pointers after Pool::allocate). Each iteration fills both Orders and
//  returns them to the pool; subsequent allocations recycle stale slots.
//  Before the fix this corrupts the book within a few iterations.
// ─────────────────────────────────────────────────────────────────────────

TEST(MatchingEngineTest, PoolRecyclingDoesNotCorruptList) {
    MatchingEngine eng;

    for (int i = 0; i < 200; ++i) {
        auto ask = eng.place_limit(1, Side::NO,  70, 1);
        ASSERT_TRUE(ask.accepted)                  << "iter " << i;
        ASSERT_EQ(eng.depth_at(Side::NO, 70), 1u)  << "iter " << i;
        ASSERT_EQ(eng.best_ask(), 70)              << "iter " << i;

        auto bid = eng.place_limit(2, Side::YES, 70, 1);
        ASSERT_TRUE(bid.accepted)                  << "iter " << i;
        ASSERT_EQ(bid.fills.size(), 1u)            << "iter " << i;
        ASSERT_EQ(bid.fills[0].qty, 1u)            << "iter " << i;
        ASSERT_EQ(bid.qty_resting, 0u)             << "iter " << i;

        ASSERT_EQ(eng.depth_at(Side::NO,  70), 0u) << "iter " << i;
        ASSERT_EQ(eng.depth_at(Side::YES, 70), 0u) << "iter " << i;
        ASSERT_EQ(eng.best_ask(), 100)             << "iter " << i;
        ASSERT_EQ(eng.best_bid(), 0)               << "iter " << i;
    }
}

TEST(MatchingEngineTest, PoolRecyclingWithCancelDoesNotCorruptList) {
    MatchingEngine eng;

    // Recycle slots via cancel rather than match.
    for (int i = 0; i < 200; ++i) {
        auto r = eng.place_limit(1, Side::YES, 60, 10);
        ASSERT_TRUE(r.accepted)                    << "iter " << i;
        ASSERT_TRUE(eng.cancel(r.order_id, 1))     << "iter " << i;
        ASSERT_EQ(eng.depth_at(Side::YES, 60), 0u) << "iter " << i;
        ASSERT_EQ(eng.best_bid(), 0)               << "iter " << i;
    }
}
