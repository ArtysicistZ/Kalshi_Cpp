#include <benchmark/benchmark.h>
#include <algorithm>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>
#include "core/flat_hash_map.h"

using kalshi::FlatHashMap;

// Production-sized capacity. 2^19 = 524288 slots, ~38% load at 200K orders.
constexpr size_t CAPACITY = 524288;

// Smaller capacity for fill-and-drain so we can recreate the map per iteration
// without amortizing 8MB mmap+memset over too few inserts.
constexpr size_t SMALL_CAP = 8192;
constexpr size_t FILL_N    = 4096;     // ~50% load on SMALL_CAP

// Workload size used by find/churn benchmarks (live set in the table)
constexpr size_t LIVE = 100000;

// ── Insert (fill from empty) ──

static void BM_FlatMap_FillDrain(benchmark::State& state) {
    for (auto _ : state) {
        FlatHashMap<uint64_t, uint64_t, SMALL_CAP> map;
        for (uint64_t i = 0; i < FILL_N; ++i) {
            map.insert(i, i);
        }
        benchmark::DoNotOptimize(map);
    }
    state.SetItemsProcessed(state.iterations() * FILL_N);
}
BENCHMARK(BM_FlatMap_FillDrain)->Unit(benchmark::kMicrosecond);

static void BM_UnorderedMap_FillDrain(benchmark::State& state) {
    for (auto _ : state) {
        std::unordered_map<uint64_t, uint64_t> map;
        map.reserve(FILL_N);
        for (uint64_t i = 0; i < FILL_N; ++i) {
            map.insert({i, i});
        }
        benchmark::DoNotOptimize(map);
    }
    state.SetItemsProcessed(state.iterations() * FILL_N);
}
BENCHMARK(BM_UnorderedMap_FillDrain)->Unit(benchmark::kMicrosecond);

// ── Find — hit ──
// Populate once outside the timed loop, then measure pure lookup throughput.

static void BM_FlatMap_FindHit(benchmark::State& state) {
    FlatHashMap<uint64_t, uint64_t, CAPACITY> map;
    for (uint64_t i = 0; i < LIVE; ++i) map.insert(i, i * 7);

    uint64_t k = 0;
    for (auto _ : state) {
        uint64_t* v = map.find(k);
        benchmark::DoNotOptimize(v);
        k = (k + 1) % LIVE;
    }
}
BENCHMARK(BM_FlatMap_FindHit);

static void BM_UnorderedMap_FindHit(benchmark::State& state) {
    std::unordered_map<uint64_t, uint64_t> map;
    map.reserve(LIVE);
    for (uint64_t i = 0; i < LIVE; ++i) map.insert({i, i * 7});

    uint64_t k = 0;
    for (auto _ : state) {
        auto it = map.find(k);
        benchmark::DoNotOptimize(it);
        k = (k + 1) % LIVE;
    }
}
BENCHMARK(BM_UnorderedMap_FindHit);

// ── Find — miss ──
// Lookup keys that aren't in the table. Stresses the EMPTY-stop branch.

static void BM_FlatMap_FindMiss(benchmark::State& state) {
    FlatHashMap<uint64_t, uint64_t, CAPACITY> map;
    for (uint64_t i = 0; i < LIVE; ++i) map.insert(i, i * 7);

    uint64_t k = LIVE;       // keys never inserted
    for (auto _ : state) {
        uint64_t* v = map.find(k);
        benchmark::DoNotOptimize(v);
        ++k;
    }
}
BENCHMARK(BM_FlatMap_FindMiss);

static void BM_UnorderedMap_FindMiss(benchmark::State& state) {
    std::unordered_map<uint64_t, uint64_t> map;
    map.reserve(LIVE);
    for (uint64_t i = 0; i < LIVE; ++i) map.insert({i, i * 7});

    uint64_t k = LIVE;
    for (auto _ : state) {
        auto it = map.find(k);
        benchmark::DoNotOptimize(it);
        ++k;
    }
}
BENCHMARK(BM_UnorderedMap_FindMiss);

