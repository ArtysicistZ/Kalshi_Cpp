// bench_hotpath_multi.cpp — multi-ticker variant of the two-thread SPSC
// pipeline bench. Producer parses N_TICKERS distinct symbols round-robined
// across the payload pool; consumer hashes the ticker string, looks up the
// per-ticker MarketState in a FlatHashMap, applies the delta to that
// market's Book, evaluates the Signal, reconciles, and serializes — all
// keyed off the dispatched state.
//
// Adds two primitives over bench_hotpath_pipe:
//   • FlatHashMap<TickerKey, MarketState*, MAP_CAP>  — ticker dispatch
//   • Pool<MarketState, POOL_CAP>                    — MarketState storage
//
// Same rdtsc-bracketed end-to-end latency; same global new/delete
// interposer enforcing zero allocation during the timed window.

#include <simdjson.h>

#include <sys/mman.h>      // mlockall

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

#include "core/clock.h"
#include "core/flat_hash_map.h"
#include "core/pool_alloc.h"
#include "core/spsc_queue.h"
#include "feed/book.h"
#include "feed/parser.h"
#include "net/serialize.h"
#include "strategy/order_manager.h"
#include "strategy/signal.h"
#include "system/tuning.h"

// ── Zero-allocation enforcement ──────────────────────────────────────────
std::atomic<uint64_t> g_alloc_count{0};
std::atomic<bool>     g_alloc_armed{false};

