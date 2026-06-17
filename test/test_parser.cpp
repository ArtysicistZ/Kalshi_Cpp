#include <gtest/gtest.h>
#include <simdjson.h>
#include <cstring>

#include "feed/parser.h"

using kalshi::OrderbookParser;
using kalshi::OrderbookDelta;
using kalshi::Side;

TEST(OrderbookParserTest, ParsesCanonicalDelta) {
    static constexpr const char* kPayload =
        R"({"type":"orderbook_delta","sid":12345,"seq":100,"msg":{"market_ticker":"DEMO-1","price":50,"delta":25,"side":"yes","ts":1234567890}})";

    simdjson::padded_string padded(kPayload, std::strlen(kPayload));
    OrderbookParser parser;
    OrderbookDelta out{};

    ASSERT_TRUE(parser.parse_orderbook_delta(padded, out));

    EXPECT_EQ(out.sid, 12345u);
    EXPECT_EQ(out.seq, 100u);
    EXPECT_STREQ(out.ticker, "DEMO-1");
    EXPECT_EQ(out.price, 50);
    EXPECT_EQ(out.delta, 25);
    EXPECT_EQ(out.side, Side::YES);
    EXPECT_EQ(out.ts, 1234567890);
}

TEST(OrderbookParserTest, ParsesNegativeDeltaAndNoSide) {
    static constexpr const char* kPayload =
        R"({"type":"orderbook_delta","sid":7,"seq":42,"msg":{"market_ticker":"INXD-24DEC31","price":99,"delta":-100,"side":"no","ts":-1}})";

    simdjson::padded_string padded(kPayload, std::strlen(kPayload));
    OrderbookParser parser;
    OrderbookDelta out{};

    ASSERT_TRUE(parser.parse_orderbook_delta(padded, out));

    EXPECT_EQ(out.sid, 7u);
    EXPECT_EQ(out.seq, 42u);
    EXPECT_STREQ(out.ticker, "INXD-24DEC31");
    EXPECT_EQ(out.price, 99);
    EXPECT_EQ(out.delta, -100);
    EXPECT_EQ(out.side, Side::NO);
    EXPECT_EQ(out.ts, -1);
}

TEST(OrderbookParserTest, ReusableAcrossManyCalls) {
    static constexpr const char* kPayload =
        R"({"type":"orderbook_delta","sid":1,"seq":1,"msg":{"market_ticker":"X","price":10,"delta":5,"side":"yes","ts":0}})";

    simdjson::padded_string padded(kPayload, std::strlen(kPayload));
    OrderbookParser parser;
    OrderbookDelta out{};

    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(parser.parse_orderbook_delta(padded, out));
        EXPECT_EQ(out.price, 10);
        EXPECT_EQ(out.delta, 5);
    }
}

TEST(OrderbookParserTest, RejectsPriceOutOfRange) {
    static constexpr const char* kZero =
        R"({"type":"orderbook_delta","sid":1,"seq":1,"msg":{"market_ticker":"X","price":0,"delta":5,"side":"yes","ts":0}})";
    static constexpr const char* kHundred =
        R"({"type":"orderbook_delta","sid":1,"seq":1,"msg":{"market_ticker":"X","price":100,"delta":5,"side":"yes","ts":0}})";

    OrderbookParser parser;
    OrderbookDelta out{};

    simdjson::padded_string p0(kZero, std::strlen(kZero));
    EXPECT_FALSE(parser.parse_orderbook_delta(p0, out));

    simdjson::padded_string p100(kHundred, std::strlen(kHundred));
    EXPECT_FALSE(parser.parse_orderbook_delta(p100, out));
}

TEST(OrderbookParserTest, RejectsUnknownSide) {
    static constexpr const char* kPayload =
        R"({"type":"orderbook_delta","sid":1,"seq":1,"msg":{"market_ticker":"X","price":50,"delta":5,"side":"maybe","ts":0}})";

    simdjson::padded_string padded(kPayload, std::strlen(kPayload));
    OrderbookParser parser;
    OrderbookDelta out{};

    EXPECT_FALSE(parser.parse_orderbook_delta(padded, out));
}

TEST(OrderbookParserTest, RejectsTickerTooLong) {
    static constexpr const char* kPayload =
        R"({"type":"orderbook_delta","sid":1,"seq":1,"msg":{"market_ticker":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","price":50,"delta":5,"side":"yes","ts":0}})";

    simdjson::padded_string padded(kPayload, std::strlen(kPayload));
    OrderbookParser parser;
    OrderbookDelta out{};

    EXPECT_FALSE(parser.parse_orderbook_delta(padded, out));
}
