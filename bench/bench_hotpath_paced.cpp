// bench_hotpath_paced.cpp — open-loop paced two-thread SPSC pipeline bench.
//
// Unlike bench_hotpath_pipe (closed-loop, saturation), this bench models an
// ARRIVAL SCHEDULE: message i is due at absolute time T0 + offset[i]. The
// producer spins on TSC until each deadline, stamps the message with its
// SCHEDULED time (not the actual send time), parses, and pushes. Latency is
// consumer-end rdtscp minus the scheduled arrival — so producer lateness and
// backlog delay are charged to every affected message. This is the
// wrk2/HdrHistogram-style correction for coordinated omission.
//
// Modes:
//   uniform  — deterministic spacing at 1/rate
//   poisson  — exponential inter-arrival gaps (mean 1/rate)
//   burst    — bursts of B back-to-back msgs; burst starts spaced B/rate
//
// Usage:
//   bench_hotpath_paced --rate 1000000 --mode poisson [--n 1000000]
//                       [--burst 64] [--pcore 16] [--ccore 18] [--csv]

#include <simdjson.h>

#include <sys/mman.h>      // mlockall

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

#include "core/clock.h"
#include "core/spsc_queue.h"
#include "feed/book.h"
#include "feed/parser.h"
#include "net/serialize.h"
#include "strategy/order_manager.h"
#include "strategy/signal.h"
#include "system/tuning.h"

// ── Zero-allocation enforcement (same mechanism as bench_hotpath_pipe) ───
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

// ── Config (CLI-overridable) ─────────────────────────────────────────────
constexpr int    N_WARMUP    = 10'000;
constexpr int    N_PAYLOADS  = 1024;
constexpr size_t QUEUE_CAP   = 1024;

static int      g_n_measure   = 1'000'000;
static double   g_rate_hz     = 1'000'000.0;
static int      g_burst_size  = 64;
static unsigned g_core_prod   = 16;
static unsigned g_core_cons   = 18;
static int      g_rt_priority = 50;
static bool     g_csv         = false;

enum class Mode { UNIFORM, POISSON, BURST };
static Mode g_mode = Mode::UNIFORM;
static const char* mode_name(Mode m) {
    switch (m) {
        case Mode::UNIFORM: return "uniform";
        case Mode::POISSON: return "poisson";
        case Mode::BURST:   return "burst";
    }
    return "?";
}

constexpr const char* kTicker = "DEMO-1";

// ── Message crossing the queue ───────────────────────────────────────────
struct HotMsg {
    kalshi::OrderbookDelta delta;        // 64 B
    uint64_t               sched_tsc;    //  8 B — SCHEDULED arrival time
};
static_assert(sizeof(HotMsg) == 72,
              "HotMsg layout drift — bench assumes 72 bytes");

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
    // uniform in (0, 1] — never returns 0, safe for log()
    double unit() {
        return (static_cast<double>(next() >> 11) + 1.0) / 9007199254740993.0;
    }
};

// ── Synthetic payload pool (identical to bench_hotpath_pipe) ─────────────
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

// ── Schedule construction ────────────────────────────────────────────────
// offset[i] = cycles after T0 at which message i is DUE. Built before the
// measured window; no allocation during measurement.
static std::vector<uint64_t> build_schedule(int n, double rate_hz, Mode mode,
                                            int burst, double cycles_per_ns) {
    std::vector<uint64_t> off(static_cast<size_t>(n));
    const double mean_gap_ns = 1e9 / rate_hz;
    Xorshift64   rng{0x0123456789ABCDEFULL};
    double       t_ns = 0.0;

    switch (mode) {
        case Mode::UNIFORM:
            for (int i = 0; i < n; ++i) {
                off[i] = static_cast<uint64_t>(t_ns * cycles_per_ns);
                t_ns += mean_gap_ns;
            }
            break;
        case Mode::POISSON:
            for (int i = 0; i < n; ++i) {
                off[i] = static_cast<uint64_t>(t_ns * cycles_per_ns);
                t_ns += -std::log(rng.unit()) * mean_gap_ns;
            }
            break;
        case Mode::BURST: {
            // Bursts of `burst` messages due simultaneously; burst starts
            // spaced so the long-run average rate equals rate_hz.
            const double burst_gap_ns = mean_gap_ns * burst;
            for (int i = 0; i < n; ++i) {
                if (i != 0 && (i % burst) == 0) t_ns += burst_gap_ns;
                off[i] = static_cast<uint64_t>(t_ns * cycles_per_ns);
            }
            break;
        }
    }
    return off;
}

// ── Shared state ─────────────────────────────────────────────────────────
kalshi::SPSCQueue<HotMsg, QUEUE_CAP> g_queue;
std::vector<simdjson::padded_string> g_pool;
std::vector<uint64_t>                g_offsets;    // measured msgs only
std::vector<uint64_t>                g_latencies;  // measured msgs only