// ── Find hit — random access ──
// Sequential lookups let the L2 prefetcher do most of the work, which masks
// real lookup latency and disproportionately favors maps with small working
// sets. Random access defeats the prefetcher, exposing actual per-find cost.

static void BM_FlatMap_FindHit_Random(benchmark::State& state) {
    FlatHashMap<uint64_t, uint64_t, CAPACITY> map;
    for (uint64_t i = 0; i < LIVE; ++i) map.insert(i, i * 7);

    std::vector<uint64_t> keys(LIVE);
    for (uint64_t i = 0; i < LIVE; ++i) keys[i] = i;
    std::shuffle(keys.begin(), keys.end(), std::mt19937_64{0xC0FFEEULL});

    size_t idx = 0;
    for (auto _ : state) {
        uint64_t* v = map.find(keys[idx]);
        benchmark::DoNotOptimize(v);
        idx = (idx + 1) % LIVE;
    }
}
BENCHMARK(BM_FlatMap_FindHit_Random);

static void BM_UnorderedMap_FindHit_Random(benchmark::State& state) {
    std::unordered_map<uint64_t, uint64_t> map;
    map.reserve(LIVE);
    for (uint64_t i = 0; i < LIVE; ++i) map.insert({i, i * 7});

    std::vector<uint64_t> keys(LIVE);
    for (uint64_t i = 0; i < LIVE; ++i) keys[i] = i;
    std::shuffle(keys.begin(), keys.end(), std::mt19937_64{0xC0FFEEULL});

    size_t idx = 0;
    for (auto _ : state) {
        auto it = map.find(keys[idx]);
        benchmark::DoNotOptimize(it);
        idx = (idx + 1) % LIVE;
    }
}
BENCHMARK(BM_UnorderedMap_FindHit_Random);

// ── Find miss — random access ──
// Same as above but with keys guaranteed absent from the map.

static void BM_FlatMap_FindMiss_Random(benchmark::State& state) {
    FlatHashMap<uint64_t, uint64_t, CAPACITY> map;
    for (uint64_t i = 0; i < LIVE; ++i) map.insert(i, i * 7);

    std::vector<uint64_t> miss_keys(LIVE);
    for (uint64_t i = 0; i < LIVE; ++i) miss_keys[i] = LIVE + i;     // absent
    std::shuffle(miss_keys.begin(), miss_keys.end(), std::mt19937_64{0xC0FFEEULL});

    size_t idx = 0;
    for (auto _ : state) {
        uint64_t* v = map.find(miss_keys[idx]);
        benchmark::DoNotOptimize(v);
        idx = (idx + 1) % LIVE;
    }
}
BENCHMARK(BM_FlatMap_FindMiss_Random);

static void BM_UnorderedMap_FindMiss_Random(benchmark::State& state) {
    std::unordered_map<uint64_t, uint64_t> map;
    map.reserve(LIVE);
    for (uint64_t i = 0; i < LIVE; ++i) map.insert({i, i * 7});

    std::vector<uint64_t> miss_keys(LIVE);
    for (uint64_t i = 0; i < LIVE; ++i) miss_keys[i] = LIVE + i;
    std::shuffle(miss_keys.begin(), miss_keys.end(), std::mt19937_64{0xC0FFEEULL});

    size_t idx = 0;
    for (auto _ : state) {
        auto it = map.find(miss_keys[idx]);
        benchmark::DoNotOptimize(it);
        idx = (idx + 1) % LIVE;
    }
}
BENCHMARK(BM_UnorderedMap_FindMiss_Random);

// ── Churning (the realistic order-book pattern) ──
// Steady-state: LIVE entries always resident. Each iteration erases the
// oldest and inserts a new one. Mirrors order placement/cancellation.

