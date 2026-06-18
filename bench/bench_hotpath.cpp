// bench_hotpath.cpp — multi-payload, zero-allocation-enforced rdtsc bench
// over the parse → apply → evaluate → reconcile → serialize hot path.

#include <simdjson.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "core/clock.h"
#include "feed/book.h"
#include "feed/parser.h"
#include "net/serialize.h"
#include "strategy/order_manager.h"
#include "strategy/signal.h"

// ── Zero-allocation enforcement ──────────────────────────────────────────
// Global new/delete overrides bump a counter when "armed". The measurement
// loop arms the counter just before timing and disarms it just after, so
// pool/object construction is excluded. Anything allocating inside the
// measurement window — simdjson internals, accidental std::vector growth,
// stray std::string — will show up here.
std::atomic<uint64_t> g_alloc_count{0};
std::atomic<bool>     g_alloc_armed{false};

void* operator new(std::size_t n) {
    if (g_alloc_armed.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(n);
    if (!p) std::abort();
    return p;
}
void* operator new[](std::size_t n) {
    if (g_alloc_armed.load(std::memory_order_relaxed)) {
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(n);
    if (!p) std::abort();
    return p;
}
void operator delete(void* p) noexcept              { std::free(p); }
void operator delete[](void* p) noexcept            { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

// ── Optimizer barrier ────────────────────────────────────────────────────
// Empty inline asm with a "memory" clobber. Forces the compiler to treat
// `v` as if it escaped to unknown code, preventing dead-store elimination.
template <typename T>
__attribute__((always_inline)) inline void escape(T const& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

// ── Config ───────────────────────────────────────────────────────────────
constexpr int N_WARMUP   = 5'000;
constexpr int N_MEASURE  = 100'000;
constexpr int N_PAYLOADS = 1024;             // power of two — `i & (N-1)`

constexpr const char* kTicker = "DEMO-1";

// ── Deterministic xorshift64 (stack-only, no allocation) ─────────────────
struct Xorshift64 {
    uint64_t s;
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    uint32_t range(uint32_t lo, uint32_t hi) {
        return lo + static_cast<uint32_t>(next() % (hi - lo + 1));
    }
};

// ── Synthetic payload pool ───────────────────────────────────────────────
// Builds N_PAYLOADS distinct orderbook-delta JSONs varying price, side, and
// delta. Pool construction allocates (padded_string owns its buffer) — done
// BEFORE the alloc counter is armed.
static std::vector<simdjson::padded_string> build_payload_pool() {
    std::vector<simdjson::padded_string> pool;
    pool.reserve(N_PAYLOADS);
    Xorshift64 rng{0xDEADBEEFCAFEBABEULL};
    char buf[256];
    for (int i = 0; i < N_PAYLOADS; ++i) {
        uint32_t    price = rng.range(1, 99);
        int32_t     delta = static_cast<int32_t>(rng.range(1, 50)) *
                            ((rng.next() & 1) ? 1 : -1);
        const char* side  = (rng.next() & 1) ? "yes" : "no";
        uint64_t    ts    = 1'700'000'000'000ULL + (rng.next() % 1'000'000ULL);
        uint64_t    seq   = static_cast<uint64_t>(i + 1);

        int n = std::snprintf(
            buf, sizeof(buf),
            R"({"type":"orderbook_delta","sid":1,"seq":%llu,)"
            R"("msg":{"market_ticker":"DEMO-1","price":%u,)"
            R"("delta":%d,"side":"%s","ts":%llu}})",
            (unsigned long long)seq, price, delta, side,
            (unsigned long long)ts);
        pool.emplace_back(buf, static_cast<size_t>(n));
    }
    return pool;
}

// ── Percentile from a pre-sorted vector ──────────────────────────────────
static uint64_t percentile(const std::vector<uint64_t>& sorted, double pct) {
    if (sorted.empty()) return 0;
    size_t idx = static_cast<size_t>(pct * (sorted.size() - 1));
    return sorted[idx];
}

// ── Log2-bucket histogram ────────────────────────────────────────────────
// Bucket k covers cycles in [2^(7+k), 2^(8+k)). Prints non-empty buckets
// only, with a 50-wide proportional bar.
static void print_histogram(const std::vector<uint64_t>& cycles,
                            const kalshi::CycleClock&    clock) {
    constexpr int N_BUCKETS = 24;
    uint64_t buckets[N_BUCKETS] = {};
    for (uint64_t c : cycles) {
        int      b = 0;
        uint64_t v = c >> 7;
        while (v >>= 1) ++b;
        if (b >= N_BUCKETS) b = N_BUCKETS - 1;
        buckets[b]++;
    }
    std::printf("\nlatency histogram (log2 cycle buckets):\n");
    std::printf("  cycles               ns                 count   bar\n");
    for (int i = 0; i < N_BUCKETS; ++i) {
        if (buckets[i] == 0) continue;
        uint64_t lo = 128ULL << i;
        uint64_t hi = lo << 1;
        int      bar = static_cast<int>(buckets[i] * 50 / cycles.size());
        char     bar_buf[64] = {};
        for (int j = 0; j < bar && j < 50; ++j) bar_buf[j] = '#';
        std::printf("  %7llu..%-7llu   %7.0f..%-7.0f   %7llu   %s\n",
                    (unsigned long long)lo, (unsigned long long)hi,
                    clock.to_ns(lo), clock.to_ns(hi),
                    (unsigned long long)buckets[i], bar_buf);
    }
}

int main() {
    // 1) Calibrate cycles → ns (50 ms sleep inside the ctor).
    kalshi::CycleClock clock;

    // 2) Build payload pool (allocates, before arming).
    auto pool = build_payload_pool();

    // 3) Construct pipeline objects (may touch heap inside simdjson — fine,
    //    counter is still disarmed).
    kalshi::OrderbookParser parser;
    kalshi::Book            book;
    kalshi::Signal          signal;
    kalshi::OrderManager    om;
    char                    outbuf[256];

    std::vector<uint64_t> cycles(N_MEASURE);  // pre-sized, no reallocation.

    // 4) Warmup. Walks the same payload pool to populate L1i, branch
    //    predictors, TLB, and any simdjson-internal capacity that grows
    //    on first parse.
    for (int i = 0; i < N_WARMUP; ++i) {
        const auto& payload = pool[i & (N_PAYLOADS - 1)];
        kalshi::OrderbookDelta delta;
        parser.parse_orderbook_delta(payload, delta);
        escape(delta);

        book.apply(delta);
        escape(book);

        kalshi::TargetQuote tq = signal.evaluate(book);
        escape(tq);

        kalshi::ActionBundle bundle = om.reconcile(tq);
        escape(bundle);

        for (uint8_t k = 0; k < bundle.cnt; ++k) {
            size_t n = kalshi::serialize_action(
                bundle.actions[k], kTicker, outbuf, sizeof(outbuf));
            escape(outbuf);
            escape(n);
        }
    }

    // 5) ARM the zero-alloc counter.
    g_alloc_armed.store(true, std::memory_order_release);

    // 6) Measurement loop.
    for (int i = 0; i < N_MEASURE; ++i) {
        const auto& payload = pool[i & (N_PAYLOADS - 1)];

        uint64_t t0 = kalshi::rdtsc();

        kalshi::OrderbookDelta delta;
        parser.parse_orderbook_delta(payload, delta);
        escape(delta);

        book.apply(delta);
        escape(book);

        kalshi::TargetQuote tq = signal.evaluate(book);
        escape(tq);

        kalshi::ActionBundle bundle = om.reconcile(tq);
        escape(bundle);

        for (uint8_t k = 0; k < bundle.cnt; ++k) {
            size_t n = kalshi::serialize_action(
                bundle.actions[k], kTicker, outbuf, sizeof(outbuf));
            escape(outbuf);
            escape(n);
        }

        uint64_t t1 = kalshi::rdtscp();
        cycles[i]   = t1 - t0;
    }

    // 7) DISARM and snapshot the alloc counter.
    g_alloc_armed.store(false, std::memory_order_release);
    uint64_t allocs = g_alloc_count.load(std::memory_order_acquire);

    // 8) Stats.
    std::sort(cycles.begin(), cycles.end());

    uint64_t c_min  = cycles.front();
    uint64_t c_p50  = percentile(cycles, 0.50);
    uint64_t c_p90  = percentile(cycles, 0.90);
    uint64_t c_p99  = percentile(cycles, 0.99);
    uint64_t c_p999 = percentile(cycles, 0.999);
    uint64_t c_max  = cycles.back();

    auto to_ns = [&](uint64_t c) { return clock.to_ns(c); };

    std::printf("\n=== bench_hotpath (multi-payload, zero-alloc-enforced) ===\n");
    std::printf("N        = %d iterations\n", N_MEASURE);
    std::printf("payloads = %d distinct deltas (xorshift64, seed=0xDEADBEEFCAFEBABE)\n",
                N_PAYLOADS);
    std::printf("\n            cycles        ns\n");
    std::printf("   min:   %8llu   %8.1f\n", (unsigned long long)c_min,  to_ns(c_min));
    std::printf("   p50:   %8llu   %8.1f\n", (unsigned long long)c_p50,  to_ns(c_p50));
    std::printf("   p90:   %8llu   %8.1f\n", (unsigned long long)c_p90,  to_ns(c_p90));
    std::printf("   p99:   %8llu   %8.1f\n", (unsigned long long)c_p99,  to_ns(c_p99));
    std::printf("  p999:   %8llu   %8.1f\n", (unsigned long long)c_p999, to_ns(c_p999));
    std::printf("   max:   %8llu   %8.1f\n", (unsigned long long)c_max,  to_ns(c_max));

    std::printf("\nzero-alloc check: %llu allocations during measurement -- %s\n",
                (unsigned long long)allocs,
                allocs == 0 ? "PASS" : "FAIL");
    std::printf("p50 < 3000 ns claim: %s\n",
                to_ns(c_p50) < 3000.0 ? "PASS" : "FAIL");

    print_histogram(cycles, clock);
    return 0;
}