std::atomic<int>      g_threads_ready{0};
std::atomic<uint64_t> g_t0{0};                     // schedule anchor (TSC)
std::atomic<uint64_t> g_throughput_end_tsc{0};
std::atomic<uint64_t> g_max_lateness_cycles{0};    // producer behind-schedule

// ── Producer ─────────────────────────────────────────────────────────────
static void producer_thread() {
    if (!kalshi::pin_this_thread_to_core(g_core_prod))
        std::fprintf(stderr, "warn: failed to pin producer to core %u\n",
                     g_core_prod);
    if (!kalshi::set_realtime_priority(g_rt_priority))
        std::fprintf(stderr, "warn: producer SCHED_FIFO@%d failed (errno=%d)\n",
                     g_rt_priority, errno);

    kalshi::OrderbookParser parser;
    HotMsg                  msg;

    // Warmup: closed-loop, untimed — warms caches, predictor, simdjson.
    for (int i = 0; i < N_WARMUP; ++i) {
        const auto& payload = g_pool[i & (N_PAYLOADS - 1)];
        parser.parse_orderbook_delta(payload, msg.delta);
        msg.sched_tsc = kalshi::rdtsc();
        while (!g_queue.try_push(msg)) { /* spin */ }
    }

    g_threads_ready.fetch_add(1, std::memory_order_release);
    while (!g_alloc_armed.load(std::memory_order_acquire)) { /* spin */ }

    // Anchor the schedule slightly in the future so message 0 is not
    // already late the moment we read the clock.
    const uint64_t t0 = kalshi::rdtsc() + 10'000;
    g_t0.store(t0, std::memory_order_release);

    uint64_t max_late = 0;

    for (int i = 0; i < g_n_measure; ++i) {
        const uint64_t deadline = t0 + g_offsets[i];

        // Open-loop pacing: spin until the message is due. If we are
        // already past the deadline (running behind), do NOT re-anchor —
        // fall through immediately; the lateness is charged to this
        // message because we stamp the SCHEDULED time below.
        uint64_t now = kalshi::rdtsc();
        while (now < deadline) now = kalshi::rdtsc();

        const uint64_t late = now - deadline;
        if (late > max_late) max_late = late;

        const auto& payload = g_pool[i & (N_PAYLOADS - 1)];
        parser.parse_orderbook_delta(payload, msg.delta);
        msg.sched_tsc = deadline;   // scheduled arrival, NOT actual time

        while (!g_queue.try_push(msg)) { /* spin under backpressure */ }
    }

    g_max_lateness_cycles.store(max_late, std::memory_order_release);
}

// ── Consumer ─────────────────────────────────────────────────────────────
static void consumer_thread() {
    if (!kalshi::pin_this_thread_to_core(g_core_cons))
        std::fprintf(stderr, "warn: failed to pin consumer to core %u\n",
                     g_core_cons);
    if (!kalshi::set_realtime_priority(g_rt_priority))
        std::fprintf(stderr, "warn: consumer SCHED_FIFO@%d failed (errno=%d)\n",
                     g_rt_priority, errno);

    kalshi::Book         book;
    kalshi::Signal       signal;
    kalshi::OrderManager om;
    char                 outbuf[256];
    HotMsg               msg;

    for (int i = 0; i < N_WARMUP; ++i) {
        while (!g_queue.try_pop(msg)) { /* spin */ }
        book.apply(msg.delta);
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

    g_threads_ready.fetch_add(1, std::memory_order_release);
    while (!g_alloc_armed.load(std::memory_order_acquire)) { /* spin */ }

    for (int i = 0; i < g_n_measure; ++i) {
        while (!g_queue.try_pop(msg)) { /* spin waiting for producer */ }

        book.apply(msg.delta);
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

        uint64_t end_tsc = kalshi::rdtscp();
        g_latencies[i] = end_tsc - msg.sched_tsc;
    }

    g_throughput_end_tsc.store(kalshi::rdtscp(), std::memory_order_release);
}

// ── Stats ────────────────────────────────────────────────────────────────
static uint64_t percentile(const std::vector<uint64_t>& sorted, double pct) {
    if (sorted.empty()) return 0;
    size_t idx = static_cast<size_t>(pct * (sorted.size() - 1));
    return sorted[idx];
}

// ── CLI ──────────────────────────────────────────────────────────────────
static void parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if      (!std::strcmp(argv[i], "--rate"))  g_rate_hz    = std::atof(next("--rate"));
        else if (!std::strcmp(argv[i], "--n"))     g_n_measure  = std::atoi(next("--n"));
        else if (!std::strcmp(argv[i], "--burst")) g_burst_size = std::atoi(next("--burst"));
        else if (!std::strcmp(argv[i], "--pcore")) g_core_prod  = static_cast<unsigned>(std::atoi(next("--pcore")));
        else if (!std::strcmp(argv[i], "--ccore")) g_core_cons  = static_cast<unsigned>(std::atoi(next("--ccore")));
        else if (!std::strcmp(argv[i], "--csv"))   g_csv        = true;
        else if (!std::strcmp(argv[i], "--mode")) {
            const char* m = next("--mode");
            if      (!std::strcmp(m, "uniform")) g_mode = Mode::UNIFORM;
            else if (!std::strcmp(m, "poisson")) g_mode = Mode::POISSON;
            else if (!std::strcmp(m, "burst"))   g_mode = Mode::BURST;
            else { std::fprintf(stderr, "unknown mode %s\n", m); std::exit(2); }
        }
        else { std::fprintf(stderr, "unknown arg %s\n", argv[i]); std::exit(2); }
    }
    if (g_rate_hz <= 0 || g_n_measure <= 0 || g_burst_size <= 0) {
        std::fprintf(stderr, "invalid config\n");
        std::exit(2);
    }
}