void* operator new(std::size_t n) {
    if (g_alloc_armed.load(std::memory_order_relaxed))
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n);
    if (!p) std::abort();
    return p;
}
void* operator new[](std::size_t n) {
    if (g_alloc_armed.load(std::memory_order_relaxed))
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n);
    if (!p) std::abort();
    return p;
}
void operator delete(void* p) noexcept                { std::free(p); }
void operator delete[](void* p) noexcept              { std::free(p); }
void operator delete(void* p, std::size_t) noexcept   { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

// ── Optimizer barrier ────────────────────────────────────────────────────
template <typename T>
__attribute__((always_inline)) inline void escape(T const& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

// ── Config ───────────────────────────────────────────────────────────────
constexpr int      N_WARMUP        = 10'000;
constexpr int      N_MEASURE       = 1'000'000;
constexpr int      N_TOTAL         = N_WARMUP + N_MEASURE;
constexpr int      N_PAYLOADS      = 1024;
constexpr int      N_TICKERS       = 256;
constexpr size_t   QUEUE_CAP       = 1024;
constexpr size_t   MAP_CAP         = 1024;     // ~25% load factor at N_TICKERS = 256
constexpr size_t   POOL_CAP        = 256;
constexpr unsigned CORE_PRODUCER   = 16;
constexpr unsigned CORE_CONSUMER   = 18;
constexpr int      RT_PRIORITY     = 50;

// ── Per-ticker state ─────────────────────────────────────────────────────
// One instance per active ticker. Backed by Pool<MarketState>; pointer
// installed in the dispatch FlatHashMap keyed on the ticker-string hash.
struct MarketState {
    kalshi::Book         book;
    kalshi::Signal       signal;
    kalshi::OrderManager om;
};
static_assert(sizeof(MarketState) >= sizeof(void*),
              "Pool free-list requires sizeof(T) >= sizeof(void*)");

using TickerKey = uint64_t;

// ── FNV-1a 64-bit hash for ticker strings ────────────────────────────────
// FlatHashMap uses (TickerKey)-1 as the empty-slot sentinel, so we mask any
// pathological hash that lands on the sentinel.
static TickerKey hash_ticker(const char* s, size_t len) noexcept {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint8_t>(s[i]);
        h *= 0x100000001b3ULL;
    }
    if (h == ~TickerKey{0}) h ^= 1;
    return h;
}

// Bounded strlen over the fixed-size ticker buffer.
static size_t ticker_strnlen(const char* s, size_t max_len) noexcept {
    for (size_t i = 0; i < max_len; ++i) if (s[i] == 0) return i;
    return max_len;
}

// ── Message crossing the queue ───────────────────────────────────────────
struct HotMsg {
    kalshi::OrderbookDelta delta;        // 64 B — includes ticker[32]
    uint64_t               produce_tsc;  //  8 B
};
static_assert(sizeof(HotMsg) == 72);

// ── Xorshift64 ───────────────────────────────────────────────────────────
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
// Each payload picks a uniformly random ticker out of N_TICKERS so the
// dispatch FlatHashMap sees realistic key churn, not the same key in a
// row. Built before the alloc counter is armed.
static std::vector<simdjson::padded_string> build_payload_pool() {
    std::vector<simdjson::padded_string> pool;
    pool.reserve(N_PAYLOADS);
    Xorshift64 rng{0xDEADBEEFCAFEBABEULL};
    char buf[256];
    for (int i = 0; i < N_PAYLOADS; ++i) {
        uint32_t    tid   = rng.range(0, N_TICKERS - 1);
        uint32_t    price = rng.range(1, 99);
        int32_t     delta = static_cast<int32_t>(rng.range(1, 50)) *
                            ((rng.next() & 1) ? 1 : -1);
        const char* side  = (rng.next() & 1) ? "yes" : "no";
        uint64_t    ts    = 1'700'000'000'000ULL + (rng.next() % 1'000'000ULL);
        uint64_t    seq   = static_cast<uint64_t>(i + 1);
        int n = std::snprintf(
            buf, sizeof(buf),
            R"({"type":"orderbook_delta","sid":1,"seq":%llu,)"
            R"("msg":{"market_ticker":"MKT-%03u","price":%u,)"
            R"("delta":%d,"side":"%s","ts":%llu}})",
            (unsigned long long)seq, tid, price, delta, side,
            (unsigned long long)ts);
        pool.emplace_back(buf, static_cast<size_t>(n));
    }
    return pool;
}

// ── Stats helpers ────────────────────────────────────────────────────────
static uint64_t percentile(const std::vector<uint64_t>& sorted, double pct) {
    if (sorted.empty()) return 0;
    size_t idx = static_cast<size_t>(pct * (sorted.size() - 1));
    return sorted[idx];
}

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

// ── Shared state ─────────────────────────────────────────────────────────
// Pool and FlatHashMap mmap their backing storage at construction time
// (before main() runs and before the alloc counter is armed). Neither
// performs operator new on its own, so they don't trip the interposer.
kalshi::SPSCQueue<HotMsg, QUEUE_CAP>                    g_queue;
std::vector<simdjson::padded_string>                    g_pool;
std::vector<uint64_t>                                   g_latencies;
kalshi::Pool<MarketState, POOL_CAP>                     g_market_pool;
kalshi::FlatHashMap<TickerKey, MarketState*, MAP_CAP>   g_markets;

std::atomic<int>      g_threads_ready{0};
std::atomic<uint64_t> g_throughput_start_tsc{0};
std::atomic<uint64_t> g_throughput_end_tsc{0};

// ── Producer thread ──────────────────────────────────────────────────────
static void producer_thread() {
    if (!kalshi::pin_this_thread_to_core(CORE_PRODUCER))
        std::fprintf(stderr, "warn: failed to pin producer to core %u\n",
                     CORE_PRODUCER);
    if (!kalshi::set_realtime_priority(RT_PRIORITY))
        std::fprintf(stderr, "warn: producer SCHED_FIFO@%d failed (errno=%d) — "
                             "run with sudo for tighter tail\n",
                     RT_PRIORITY, errno);

    kalshi::OrderbookParser parser;
    HotMsg                  msg;

    for (int i = 0; i < N_TOTAL; ++i) {
        if (i == N_WARMUP) {
            g_threads_ready.fetch_add(1, std::memory_order_release);
            while (!g_alloc_armed.load(std::memory_order_acquire)) {
                // spin
            }
        }

        const auto& payload = g_pool[i & (N_PAYLOADS - 1)];

        uint64_t t0 = kalshi::rdtsc();
        parser.parse_orderbook_delta(payload, msg.delta);
        msg.produce_tsc = t0;

        while (!g_queue.try_push(msg)) {
            // spin under backpressure
        }
    }
}

// ── Consumer thread ──────────────────────────────────────────────────────
// Per message: hash ticker → FlatHashMap.find → MarketState* → book.apply
// → signal → reconcile → serialize. Latency includes the dispatch step.
static void consumer_thread() {
    if (!kalshi::pin_this_thread_to_core(CORE_CONSUMER))
        std::fprintf(stderr, "warn: failed to pin consumer to core %u\n",
                     CORE_CONSUMER);
    if (!kalshi::set_realtime_priority(RT_PRIORITY))
        std::fprintf(stderr, "warn: consumer SCHED_FIFO@%d failed (errno=%d) — "
                             "run with sudo for tighter tail\n",
                     RT_PRIORITY, errno);

    char   outbuf[256];
    HotMsg msg;

    for (int i = 0; i < N_TOTAL; ++i) {
        if (i == N_WARMUP) {
            g_threads_ready.fetch_add(1, std::memory_order_release);
            while (!g_alloc_armed.load(std::memory_order_acquire)) {
                // spin
            }
            g_throughput_start_tsc.store(kalshi::rdtsc(),
                                         std::memory_order_relaxed);
        }

        while (!g_queue.try_pop(msg)) {
            // spin
        }

        // Ticker dispatch: every market is pre-registered, so a miss would
        // indicate a parser/hash bug — abort rather than skip silently
        // (which would falsely tighten the latency distribution).
        size_t        tlen = ticker_strnlen(msg.delta.ticker,
                                            sizeof(msg.delta.ticker));
        TickerKey     key  = hash_ticker(msg.delta.ticker, tlen);
        MarketState** slot = g_markets.find(key);
        if (!slot) std::abort();
        MarketState*  m    = *slot;
        escape(m);

        m->book.apply(msg.delta);
        escape(m->book);

        kalshi::TargetQuote tq = m->signal.evaluate(m->book);
        escape(tq);

        kalshi::ActionBundle bundle = m->om.reconcile(tq);
        escape(bundle);

        for (uint8_t k = 0; k < bundle.cnt; ++k) {
            size_t n = kalshi::serialize_action(
                bundle.actions[k], msg.delta.ticker, outbuf, sizeof(outbuf));
            escape(outbuf);
            escape(n);
        }

        uint64_t end_tsc = kalshi::rdtscp();
        if (i >= N_WARMUP) {
            g_latencies[i - N_WARMUP] = end_tsc - msg.produce_tsc;
        }
    }

    g_throughput_end_tsc.store(kalshi::rdtscp(),
                               std::memory_order_relaxed);
}

// ── Pre-populate the per-ticker MarketState table ───────────────────────
// One MarketState per ticker, allocated from the Pool, default-constructed
// in place, pointer registered in the FlatHashMap under its FNV-1a hash.
// Runs single-threaded at startup, well before the alloc counter is armed.
static void register_markets() {
    char name[32];
    for (int i = 0; i < N_TICKERS; ++i) {
        std::snprintf(name, sizeof(name), "MKT-%03d", i);
        TickerKey    key = hash_ticker(name, std::strlen(name));
        MarketState* m   = g_market_pool.allocate();
        if (!m) std::abort();
        new (m) MarketState{};
        if (!g_markets.insert(key, m)) std::abort();
    }
}

// ── Main ─────────────────────────────────────────────────────────────────
int main() {
    kalshi::CycleClock clock;

    g_pool = build_payload_pool();
    g_latencies.resize(N_MEASURE);

    register_markets();

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::fprintf(stderr, "warn: mlockall failed (errno=%d) — "
                             "run with sudo to lock pages and avoid "
                             "page-fault outliers\n", errno);
    }

    std::thread t_prod(producer_thread);
    std::thread t_cons(consumer_thread);

    while (g_threads_ready.load(std::memory_order_acquire) < 2) {
        // spin
    }
    g_alloc_armed.store(true, std::memory_order_release);

    t_prod.join();
    t_cons.join();

    g_alloc_armed.store(false, std::memory_order_release);
    uint64_t allocs = g_alloc_count.load(std::memory_order_acquire);

    // Dump raw cycle samples + meta for offline plotting.
    if (FILE* f = std::fopen("/tmp/bench_hotpath_multi_cycles.bin", "wb")) {
        std::fwrite(g_latencies.data(), sizeof(uint64_t),
                    g_latencies.size(), f);
        std::fclose(f);
    }
    if (FILE* f = std::fopen("/tmp/bench_hotpath_multi_meta.txt", "w")) {
        std::fprintf(f, "ns_per_cycle %.12f\n", clock.to_ns(1));
        std::fprintf(f, "n_samples %zu\n", g_latencies.size());
        std::fprintf(f, "producer_core %u\n", CORE_PRODUCER);
        std::fprintf(f, "consumer_core %u\n", CORE_CONSUMER);
        std::fprintf(f, "n_tickers %d\n", N_TICKERS);
        std::fclose(f);
    }

    std::sort(g_latencies.begin(), g_latencies.end());

    uint64_t c_min  = g_latencies.front();
    uint64_t c_p50  = percentile(g_latencies, 0.50);
    uint64_t c_p90  = percentile(g_latencies, 0.90);
    uint64_t c_p99  = percentile(g_latencies, 0.99);
    uint64_t c_p999 = percentile(g_latencies, 0.999);
    uint64_t c_max  = g_latencies.back();

    uint64_t tput_cycles =
        g_throughput_end_tsc.load(std::memory_order_relaxed) -
        g_throughput_start_tsc.load(std::memory_order_relaxed);
    double tput_seconds = clock.to_ns(tput_cycles) / 1e9;
    double msgs_per_sec = static_cast<double>(N_MEASURE) / tput_seconds;

    auto to_ns = [&](uint64_t c) { return clock.to_ns(c); };

    std::printf("\n=== bench_hotpath_multi (2-thread SPSC pipeline, %d tickers) ===\n",
                N_TICKERS);
    std::printf("producer core = %u, consumer core = %u\n",
                CORE_PRODUCER, CORE_CONSUMER);
    std::printf("dispatch     = FlatHashMap<TickerKey,MarketState*,%zu>, %d distinct tickers\n",
                MAP_CAP, N_TICKERS);
    std::printf("market pool  = Pool<MarketState,%zu>\n", POOL_CAP);
    std::printf("N            = %d measured msgs (after %d warmup)\n",
                N_MEASURE, N_WARMUP);
    std::printf("\n            cycles        ns\n");
    std::printf("   min:   %8llu   %8.1f\n", (unsigned long long)c_min,  to_ns(c_min));
    std::printf("   p50:   %8llu   %8.1f\n", (unsigned long long)c_p50,  to_ns(c_p50));
    std::printf("   p90:   %8llu   %8.1f\n", (unsigned long long)c_p90,  to_ns(c_p90));
    std::printf("   p99:   %8llu   %8.1f\n", (unsigned long long)c_p99,  to_ns(c_p99));
    std::printf("  p999:   %8llu   %8.1f\n", (unsigned long long)c_p999, to_ns(c_p999));
    std::printf("   max:   %8llu   %8.1f\n", (unsigned long long)c_max,  to_ns(c_max));

    std::printf("\nsustained throughput: %.2f M msgs/sec  (over %.3f sec)\n",
                msgs_per_sec / 1e6, tput_seconds);
    std::printf("zero-alloc check: %llu allocations during measurement -- %s\n",
                (unsigned long long)allocs,
                allocs == 0 ? "PASS" : "FAIL");

    print_histogram(g_latencies, clock);
    return 0;
}
