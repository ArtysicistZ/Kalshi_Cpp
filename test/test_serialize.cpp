#include <gtest/gtest.h>
#include <simdjson.h>

#include <cstring>
#include <string>

#include "feed/parser.h"
#include "strategy/order_manager.h"
#include "net/serialize.h"

using kalshi::Action;
using kalshi::ActionKind;
using kalshi::Side;
using kalshi::serialize_action;

namespace {

Action place_action(Side s, uint8_t price, uint32_t qty, uint64_t id) {
    return Action{ActionKind::PLACE, s, price, qty, id};
}

Action cancel_action(Side s, uint64_t id) {
    return Action{ActionKind::CANCEL, s, 0, 0, id};
}

struct ParsedFields {
    std::string action;
    std::string ticker;
    std::string side;
    int64_t price = -1;
    int64_t qty = -1;
    uint64_t client_id = 0;
    bool has_side = false;
    bool has_price = false;
    bool has_qty = false;
};

ParsedFields parse_bytes(const char* bytes, size_t len) {
    ParsedFields f{};
    simdjson::padded_string padded(bytes, len);
    simdjson::ondemand::parser parser;
    auto doc = parser.iterate(padded);

    std::string_view sv;
    EXPECT_FALSE(doc["action"].get(sv));
    f.action = std::string(sv);
    EXPECT_FALSE(doc["ticker"].get(sv));
    f.ticker = std::string(sv);

    if (!doc["side"].get(sv)) {
        f.has_side = true;
        f.side = std::string(sv);
    }
    if (!doc["price"].get(f.price)) f.has_price = true;
    if (!doc["qty"].get(f.qty)) f.has_qty = true;
    EXPECT_FALSE(doc["client_id"].get(f.client_id));
    return f;
}

}

TEST(SerializeTest, PlaceRoundTripsThroughSimdjson) {
    char buf[256];
    Action a = place_action(Side::YES, 51, 10, 42);

    size_t n = serialize_action(a, "DEMO-1", buf, sizeof(buf));
    ASSERT_GT(n, 0u);

    auto f = parse_bytes(buf, n);
    EXPECT_EQ(f.action, "place");
    EXPECT_EQ(f.ticker, "DEMO-1");
    EXPECT_TRUE(f.has_side);
    EXPECT_EQ(f.side, "yes");
    EXPECT_TRUE(f.has_price);
    EXPECT_EQ(f.price, 51);
    EXPECT_TRUE(f.has_qty);
    EXPECT_EQ(f.qty, 10);
    EXPECT_EQ(f.client_id, 42u);
}

TEST(SerializeTest, CancelRoundTripsThroughSimdjson) {
    char buf[256];
    Action a = cancel_action(Side::YES, 99);

    size_t n = serialize_action(a, "DEMO-1", buf, sizeof(buf));
    ASSERT_GT(n, 0u);

    auto f = parse_bytes(buf, n);
    EXPECT_EQ(f.action, "cancel");
    EXPECT_EQ(f.ticker, "DEMO-1");
    EXPECT_FALSE(f.has_side);
    EXPECT_FALSE(f.has_price);
    EXPECT_FALSE(f.has_qty);
    EXPECT_EQ(f.client_id, 99u);
}

TEST(SerializeTest, NoSideSerializesCorrectly) {
    char buf[256];
    Action a = place_action(Side::NO, 31, 10, 7);

    size_t n = serialize_action(a, "INXD-24DEC31", buf, sizeof(buf));
    ASSERT_GT(n, 0u);

    auto f = parse_bytes(buf, n);
    EXPECT_EQ(f.side, "no");
    EXPECT_EQ(f.ticker, "INXD-24DEC31");
}

TEST(SerializeTest, BoundaryPricesSerialize) {
    char buf[256];

    for (uint8_t price : {uint8_t{1}, uint8_t{9}, uint8_t{10}, uint8_t{50}, uint8_t{99}}) {
        Action a = place_action(Side::YES, price, 10, 1);
        size_t n = serialize_action(a, "X", buf, sizeof(buf));
        ASSERT_GT(n, 0u);
        auto f = parse_bytes(buf, n);
        EXPECT_EQ(f.price, static_cast<int64_t>(price)) << "price=" << int(price);
    }
}

TEST(SerializeTest, LargeQtySerializes) {
    char buf[256];
    Action a = place_action(Side::YES, 50, UINT32_MAX, 1);

    size_t n = serialize_action(a, "X", buf, sizeof(buf));
    ASSERT_GT(n, 0u);

    auto f = parse_bytes(buf, n);
    EXPECT_EQ(static_cast<uint32_t>(f.qty), UINT32_MAX);
}

TEST(SerializeTest, LargeClientIdSerializes) {
    char buf[256];
    Action a = place_action(Side::YES, 50, 10, UINT64_MAX);

    size_t n = serialize_action(a, "X", buf, sizeof(buf));
    ASSERT_GT(n, 0u);

    auto f = parse_bytes(buf, n);
    EXPECT_EQ(f.client_id, UINT64_MAX);
}

TEST(SerializeTest, ZeroClientIdEmitsZero) {
    char buf[256];
    Action a = cancel_action(Side::YES, 0);

    size_t n = serialize_action(a, "X", buf, sizeof(buf));
    ASSERT_GT(n, 0u);

    std::string out(buf, n);
    EXPECT_NE(out.find("\"client_id\":0}"), std::string::npos) << out;
}

TEST(SerializeTest, TooSmallCapReturnsZero) {
    char buf[16];
    Action a = place_action(Side::YES, 50, 10, 1);

    size_t n = serialize_action(a, "DEMO-1", buf, sizeof(buf));
    EXPECT_EQ(n, 0u);
}

TEST(SerializeTest, CapAtBoundaryAccepts) {
    char buf[256];
    Action a = place_action(Side::YES, 50, 10, 1);

    size_t n = serialize_action(a, "DEMO-1", buf, 256);
    EXPECT_GT(n, 0u);

    size_t n2 = serialize_action(a, "DEMO-1", buf, 255);
    EXPECT_EQ(n2, 0u);
}

TEST(SerializeTest, ReturnedLengthMatchesBytesWritten) {
    char buf[256];
    std::memset(buf, 0xAA, sizeof(buf));
    Action a = place_action(Side::YES, 51, 10, 42);

    size_t n = serialize_action(a, "DEMO-1", buf, sizeof(buf));
    ASSERT_GT(n, 0u);

    EXPECT_EQ(static_cast<unsigned char>(buf[n - 1]), '}');
    EXPECT_EQ(static_cast<unsigned char>(buf[n]), 0xAA);
}

TEST(SerializeTest, KnownPlaceProducesExactBytes) {
    char buf[256];
    Action a = place_action(Side::YES, 51, 10, 42);

    size_t n = serialize_action(a, "DEMO-1", buf, sizeof(buf));
    std::string actual(buf, n);
    std::string expected =
        R"({"action":"place","ticker":"DEMO-1","side":"yes","price":51,"qty":10,"client_id":42})";

    EXPECT_EQ(actual, expected);
}

TEST(SerializeTest, KnownCancelProducesExactBytes) {
    char buf[256];
    Action a = cancel_action(Side::NO, 7);

    size_t n = serialize_action(a, "X", buf, sizeof(buf));
    std::string actual(buf, n);
    std::string expected =
        R"({"action":"cancel","ticker":"X","client_id":7})";

    EXPECT_EQ(actual, expected);
}
