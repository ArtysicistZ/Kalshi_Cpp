#include <benchmark/benchmark.h>
#include <cstdint>
#include <cstdlib>
#include <list>
#include <vector>
#include "core/pool_alloc.h"

using kalshi::Pool;

// Simulated Order struct — sized to one cache line
struct Order {
    uint64_t id;
    uint64_t timestamp;
    int32_t price;
    int32_t qty;
    int32_t side;
    int32_t status;
    char ticker[32];
    // 64 bytes total (one cache line)
};

static_assert(sizeof(Order) == 64, "Order should be exactly one cache line");

// ── Single-shot alloc + dealloc cycle ──

static void BM_Pool_AllocDealloc(benchmark::State& state) {
    Pool<Order, 524288> pool;
    for (auto _ : state) {
        Order* o = pool.allocate();
        benchmark::DoNotOptimize(o);
        pool.deallocate(o);
    }
}
BENCHMARK(BM_Pool_AllocDealloc);

static void BM_Malloc_AllocFree(benchmark::State& state) {
    for (auto _ : state) {
        Order* o = static_cast<Order*>(std::malloc(sizeof(Order)));
        benchmark::DoNotOptimize(o);
        std::free(o);
    }
}
BENCHMARK(BM_Malloc_AllocFree);

static void BM_New_Delete(benchmark::State& state) {
    for (auto _ : state) {
        Order* o = new Order();
        benchmark::DoNotOptimize(o);
        delete o;
    }
}
BENCHMARK(BM_New_Delete);

// ── Sustained throughput: fill, then drain ──

static void BM_Pool_FillDrain(benchmark::State& state) {
    constexpr size_t N = 4096;
    Pool<Order, N> pool;
    std::vector<Order*> ptrs(N);

    for (auto _ : state) {
        for (size_t i = 0; i < N; ++i) ptrs[i] = pool.allocate();
        for (size_t i = 0; i < N; ++i) pool.deallocate(ptrs[i]);
    }
    state.SetItemsProcessed(state.iterations() * N * 2);  // 2 ops per item
}
BENCHMARK(BM_Pool_FillDrain)->Unit(benchmark::kMicrosecond);

static void BM_Malloc_FillDrain(benchmark::State& state) {
    constexpr size_t N = 4096;
    std::vector<Order*> ptrs(N);

    for (auto _ : state) {
        for (size_t i = 0; i < N; ++i)
            ptrs[i] = static_cast<Order*>(std::malloc(sizeof(Order)));
        for (size_t i = 0; i < N; ++i) std::free(ptrs[i]);
    }
    state.SetItemsProcessed(state.iterations() * N * 2);
}
BENCHMARK(BM_Malloc_FillDrain)->Unit(benchmark::kMicrosecond);

// ── Realistic pattern: allocate, do small work, deallocate ──
// (simulates an order being created, briefly used, then completed)

static void BM_Pool_RealisticUsage(benchmark::State& state) {
    Pool<Order, 524288> pool;
    uint64_t next_id = 0;
    for (auto _ : state) {
        Order* o = pool.allocate();
        o->id = ++next_id;
        o->timestamp = next_id * 1000;
        o->price = 4200;
        o->qty = 100;
        o->side = 1;
        o->status = 0;
        benchmark::DoNotOptimize(o->id);
        pool.deallocate(o);
    }
}
BENCHMARK(BM_Pool_RealisticUsage);

static void BM_Malloc_RealisticUsage(benchmark::State& state) {
    uint64_t next_id = 0;
    for (auto _ : state) {
        Order* o = static_cast<Order*>(std::malloc(sizeof(Order)));
        o->id = ++next_id;
        o->timestamp = next_id * 1000;
        o->price = 4200;
        o->qty = 100;
        o->side = 1;
        o->status = 0;
        benchmark::DoNotOptimize(o->id);
        std::free(o);
    }
}
BENCHMARK(BM_Malloc_RealisticUsage);

// ── Interleaved alloc/dealloc (simulates real order lifecycle) ──
// Keep ~1000 orders open at any time, churning constantly

static void BM_Pool_Churning(benchmark::State& state) {
    constexpr size_t LIVE = 1024;
    Pool<Order, 4096> pool;
    std::vector<Order*> live(LIVE);

    // Prime the pool with LIVE orders
    for (size_t i = 0; i < LIVE; ++i) live[i] = pool.allocate();

    size_t idx = 0;
    for (auto _ : state) {
        // Free one, allocate one (steady state of LIVE orders)
        pool.deallocate(live[idx]);
        live[idx] = pool.allocate();
        idx = (idx + 1) & (LIVE - 1);
    }

    for (size_t i = 0; i < LIVE; ++i) pool.deallocate(live[i]);
}
BENCHMARK(BM_Pool_Churning);

static void BM_Malloc_Churning(benchmark::State& state) {
    constexpr size_t LIVE = 1024;
    std::vector<Order*> live(LIVE);

    for (size_t i = 0; i < LIVE; ++i)
        live[i] = static_cast<Order*>(std::malloc(sizeof(Order)));

    size_t idx = 0;
    for (auto _ : state) {
        std::free(live[idx]);
        live[idx] = static_cast<Order*>(std::malloc(sizeof(Order)));
        idx = (idx + 1) & (LIVE - 1);
    }

    for (size_t i = 0; i < LIVE; ++i) std::free(live[i]);
}
BENCHMARK(BM_Malloc_Churning);

// ── std::list comparison ──
// std::list allocates a node per element via malloc internally.
// This is the closest STL container to what our pool replaces.

static void BM_StdList_PushPop(benchmark::State& state) {
    std::list<Order> list;
    Order o{};
    for (auto _ : state) {
        list.push_back(o);
        list.pop_back();
    }
}
BENCHMARK(BM_StdList_PushPop);

static void BM_StdList_Churning(benchmark::State& state) {
    constexpr size_t LIVE = 1024;
    std::list<Order> list;

    Order o{};
    for (size_t i = 0; i < LIVE; ++i) list.push_back(o);

    for (auto _ : state) {
        list.pop_front();        // free internal node
        list.push_back(o);       // allocate new internal node
    }
}
BENCHMARK(BM_StdList_Churning);
