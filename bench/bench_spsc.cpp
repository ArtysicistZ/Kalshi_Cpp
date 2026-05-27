#include <benchmark/benchmark.h>
#include <thread>
#include <atomic>
#include "core/spsc_queue.h"

using kalshi::SPSCQueue;

// Single-thread push+pop round trip
static void BM_PushPop(benchmark::State& state) {
    SPSCQueue<int, 1024> q;
    int out = 0;
    for (auto _ : state) {
        q.try_push(42);
        q.try_pop(out);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_PushPop);

// Push-only (measures try_push in isolation, drain periodically to avoid full)
static void BM_Push(benchmark::State& state) {
    SPSCQueue<int, 1024> q;
    int i = 0;
    for (auto _ : state) {
        if (!q.try_push(i++)) {
            // drain the queue
            int out;
            while (q.try_pop(out)) {}
            q.try_push(i++);
        }
    }
}
BENCHMARK(BM_Push);

// Pop-only (pre-fill, measure try_pop, refill when empty)
static void BM_Pop(benchmark::State& state) {
    SPSCQueue<int, 1024> q;
    for (int i = 0; i < 1024; ++i)
        q.try_push(i);

    int out = 0;
    for (auto _ : state) {
        if (!q.try_pop(out)) {
            // refill the queue
            for (int i = 0; i < 1024; ++i)
                q.try_push(i);
            q.try_pop(out);
        }
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_Pop);

// Emplace (construct a struct directly in the slot)
struct MarketTick {
    int price;
    int qty;
    char side;
    MarketTick() : price(0), qty(0), side('B') {}
    MarketTick(int p, int q, char s) : price(p), qty(q), side(s) {}
};

static void BM_Emplace(benchmark::State& state) {
    SPSCQueue<MarketTick, 1024> q;
    MarketTick out;
    for (auto _ : state) {
        q.try_emplace(42, 100, 'B');
        q.try_pop(out);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_Emplace);

// Larger payload — simulates real MarketData struct
struct FatMessage {
    char ticker[32];
    double prices[8];
    int64_t timestamp;
    int qty;
    // ~108 bytes total
};

static void BM_PushPopFat(benchmark::State& state) {
    SPSCQueue<FatMessage, 1024> q;
    FatMessage msg{};
    msg.qty = 100;
    msg.timestamp = 1234567890;
    FatMessage out{};
    for (auto _ : state) {
        q.try_push(msg);
        q.try_pop(out);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_PushPopFat);

// ── Cross-thread benchmarks ──

// Throughput: producer and consumer on separate threads
// Measures how many items/sec can flow through the queue
static void BM_CrossThread_Throughput(benchmark::State& state) {
    const int64_t items_per_iter = 100000;
    SPSCQueue<int64_t, 1024> q;

    for (auto _ : state) {
        std::atomic<bool> done{false};

        // Consumer thread
        std::thread consumer([&] {
            int64_t out = 0;
            int64_t count = 0;
            while (count < items_per_iter) {
                if (q.try_pop(out))
                    ++count;
            }
            benchmark::DoNotOptimize(out);
        });

        // Producer (this thread)
        for (int64_t i = 0; i < items_per_iter; ++i) {
            while (!q.try_push(i)) {}  // spin until room
        }

        consumer.join();
    }
    state.SetItemsProcessed(state.iterations() * items_per_iter);
}
BENCHMARK(BM_CrossThread_Throughput)->Unit(benchmark::kMicrosecond);

// RTT ping-pong: measures round-trip latency between two threads
// Thread A pushes to q1, Thread B reads from q1 and pushes to q2, Thread A reads from q2
// One round trip = one cache-line bounce in each direction
static void BM_CrossThread_RTT(benchmark::State& state) {
    SPSCQueue<int64_t, 1024> q1;  // A → B
    SPSCQueue<int64_t, 1024> q2;  // B → A
    const int64_t rounds = 10000;

    for (auto _ : state) {
        // Thread B: read from q1, echo back on q2
        std::thread echo([&] {
            int64_t val = 0;
            for (int64_t i = 0; i < rounds; ++i) {
                while (!q1.try_pop(val)) {}   // wait for A's message
                while (!q2.try_push(val)) {}  // send it back
            }
        });

        // Thread A (this thread): push to q1, read response from q2
        int64_t val = 0;
        for (int64_t i = 0; i < rounds; ++i) {
            while (!q1.try_push(i)) {}    // send to B
            while (!q2.try_pop(val)) {}   // wait for B's reply
        }
        benchmark::DoNotOptimize(val);

        echo.join();
    }
    state.SetItemsProcessed(state.iterations() * rounds);
}
BENCHMARK(BM_CrossThread_RTT)->Unit(benchmark::kNanosecond);