int main(int argc, char** argv) {
    parse_args(argc, argv);

    kalshi::CycleClock clock;
    const double ns_per_cycle   = clock.to_ns(1);
    const double cycles_per_ns  = 1.0 / ns_per_cycle;

    g_pool = build_payload_pool();
    g_latencies.resize(static_cast<size_t>(g_n_measure));
    g_offsets = build_schedule(g_n_measure, g_rate_hz, g_mode,
                               g_burst_size, cycles_per_ns);

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::fprintf(stderr, "warn: mlockall failed (errno=%d)\n", errno);
    }

    std::thread t_prod(producer_thread);
    std::thread t_cons(consumer_thread);

    while (g_threads_ready.load(std::memory_order_acquire) < 2) { /* spin */ }
    g_alloc_armed.store(true, std::memory_order_release);

    t_prod.join();
    t_cons.join();

    g_alloc_armed.store(false, std::memory_order_release);
    uint64_t allocs = g_alloc_count.load(std::memory_order_acquire);

    // Achieved throughput over the measured window.
    uint64_t t0        = g_t0.load(std::memory_order_acquire);
    uint64_t t_end     = g_throughput_end_tsc.load(std::memory_order_acquire);
    double   window_s  = clock.to_ns(t_end - t0) / 1e9;
    double   achieved  = static_cast<double>(g_n_measure) / window_s;
    double   max_late_ns =
        clock.to_ns(g_max_lateness_cycles.load(std::memory_order_acquire));

    std::sort(g_latencies.begin(), g_latencies.end());
    auto ns = [&](double pct) { return clock.to_ns(percentile(g_latencies, pct)); };
    double v_min = clock.to_ns(g_latencies.front());
    double v_max = clock.to_ns(g_latencies.back());

    // Sustainability: if the producer ended far behind schedule, the offered
    // rate exceeded capacity and latencies are backlog-dominated.
    const bool sustainable = max_late_ns < 1e6;   // < 1 ms behind at worst

    if (g_csv) {
        // mode,rate_hz,n,burst,p50_ns,p90_ns,p99_ns,p999_ns,max_ns,
        // achieved_hz,max_late_ns,allocs,sustainable
        std::printf("%s,%.0f,%d,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f,%.0f,%llu,%d\n",
                    mode_name(g_mode), g_rate_hz, g_n_measure,
                    g_mode == Mode::BURST ? g_burst_size : 1,
                    ns(0.50), ns(0.90), ns(0.99), ns(0.999), v_max,
                    achieved, max_late_ns,
                    (unsigned long long)allocs, sustainable ? 1 : 0);
        return 0;
    }

    std::printf("\n=== bench_hotpath_paced (open-loop, schedule-stamped) ===\n");
    std::printf("mode = %s, offered rate = %.0f msg/s", mode_name(g_mode), g_rate_hz);
    if (g_mode == Mode::BURST) std::printf(", burst = %d", g_burst_size);
    std::printf("\ncores = %u -> %u, N = %d measured (after %d warmup)\n",
                g_core_prod, g_core_cons, g_n_measure, N_WARMUP);
    std::printf("\n   latency from SCHEDULED arrival to serialized action:\n");
    std::printf("   min:  %10.1f ns\n", v_min);
    std::printf("   p50:  %10.1f ns\n", ns(0.50));
    std::printf("   p90:  %10.1f ns\n", ns(0.90));
    std::printf("   p99:  %10.1f ns\n", ns(0.99));
    std::printf("  p999:  %10.1f ns\n", ns(0.999));
    std::printf("   max:  %10.1f ns\n", v_max);
    std::printf("\nachieved throughput : %.3f M msg/s (window %.3f s)\n",
                achieved / 1e6, window_s);
    std::printf("producer max lateness: %.1f ns %s\n", max_late_ns,
                sustainable ? "" : " (UNSUSTAINABLE: offered rate > capacity)");
    std::printf("zero-alloc check     : %llu allocations -- %s\n",
                (unsigned long long)allocs, allocs == 0 ? "PASS" : "FAIL");
    return 0;
}
