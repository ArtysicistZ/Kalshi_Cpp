#include <gtest/gtest.h>
#include <cstdint>
#include <set>
#include "core/pool_alloc.h"

using kalshi::Pool;

// A POD type at least sizeof(void*) so the pool's static_assert passes
struct Block64 {
    uint64_t a;
    uint64_t b;
    uint64_t c;
    uint64_t d;
};

TEST(PoolTest, AllocateOne) {
    Pool<Block64, 4> pool;
    Block64* p = pool.allocate();
    EXPECT_NE(p, nullptr);
}

TEST(PoolTest, FullSignalsCorrectly) {
    Pool<Block64, 4> pool;
    EXPECT_FALSE(pool.full());

    for (size_t i = 0; i < 4; ++i) {
        EXPECT_NE(pool.allocate(), nullptr);
    }
    EXPECT_TRUE(pool.full());

    // Next allocation fails
    EXPECT_EQ(pool.allocate(), nullptr);
}

TEST(PoolTest, DeallocateMakesAvailable) {
    Pool<Block64, 2> pool;
    Block64* a = pool.allocate();
    Block64* b = pool.allocate();
    ASSERT_TRUE(pool.full());

    pool.deallocate(a);
    EXPECT_FALSE(pool.full());

    Block64* c = pool.allocate();
    EXPECT_NE(c, nullptr);
    EXPECT_TRUE(pool.full());

    // Cleanup so destructor doesn't see dangling state (not strictly necessary)
    pool.deallocate(b);
    pool.deallocate(c);
}

TEST(PoolTest, LIFOOrder) {
    Pool<Block64, 4> pool;
    Block64* a = pool.allocate();
    Block64* b = pool.allocate();
    Block64* c = pool.allocate();

    pool.deallocate(b);
    pool.deallocate(c);

    // Most recently deallocated should come out first
    EXPECT_EQ(pool.allocate(), c);
    EXPECT_EQ(pool.allocate(), b);

    pool.deallocate(a);
}

TEST(PoolTest, AllPointersAreUnique) {
    constexpr size_t N = 64;
    Pool<Block64, N> pool;
    std::set<Block64*> seen;

    for (size_t i = 0; i < N; ++i) {
        Block64* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(seen.count(p), 0u);   // never returned before
        seen.insert(p);
    }
    EXPECT_EQ(seen.size(), N);
    EXPECT_TRUE(pool.full());
}

TEST(PoolTest, ReuseAfterFullCycle) {
    constexpr size_t N = 8;
    Pool<Block64, N> pool;
    Block64* ptrs[N];

    // Fill
    for (size_t i = 0; i < N; ++i) ptrs[i] = pool.allocate();
    ASSERT_TRUE(pool.full());

    // Drain
    for (size_t i = 0; i < N; ++i) pool.deallocate(ptrs[i]);
    EXPECT_FALSE(pool.full());

    // Refill — must succeed N times again
    std::set<Block64*> seen;
    for (size_t i = 0; i < N; ++i) {
        Block64* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        seen.insert(p);
    }
    EXPECT_EQ(seen.size(), N);
    EXPECT_TRUE(pool.full());
}

TEST(PoolTest, CanWriteAndReadAllocatedMemory) {
    Pool<Block64, 4> pool;
    Block64* p = pool.allocate();
    ASSERT_NE(p, nullptr);

    // Writing into allocated memory should not corrupt the pool
    p->a = 0xDEADBEEF;
    p->b = 0xCAFEBABE;
    p->c = 0x12345678;
    p->d = 0x87654321;

    EXPECT_EQ(p->a, 0xDEADBEEFu);
    EXPECT_EQ(p->b, 0xCAFEBABEu);
    EXPECT_EQ(p->c, 0x12345678u);
    EXPECT_EQ(p->d, 0x87654321u);

    pool.deallocate(p);

    // Pool should still work after deallocation overwrote those bytes
    Block64* q = pool.allocate();
    EXPECT_NE(q, nullptr);
}

// A non-trivial type to exercise placement new + explicit destructor
struct Counted {
    static int live_count;
    int value;

    Counted(int v) : value(v) { ++live_count; }
    ~Counted() { --live_count; }
};
int Counted::live_count = 0;

TEST(PoolTest, PlacementNewAndExplicitDestructor) {
    Counted::live_count = 0;

    // Counted is small — pad it to satisfy pool's sizeof requirement
    struct PaddedCounted {
        Counted c;
        char pad[sizeof(void*)];
        PaddedCounted(int v) : c(v) {}
    };

    Pool<PaddedCounted, 4> pool;

    PaddedCounted* a = pool.allocate();
    PaddedCounted* b = pool.allocate();
    new (a) PaddedCounted(10);
    new (b) PaddedCounted(20);
    EXPECT_EQ(Counted::live_count, 2);
    EXPECT_EQ(a->c.value, 10);
    EXPECT_EQ(b->c.value, 20);

    a->~PaddedCounted();
    pool.deallocate(a);
    EXPECT_EQ(Counted::live_count, 1);

    b->~PaddedCounted();
    pool.deallocate(b);
    EXPECT_EQ(Counted::live_count, 0);
}

TEST(PoolTest, CapacityReturnsCorrectValue) {
    Pool<Block64, 16> pool;
    EXPECT_EQ(pool.capacity(), 16u);
}
