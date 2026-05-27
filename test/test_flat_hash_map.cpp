#include <gtest/gtest.h>
#include <cstdint>
#include <unordered_set>
#include <vector>
#include <utility>
#include "core/flat_hash_map.h"

using kalshi::FlatHashMap;

// ── Basic round-trip ──

TEST(FlatHashMapTest, EmptyOnConstruction) {
    FlatHashMap<uint64_t, uint64_t, 16> map;
    EXPECT_EQ(map.size(), 0u);
    EXPECT_TRUE(map.empty());
}

TEST(FlatHashMapTest, CapacityReturnsTemplateArg) {
    FlatHashMap<uint64_t, uint64_t, 64> map;
    EXPECT_EQ(map.capacity(), 64u);
}

TEST(FlatHashMapTest, InsertOneAndFind) {
    FlatHashMap<uint64_t, uint64_t, 16> map;
    EXPECT_TRUE(map.insert(5, 100));
    EXPECT_EQ(map.size(), 1u);
    EXPECT_FALSE(map.empty());

    uint64_t* v = map.find(5);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, 100u);
}

TEST(FlatHashMapTest, FindMissingReturnsNull) {
    FlatHashMap<uint64_t, uint64_t, 16> map;
    EXPECT_EQ(map.find(99), nullptr);

    map.insert(5, 100);
    EXPECT_EQ(map.find(99), nullptr);   // still missing
    EXPECT_NE(map.find(5), nullptr);
}

// ── Update semantics (no duplicates) ──

TEST(FlatHashMapTest, InsertSameKeyUpdatesValue) {
    FlatHashMap<uint64_t, uint64_t, 16> map;
    map.insert(5, 100);
    map.insert(5, 200);
    map.insert(5, 300);

    EXPECT_EQ(map.size(), 1u);   // not 3
    uint64_t* v = map.find(5);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, 300u);
}

// ── Erase ──

TEST(FlatHashMapTest, EraseRemovesKey) {
    FlatHashMap<uint64_t, uint64_t, 16> map;
    map.insert(5, 100);
    ASSERT_NE(map.find(5), nullptr);

    EXPECT_TRUE(map.erase(5));
    EXPECT_EQ(map.find(5), nullptr);
    EXPECT_EQ(map.size(), 0u);
}

TEST(FlatHashMapTest, EraseMissingReturnsFalse) {
    FlatHashMap<uint64_t, uint64_t, 16> map;
    EXPECT_FALSE(map.erase(99));

    map.insert(5, 100);
    EXPECT_FALSE(map.erase(99));
    EXPECT_TRUE(map.erase(5));
    EXPECT_FALSE(map.erase(5));      // already gone
}

TEST(FlatHashMapTest, EraseReducesSize) {
    FlatHashMap<uint64_t, uint64_t, 32> map;
    for (uint64_t i = 0; i < 10; ++i) map.insert(i, i * 7);
    EXPECT_EQ(map.size(), 10u);

    for (uint64_t i = 0; i < 5; ++i) {
        EXPECT_TRUE(map.erase(i));
    }
    EXPECT_EQ(map.size(), 5u);

    for (uint64_t i = 0; i < 5; ++i)  EXPECT_EQ(map.find(i), nullptr);
    for (uint64_t i = 5; i < 10; ++i) EXPECT_NE(map.find(i), nullptr);
}

TEST(FlatHashMapTest, ReinsertAfterErase) {
    FlatHashMap<uint64_t, uint64_t, 16> map;
    map.insert(5, 100);
    map.erase(5);
    EXPECT_EQ(map.find(5), nullptr);

    EXPECT_TRUE(map.insert(5, 999));
    uint64_t* v = map.find(5);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, 999u);
    EXPECT_EQ(map.size(), 1u);
}

// ── Stress: many keys, no collisions of identity ──

TEST(FlatHashMapTest, ManyInsertsNoDuplicates) {
    constexpr size_t N = 1000;
    FlatHashMap<uint64_t, uint64_t, 4096> map;

    for (uint64_t i = 0; i < N; ++i) {
        ASSERT_TRUE(map.insert(i, i * 13 + 1));
    }
    EXPECT_EQ(map.size(), N);

    // Every key must be findable with the correct value
    for (uint64_t i = 0; i < N; ++i) {
        uint64_t* v = map.find(i);
        ASSERT_NE(v, nullptr) << "missing key " << i;
        EXPECT_EQ(*v, i * 13 + 1);
    }
}

// ── Backshift correctness ──
// After erasing keys in the middle of probe chains, all survivors must still
// be findable. This is the test that catches a broken backshift implementation.

