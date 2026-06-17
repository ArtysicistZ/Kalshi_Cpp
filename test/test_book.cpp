#include <gtest/gtest.h>

#include "feed/book.h"
#include "feed/parser.h"

using kalshi::Book;
using kalshi::OrderbookDelta;
using kalshi::Side;

namespace {

OrderbookDelta make_delta(Side side, uint8_t price, int32_t delta) {
    OrderbookDelta d{};
    d.side = side;
    d.price = price;
    d.delta = delta;
    return d;
}

}

TEST(BookTest, EmptyBookBestsAreZero) {
    Book book;
    EXPECT_EQ(book.best_yes_bid(), 0);
    EXPECT_EQ(book.best_no_bid(), 0);
    EXPECT_EQ(book.qty_at(Side::YES, 50), 0);
    EXPECT_EQ(book.qty_at(Side::NO, 50), 0);
}

TEST(BookTest, SingleLevelBecomesBest) {
    Book book;
    book.apply(make_delta(Side::YES, 50, 10));

    EXPECT_EQ(book.best_yes_bid(), 50);
    EXPECT_EQ(book.best_no_bid(), 0);
    EXPECT_EQ(book.qty_at(Side::YES, 50), 10);
}

TEST(BookTest, HigherLevelOverridesBest) {
    Book book;
    book.apply(make_delta(Side::YES, 50, 10));
    book.apply(make_delta(Side::YES, 60, 5));

    EXPECT_EQ(book.best_yes_bid(), 60);
    EXPECT_EQ(book.qty_at(Side::YES, 50), 10);
    EXPECT_EQ(book.qty_at(Side::YES, 60), 5);
}

TEST(BookTest, LowerLevelDoesNotChangeBest) {
    Book book;
    book.apply(make_delta(Side::YES, 60, 5));
    book.apply(make_delta(Side::YES, 40, 100));

    EXPECT_EQ(book.best_yes_bid(), 60);
    EXPECT_EQ(book.qty_at(Side::YES, 40), 100);
}

TEST(BookTest, DrainingTopRescansDown) {
    Book book;
    book.apply(make_delta(Side::YES, 50, 10));
    book.apply(make_delta(Side::YES, 60, 5));
    EXPECT_EQ(book.best_yes_bid(), 60);

    book.apply(make_delta(Side::YES, 60, -5));

    EXPECT_EQ(book.best_yes_bid(), 50);
    EXPECT_EQ(book.qty_at(Side::YES, 60), 0);
    EXPECT_EQ(book.qty_at(Side::YES, 50), 10);
}

TEST(BookTest, DrainingTopWithGapRescansFurtherDown) {
    Book book;
    book.apply(make_delta(Side::YES, 20, 7));
    book.apply(make_delta(Side::YES, 80, 3));
    EXPECT_EQ(book.best_yes_bid(), 80);

    book.apply(make_delta(Side::YES, 80, -3));

    EXPECT_EQ(book.best_yes_bid(), 20);
    EXPECT_EQ(book.qty_at(Side::YES, 80), 0);
}

TEST(BookTest, DrainingOnlyLevelGivesZero) {
    Book book;
    book.apply(make_delta(Side::YES, 50, 10));
    EXPECT_EQ(book.best_yes_bid(), 50);

    book.apply(make_delta(Side::YES, 50, -10));

    EXPECT_EQ(book.best_yes_bid(), 0);
    EXPECT_EQ(book.qty_at(Side::YES, 50), 0);
}

TEST(BookTest, NoSideIndependentOfYesSide) {
    Book book;
    book.apply(make_delta(Side::YES, 50, 10));
    book.apply(make_delta(Side::NO, 30, 20));

    EXPECT_EQ(book.best_yes_bid(), 50);
    EXPECT_EQ(book.best_no_bid(), 30);
    EXPECT_EQ(book.qty_at(Side::YES, 30), 0);
    EXPECT_EQ(book.qty_at(Side::NO, 50), 0);

    book.apply(make_delta(Side::NO, 30, -20));

    EXPECT_EQ(book.best_yes_bid(), 50);
    EXPECT_EQ(book.best_no_bid(), 0);
    EXPECT_EQ(book.qty_at(Side::YES, 50), 10);
}

TEST(BookTest, NoSideHigherIsBetterToo) {
    Book book;
    book.apply(make_delta(Side::NO, 25, 5));
    book.apply(make_delta(Side::NO, 70, 8));

    EXPECT_EQ(book.best_no_bid(), 70);

    book.apply(make_delta(Side::NO, 70, -8));

    EXPECT_EQ(book.best_no_bid(), 25);
}

TEST(BookTest, PartialDrainOfTopKeepsBest) {
    Book book;
    book.apply(make_delta(Side::YES, 50, 10));
    book.apply(make_delta(Side::YES, 50, -3));

    EXPECT_EQ(book.best_yes_bid(), 50);
    EXPECT_EQ(book.qty_at(Side::YES, 50), 7);
}

TEST(BookTest, BoundaryPrices) {
    Book book;
    book.apply(make_delta(Side::YES, 1, 5));
    book.apply(make_delta(Side::YES, 99, 2));

    EXPECT_EQ(book.best_yes_bid(), 99);

    book.apply(make_delta(Side::YES, 99, -2));

    EXPECT_EQ(book.best_yes_bid(), 1);

    book.apply(make_delta(Side::YES, 1, -5));

    EXPECT_EQ(book.best_yes_bid(), 0);
}
