// bench_hotpath.cpp — measures end-to-end latency of the parse → apply →
// evaluate → reconcile → serialize pipeline using rdtsc.

#include <simdjson.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/clock.h"
#include "feed/book.h"
#include "feed/parser.h"
#include "net/serialize.h"
#include "strategy/order_manager.h"
#include "strategy/signal.h"

// ── Optimizer barrier ────────────────────────────────────────────────────
// Empty inline asm with a "memory" clobber. Tells the compiler that the
// referenced value escapes to unknown code and that memory may have been
// touched, so it cannot eliminate or hoist the producing computation.
// Zero instructions emitted — pure compile-time constraint.
template <typename T>
__attribute__((always_inline)) inline void escape(T const& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

// ── Config ───────────────────────────────────────────────────────────────
constexpr int N_WARMUP  = 5'000;
constexpr int N_MEASURE = 100'000;

constexpr const char* kPayload =
    R"({"type":"orderbook_delta","sid":1,"seq":1,"msg":{"market_ticker":"DEMO-1","price":50,"delta":25,"side":"yes","ts":1234567890}})";

constexpr const char* kTicker = "DEMO-1";

// ── Percentile helper ────────────────────────────────────────────────────
// Expects `sorted` already sorted ascending.
static uint64_t percentile(const std::vector<uint64_t>& sorted, double pct) {
    if (sorted.empty()) return 0;
    size_t idx = static_cast<size_t>(pct * (sorted.size() - 1));
    return sorted[idx];
}

int main() {
    // ── Calibrate cycles-to-ns ───────────────────────────────────────────
    // Constructor sleeps ~50ms and computes ns_per_cycle.
    kalshi::CycleClock clock;

    // ── Pre-build everything outside the measurement window ─────────────
    simdjson::padded_string padded(kPayload, std::strlen(kPayload));
    kalshi::OrderbookParser parser;
    kalshi::Book            book;
    kalshi::Signal          signal;
    kalshi::OrderManager    om;
    char                    outbuf[256];

    // ── Warmup ──────────────────────────────────────────────────────────
    // Run the pipeline untimed to populate L1i, branch predictors, TLB.
    for (int i = 0; i < N_WARMUP; ++i) {
        kalshi::OrderbookDelta delta;
        parser.parse_orderbook_delta(padded, delta);
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

    // ── Measurement ─────────────────────────────────────────────────────
    std::vector<uint64_t> cycles(N_MEASURE);

    for (int i = 0; i < N_MEASURE; ++i) {
        uint64_t t0 = kalshi::rdtsc();

        kalshi::OrderbookDelta delta;
        parser.parse_orderbook_delta(padded, delta);
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
        cycles[i] = t1 - t0;
    }

    // ── Stats ───────────────────────────────────────────────────────────
    std::sort(cycles.begin(), cycles.end());

    uint64_t c_min  = cycles.front();
    uint64_t c_p50  = percentile(cycles, 0.50);
    uint64_t c_p90  = percentile(cycles, 0.90);
    uint64_t c_p99  = percentile(cycles, 0.99);
    uint64_t c_p999 = percentile(cycles, 0.999);
    uint64_t c_max  = cycles.back();

    auto to_ns = [&](uint64_t c) { return clock.to_ns(c); };

    std::printf("\n=== bench_hotpath ===\n");
    std::printf("N = %d iterations\n\n", N_MEASURE);
    std::printf("            cycles        ns\n");
    std::printf("   min:   %8llu   %8.1f\n", (unsigned long long)c_min,  to_ns(c_min));
    std::printf("   p50:   %8llu   %8.1f\n", (unsigned long long)c_p50,  to_ns(c_p50));
    std::printf("   p90:   %8llu   %8.1f\n", (unsigned long long)c_p90,  to_ns(c_p90));
    std::printf("   p99:   %8llu   %8.1f\n", (unsigned long long)c_p99,  to_ns(c_p99));
    std::printf("  p999:   %8llu   %8.1f\n", (unsigned long long)c_p999, to_ns(c_p999));
    std::printf("   max:   %8llu   %8.1f\n", (unsigned long long)c_max,  to_ns(c_max));

    std::printf("\nclaim: p50 < 3000 ns -- %s\n",
                to_ns(c_p50) < 3000.0 ? "PASS" : "FAIL");

    return 0;
}
