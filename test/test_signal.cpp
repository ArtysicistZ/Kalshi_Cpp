#include <gtest/gtest.h>

#include "feed/book.h"
#include "feed/parser.h"
#include "strategy/signal.h"

using kalshi::Book;
using kalshi::OrderbookDelta;
using kalshi::Side;
using kalshi::Signal;
using kalshi::TargetQuote;

namespace {

OrderbookDelta make_delta(Side side, uint8_t price, int32_t delta) {
    OrderbookDelta d{};
    d.side = side;
    d.price = price;
    d.delta = delta;
    return d;
}

}

TEST(SignalTest, EmptyBookEmitsInactive) {
    Book book;
    Signal sig;

    TargetQuote q = sig.evaluate(book);

    EXPECT_FALSE(q.active);
}

TEST(SignalTest, OnlyYesSideEmitsInactive) {
    Book book;
    book.apply(make_delta(Side::YES, 50, 10));
    Signal sig;

    TargetQuote q = sig.evaluate(book);

    EXPECT_FALSE(q.active);
}

TEST(SignalTest, OnlyNoSideEmitsInactive) {
    Book book;
    book.apply(make_delta(Side::NO, 30, 10));
    Signal sig;

    TargetQuote q = sig.evaluate(book);

    EXPECT_FALSE(q.active);
}

TEST(SignalTest, TightSpreadOneEmitsInactive) {
    Book book;
    book.apply(make_delta(Side::YES, 50, 10));
    book.apply(make_delta(Side::NO, 49, 10));
    Signal sig;

    TargetQuote q = sig.evaluate(book);

    EXPECT_FALSE(q.active);
}

TEST(SignalTest, TightSpreadTwoEmitsInactive) {
    Book book;
    book.apply(make_delta(Side::YES, 50, 10));
    book.apply(make_delta(Side::NO, 48, 10));
    Signal sig;

    TargetQuote q = sig.evaluate(book);

    EXPECT_FALSE(q.active);
}

TEST(SignalTest, ExactlyMinSpreadEmitsActive) {
    Book book;
    book.apply(make_delta(Side::YES, 50, 10));
    book.apply(make_delta(Side::NO, 47, 10));
    Signal sig;

    TargetQuote q = sig.evaluate(book);

    EXPECT_TRUE(q.active);
    EXPECT_EQ(q.yes_price, 51);
    EXPECT_EQ(q.no_price, 48);
    EXPECT_EQ(q.qty, Signal::DEFAULT_QTY);
}

TEST(SignalTest, WideSpreadEmitsImprovingQuote) {
    Book book;
    book.apply(make_delta(Side::YES, 40, 10));
    book.apply(make_delta(Side::NO, 30, 10));
    Signal sig;

    TargetQuote q = sig.evaluate(book);

    EXPECT_TRUE(q.active);
    EXPECT_EQ(q.yes_price, 41);
    EXPECT_EQ(q.no_price, 31);
    EXPECT_EQ(q.qty, Signal::DEFAULT_QTY);
}

TEST(SignalTest, BoundaryLowPricesActive) {
    Book book;
    book.apply(make_delta(Side::YES, 50, 10));
    book.apply(make_delta(Side::NO, 1, 10));
    Signal sig;

    TargetQuote q = sig.evaluate(book);

    EXPECT_TRUE(q.active);
    EXPECT_EQ(q.yes_price, 51);
    EXPECT_EQ(q.no_price, 2);
}

TEST(SignalTest, BookTopChangesAreReflected) {
    Book book;
    book.apply(make_delta(Side::YES, 40, 10));
    book.apply(make_delta(Side::NO, 30, 10));
    Signal sig;

    TargetQuote q1 = sig.evaluate(book);
    EXPECT_EQ(q1.yes_price, 41);

    book.apply(make_delta(Side::YES, 45, 5));

    TargetQuote q2 = sig.evaluate(book);
    EXPECT_TRUE(q2.active);
    EXPECT_EQ(q2.yes_price, 46);
    EXPECT_EQ(q2.no_price, 31);
}

TEST(SignalTest, EvaluateIsConstAndPure) {
    Book book;
    book.apply(make_delta(Side::YES, 40, 10));
    book.apply(make_delta(Side::NO, 30, 10));
    Signal sig;

    TargetQuote q1 = sig.evaluate(book);
    TargetQuote q2 = sig.evaluate(book);

    EXPECT_EQ(q1.active, q2.active);
    EXPECT_EQ(q1.yes_price, q2.yes_price);
    EXPECT_EQ(q1.no_price, q2.no_price);
    EXPECT_EQ(q1.qty, q2.qty);
}
