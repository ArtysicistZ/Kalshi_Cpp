#include <gtest/gtest.h>
#include "core/spsc_queue.h"

using kalshi::SPSCQueue;

TEST(SPSCQueueTest, PushAndPop) {
    SPSCQueue<int, 4> q;
    int out = 0;
    EXPECT_TRUE(q.try_push(42));
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 42);
}

TEST(SPSCQueueTest, PopFromEmpty) {
    SPSCQueue<int, 4> q;
    int out = 0;
    EXPECT_FALSE(q.try_pop(out));
}

TEST(SPSCQueueTest, PushToFull) {
    SPSCQueue<int, 4> q;
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_TRUE(q.try_push(4));
    EXPECT_FALSE(q.try_push(5));
}

TEST(SPSCQueueTest, FIFOOrder) {
    SPSCQueue<int, 8> q;
    for (int i = 0; i < 5; ++i)
        q.try_push(i);

    for (int i = 0; i < 5; ++i) {
        int out = -1;
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out, i);
    }
}

TEST(SPSCQueueTest, WrapAround) {
    SPSCQueue<int, 4> q;
    for (int round = 0; round < 10; ++round) {
        for (int i = 0; i < 4; ++i)
            ASSERT_TRUE(q.try_push(round * 4 + i));
        for (int i = 0; i < 4; ++i) {
            int out = -1;
            ASSERT_TRUE(q.try_pop(out));
            EXPECT_EQ(out, round * 4 + i);
        }
    }
}

TEST(SPSCQueueTest, FrontAndPop) {
    SPSCQueue<int, 4> q;
    EXPECT_EQ(q.front(), nullptr);

    q.try_push(99);
    int* p = q.front();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 99);
    q.pop();

    EXPECT_EQ(q.front(), nullptr);
}

TEST(SPSCQueueTest, SizeAndEmpty) {
    SPSCQueue<int, 4> q;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);

    q.try_push(1);
    EXPECT_FALSE(q.empty());
    EXPECT_EQ(q.size(), 1u);

    q.try_push(2);
    EXPECT_EQ(q.size(), 2u);

    int out;
    q.try_pop(out);
    EXPECT_EQ(q.size(), 1u);
}

TEST(SPSCQueueTest, Emplace) {
    struct Point {
        int x, y;
        Point() : x(0), y(0) {}
        Point(int a, int b) : x(a), y(b) {}
    };

    SPSCQueue<Point, 4> q;
    EXPECT_TRUE(q.try_emplace(3, 7));

    Point out;
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out.x, 3);
    EXPECT_EQ(out.y, 7);
}

TEST(SPSCQueueTest, MoveOverload) {
    SPSCQueue<int, 4> q;
    int val = 42;
    EXPECT_TRUE(q.try_push(std::move(val)));

    int out = 0;
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 42);
}