TEST(FlatHashMapTest, BackshiftPreservesLookups) {
    constexpr size_t N = 2000;
    FlatHashMap<uint64_t, uint64_t, 4096> map;

    // Insert N keys
    for (uint64_t i = 0; i < N; ++i) {
        ASSERT_TRUE(map.insert(i, i + 1000));
    }

    // Erase every odd key
    for (uint64_t i = 1; i < N; i += 2) {
        ASSERT_TRUE(map.erase(i));
    }
    EXPECT_EQ(map.size(), N / 2);

    // Every even key must still resolve to the right value
    for (uint64_t i = 0; i < N; i += 2) {
        uint64_t* v = map.find(i);
        ASSERT_NE(v, nullptr) << "lost even key " << i;
        EXPECT_EQ(*v, i + 1000);
    }
    // Every odd key must be gone
    for (uint64_t i = 1; i < N; i += 2) {
        EXPECT_EQ(map.find(i), nullptr) << "stale odd key " << i;
    }
}

// ── Probe-chain stress: erase + reinsert + verify ──

TEST(FlatHashMapTest, ChurnPreservesAllSurvivors) {
    constexpr size_t N = 1000;
    FlatHashMap<uint64_t, uint64_t, 4096> map;

    for (uint64_t i = 0; i < N; ++i) map.insert(i, i);

    // Repeated cycles: erase key k, reinsert with new value, verify
    for (uint64_t round = 0; round < 5; ++round) {
        for (uint64_t i = 0; i < N; ++i) {
            ASSERT_TRUE(map.erase(i));
            ASSERT_TRUE(map.insert(i, i + round * 10000));
        }
    }

    EXPECT_EQ(map.size(), N);
    for (uint64_t i = 0; i < N; ++i) {
        uint64_t* v = map.find(i);
        ASSERT_NE(v, nullptr);
        EXPECT_EQ(*v, i + 4 * 10000u);
    }
}

// ── Value/pointer return semantics ──

TEST(FlatHashMapTest, FindReturnsMutablePointer) {
    FlatHashMap<uint64_t, uint64_t, 16> map;
    map.insert(5, 100);

    uint64_t* v = map.find(5);
    ASSERT_NE(v, nullptr);
    *v = 999;                              // mutate through the pointer

    uint64_t* v2 = map.find(5);
    ASSERT_NE(v2, nullptr);
    EXPECT_EQ(*v2, 999u);                  // see the mutation
}

// ── Both lvalue and rvalue must be accepted by insert ──

TEST(FlatHashMapTest, InsertAcceptsLvalueAndRvalue) {
    FlatHashMap<uint64_t, uint64_t, 16> map;

    uint64_t lvalue = 100;
    EXPECT_TRUE(map.insert(1, lvalue));         // lvalue
    EXPECT_TRUE(map.insert(2, 200));            // prvalue
    EXPECT_TRUE(map.insert(3, std::move(lvalue))); // xvalue

    EXPECT_NE(map.find(1), nullptr);
    EXPECT_NE(map.find(2), nullptr);
    EXPECT_NE(map.find(3), nullptr);
}

// ── Capacity boundary ──
// Confirm that when the table reaches its load limit, insert returns false
// rather than corrupting state. Capacity here is intentionally tiny.

TEST(FlatHashMapTest, InsertReturnsFalseWhenFull) {
    constexpr size_t CAP = 8;
    FlatHashMap<uint64_t, uint64_t, CAP> map;

    size_t inserted = 0;
    for (uint64_t i = 0; i < CAP * 2; ++i) {
        if (map.insert(i, i)) ++inserted;
    }
    // Map should accept up to CAP entries, then refuse
    EXPECT_LE(inserted, CAP);
    EXPECT_EQ(map.size(), inserted);
}

// ── Stress: random insert/erase patterns ──

TEST(FlatHashMapTest, RandomInsertEraseStaysConsistent) {
    constexpr size_t N = 500;
    FlatHashMap<uint64_t, uint64_t, 4096> map;
    std::unordered_set<uint64_t> reference;

    // Use a deterministic-but-irregular pattern
    auto next = [](uint64_t x) { return (x * 2654435761u + 1) & 0xFFFFu; };

    uint64_t seed = 1;
    for (size_t step = 0; step < N * 4; ++step) {
        seed = next(seed);
        if (reference.count(seed) == 0 && reference.size() < N) {
            map.insert(seed, seed * 3);
            reference.insert(seed);
        } else if (reference.count(seed)) {
            map.erase(seed);
            reference.erase(seed);
        }
    }

    EXPECT_EQ(map.size(), reference.size());
    for (uint64_t k : reference) {
        uint64_t* v = map.find(k);
        ASSERT_NE(v, nullptr) << "missing key " << k;
        EXPECT_EQ(*v, k * 3);
    }
}