static void BM_FlatMap_Churning(benchmark::State& state) {
    constexpr size_t L = 1024;
    FlatHashMap<uint64_t, uint64_t, 4096> map;
    for (uint64_t i = 0; i < L; ++i) map.insert(i, i);

    uint64_t old_id = 0;
    uint64_t new_id = L;
    for (auto _ : state) {
        map.erase(old_id);
        map.insert(new_id, new_id);
        ++old_id;
        ++new_id;
    }
}
BENCHMARK(BM_FlatMap_Churning);

static void BM_UnorderedMap_Churning(benchmark::State& state) {
    constexpr size_t L = 1024;
    std::unordered_map<uint64_t, uint64_t> map;
    map.reserve(L * 2);
    for (uint64_t i = 0; i < L; ++i) map.insert({i, i});

    uint64_t old_id = 0;
    uint64_t new_id = L;
    for (auto _ : state) {
        map.erase(old_id);
        map.insert({new_id, new_id});
        ++old_id;
        ++new_id;
    }
}
BENCHMARK(BM_UnorderedMap_Churning);

// ── Realistic mixed workload ──
// 95% find, 5% paired erase+insert. Sliding window of LIVE entries keeps
// the map size constant — no runaway growth, no load-factor blowup.

static void BM_FlatMap_MixedWorkload(benchmark::State& state) {
    FlatHashMap<uint64_t, uint64_t, CAPACITY> map;
    for (uint64_t i = 0; i < LIVE; ++i) map.insert(i, i);

    uint64_t k = 0;
    uint64_t oldest = 0;
    uint64_t next_id = LIVE;
    for (auto _ : state) {
        if ((k % 20) < 19) {
            uint64_t* v = map.find(oldest + (k % LIVE));
            benchmark::DoNotOptimize(v);
        } else {
            map.erase(oldest);
            map.insert(next_id, next_id);
            ++oldest;
            ++next_id;
        }
        ++k;
    }
}
BENCHMARK(BM_FlatMap_MixedWorkload);

static void BM_UnorderedMap_MixedWorkload(benchmark::State& state) {
    std::unordered_map<uint64_t, uint64_t> map;
    map.reserve(LIVE * 2);
    for (uint64_t i = 0; i < LIVE; ++i) map.insert({i, i});

    uint64_t k = 0;
    uint64_t oldest = 0;
    uint64_t next_id = LIVE;
    for (auto _ : state) {
        if ((k % 20) < 19) {
            auto it = map.find(oldest + (k % LIVE));
            benchmark::DoNotOptimize(it);
        } else {
            map.erase(oldest);
            map.insert({next_id, next_id});
            ++oldest;
            ++next_id;
        }
        ++k;
    }
}
BENCHMARK(BM_UnorderedMap_MixedWorkload);

// ── Erase-heavy (worst case for backshift) ──
// Pre-fill, then erase every key. Measures pure erase cost.

static void BM_FlatMap_DrainAll(benchmark::State& state) {
    constexpr size_t N = 4096;
    for (auto _ : state) {
        state.PauseTiming();
        FlatHashMap<uint64_t, uint64_t, SMALL_CAP> map;
        for (uint64_t i = 0; i < N; ++i) map.insert(i, i);
        state.ResumeTiming();

        for (uint64_t i = 0; i < N; ++i) map.erase(i);
    }
    state.SetItemsProcessed(state.iterations() * N);
}
BENCHMARK(BM_FlatMap_DrainAll)->Unit(benchmark::kMicrosecond);

static void BM_UnorderedMap_DrainAll(benchmark::State& state) {
    constexpr size_t N = 4096;
    for (auto _ : state) {
        state.PauseTiming();
        std::unordered_map<uint64_t, uint64_t> map;
        map.reserve(N);
        for (uint64_t i = 0; i < N; ++i) map.insert({i, i});
        state.ResumeTiming();

        for (uint64_t i = 0; i < N; ++i) map.erase(i);
    }
    state.SetItemsProcessed(state.iterations() * N);
}
BENCHMARK(BM_UnorderedMap_DrainAll)->Unit(benchmark::kMicrosecond);
