# kalshi-cpp

Low-latency prediction market trading client in C++20. Connects to Kalshi's WebSocket and REST APIs with lock-free inter-thread communication, custom memory allocators, and OS-level performance tuning. Zero heap allocation on the hot path.

## Why This Exists

There is no C++ client for any prediction market. Every existing Kalshi/Polymarket client is Python, TypeScript, or Go. This project fills that gap while demonstrating the systems programming techniques used in production trading infrastructure: async networking, lock-free queues, arena/pool allocators, CPU pinning, and nanosecond-resolution latency measurement.

To avoid the KYC/SSN requirements of Kalshi's regulated demo environment and to enable reproducible, controllable testing, this project also includes a **conformant exchange simulator** (`kalshi-sim`) that speaks Kalshi's wire protocol (RSA-PSS-signed REST + WebSocket market data feed). Both ends — client and simulator — are written in C++ and optimized. The sim lets us drive adversarial scenarios (latency spikes, partial fills, network drops, sequence-number gaps) on demand, and runs end-to-end benchmarks under realistic conditions injected via `tc netem`.

## Architecture

```
   ┌──────────────────────────────┐         ┌──────────────────────────────┐
   │       kalshi-cpp client      │         │       kalshi-sim  server     │
   │                              │         │                              │
   │  ┌──────────┐                │   WSS   │              ┌────────────┐  │
   │  │ Network  │ ─── orders ──> │ ──────> │ ──── feed ── │ Matching   │  │
   │  │ Thread   │                │  REST   │              │ Engine     │  │
   │  │          │ <── fills ──── │ <────── │ ── replies ─>│ + Auth Ver │  │
   │  │  io_uring│                │ TLS+PSS │              │            │  │
   │  └──────────┘                │  signed │              └────────────┘  │
   │       │                      │         │                    │         │
   │       │ SPSC                 │         │                    │         │
   │       v                      │         │                    v         │
   │  ┌────────────┐              │         │   ┌──────────────────────┐   │
   │  │  Strategy  │              │         │   │  scenario injection: │   │
   │  │  Order Mgr │              │         │   │  latency, drops,     │   │
   │  │  Book      │              │         │   │  partial fills,      │   │
   │  └────────────┘              │         │   │  seq gaps            │   │
   │       │                      │         │   └──────────────────────┘   │
   │       v                      │         │                              │
   │  ┌──────────────────────┐    │         │  CPU pin, NODELAY, busy poll │
   │  │  Latency Logger      │    │         └──────────────────────────────┘
   │  │  (rdtsc, p50/p99)    │    │                          │
   │  └──────────────────────┘    │                          │
   │                              │            tc netem on loopback adds
   │  Memory: Arena + Pool        │            realistic latency + jitter
   │  OS: CPU pin, huge pages,    │            (100 µs RTT, 0.1% loss)
   │      mlockall, FIFO          │
   └──────────────────────────────┘
```

**Two processes, identical low-latency techniques on both sides:**
- **Client (`kalshi-cpp`)** — two threads (network + strategy) connected by lock-free SPSC queues. Sends signed REST orders, consumes WS market data feed.
- **Simulator (`kalshi-sim`)** — verifies signatures with the public key, replays/generates market data over WS, runs a CLOB matching engine, supports scenario-injection knobs (delay, drops, partial fills) over a control channel.

**Latency realism on a single host**: `tc qdisc add dev lo root netem delay 100us 20us` injects realistic LAN-class delay + jitter on the loopback interface, so end-to-end measurements are comparable to a real colocated deployment.

**I/O backend**: Boost.Asio abstracts the kernel interface — `io_uring` on Linux 5.15+, `epoll` fallback. On Windows: IOCP. Code is identical; Asio picks at compile time.

**Two threads, no locks:**
- **Network thread**: owns the WebSocket connection, parses incoming JSON, pushes parsed market data into an SPSC queue. Also sends outbound orders received from the strategy thread's SPSC queue.
- **Strategy thread**: reads market data from queue, maintains local order book, generates signals, manages orders.

Communication is strictly via lock-free SPSC ring buffers. No mutex, no condition variable, no shared mutable state.

## Platform

**Target**: Linux x86-64 (production trading systems run Linux).
**Development**: Windows 11 + WSL2 (Ubuntu 22.04+). All code compiles and runs inside WSL2, which provides a real Linux kernel.

The codebase uses POSIX/Linux APIs for OS-level tuning (Phase 4). These have Windows equivalents for reference but are not abstracted behind a portability layer — targeting one platform avoids #ifdef bloat and lets us use each API idiomatically.

| Capability | Linux API | Windows equivalent (reference only) |
|---|---|---|
| CPU pinning | `sched_setaffinity()` | `SetThreadAffinityMask()` |
| Real-time scheduling | `sched_setscheduler(SCHED_FIFO)` | `SetPriorityClass(REALTIME_PRIORITY_CLASS)` |
| Lock memory | `mlockall(MCL_CURRENT \| MCL_FUTURE)` | `VirtualLock()` |
| Huge pages | `mmap(MAP_HUGETLB)` | `VirtualAlloc(MEM_LARGE_PAGES)` |
| Transparent huge pages | `madvise(MADV_HUGEPAGE)` | N/A (Windows uses large pages explicitly) |
| High-res timestamp | `rdtsc` inline asm | `__rdtsc()` intrinsic (identical instruction) |

**WSL2 notes**: WSL2 runs a real Linux 5.15+ kernel, so `epoll`, `sched_setaffinity`, `mmap(MAP_HUGETLB)`, and `mlockall` all work. `io_uring` support depends on the WSL kernel version (available in 5.15+ but may require kernel update). `SCHED_FIFO` requires `CAP_SYS_NICE` (run with `sudo` or adjust `/etc/security/limits.conf`). Huge pages require `vm.nr_hugepages` sysctl configuration.

## Scope

### Phase 1: Foundation (Week 1-2)

**1.1 Build system + project structure**
```
kalshi-cpp/
├── CMakeLists.txt
├── DESIGN.md
├── src/                            # Client process
│   ├── main.cpp
│   ├── net/                        # Networking layer
│   │   ├── ws_client.h/cpp             # WebSocket client (Boost.Beast)
│   │   ├── ws_reconnect.h/cpp          # Reconnection state machine + backoff
│   │   ├── rest_client.h/cpp           # REST client (order placement)
│   │   ├── rate_limiter.h              # Token-bucket rate limiter (mirrors Kalshi tiers)
│   │   ├── sockopt.h/cpp               # NODELAY / busy-poll / QUICKACK / buf sizing
│   │   └── auth.h/cpp                  # RSA-PSS SHA256 signing
│   ├── feed/                       # Market data processing
│   │   ├── parser.h/cpp                # JSON -> internal structs (simdjson)
│   │   └── book.h/cpp                  # Order book reconstruction
│   ├── core/                       # Low-latency primitives (DONE)
│   │   ├── spsc_queue.h                # Lock-free SPSC ring buffer
│   │   ├── arena_alloc.h               # Arena (bump) allocator
│   │   ├── pool_alloc.h                # Fixed-size pool allocator
│   │   ├── flat_hash_map.h             # Robin Hood open-addressing hash map
│   │   └── clock.h                     # rdtsc + calibration
│   ├── strategy/                   # Trading logic
│   │   ├── signal.h/cpp                # Signal generation
│   │   └── order_manager.h/cpp         # Order lifecycle
│   ├── system/                     # OS-level optimization
│   │   └── tuning.h/cpp                # CPU pin, huge pages, mlockall, SCHED_FIFO
│   └── util/
│       ├── log.h                       # Lock-free logging
│       └── histogram.h                 # Latency percentile tracking (HDR)
├── sim/                            # Exchange simulator `kalshi-sim` (Phase 5)
│   ├── main.cpp                        # Server entrypoint, arg parsing
│   ├── rest_server.h/cpp               # HTTPS server (Beast); endpoint dispatch
│   ├── ws_server.h/cpp                 # WS feed (replay + generative modes)
│   ├── auth_verify.h/cpp               # EVP_DigestVerify against registered pubkeys
│   ├── matching_engine.h/cpp           # CLOB, price-time priority
│   ├── scenario.h/cpp                  # /sim/* control endpoint (inject delay etc.)
│   └── replay/                         # Captured Kalshi payload samples
│       └── *.json
├── bench/                          # Micro-benchmarks
│   ├── bench_spsc.cpp                  # DONE
│   ├── bench_pool.cpp                  # DONE
│   ├── bench_flat_hash_map.cpp         # DONE
│   ├── bench_arena.cpp
│   ├── bench_parser.cpp                # simdjson vs nlohmann
│   ├── bench_book.cpp                  # orderbook_delta apply latency
│   └── bench_e2e.cpp                   # tick-to-trade against kalshi-sim under tc netem
├── test/                           # Unit tests
│   ├── test_spsc.cpp                   # DONE
│   ├── test_pool.cpp                   # DONE
│   ├── test_flat_hash_map.cpp          # DONE
│   ├── test_auth.cpp                   # Self-test + Kalshi reference vector
│   ├── test_book.cpp
│   ├── test_parser.cpp
│   └── test_matching.cpp               # kalshi-sim CLOB correctness
├── scripts/
│   ├── isolate_cpus.sh                 # CPU isolation helper
│   ├── netem_colocated.sh              # tc netem: ~5 us RTT
│   ├── netem_lan.sh                    # tc netem: ~100 us RTT, ±20 us jitter
│   ├── netem_wan.sh                    # tc netem: 1 ms RTT, 0.1% loss
│   └── netem_clear.sh                  # tc qdisc del
└── docs/
    ├── PRODUCTION_TUNING.md            # Kernel-bypass NIC path (Solarflare/DPDK)
    └── BENCHMARK_RESULTS.md            # Per-knob latency ablation table
```

**1.2 Kalshi auth + REST client**
- RSA-PSS SHA256 signature generation using OpenSSL EVP API
- Signature message format: `{timestamp_ms}{HTTP_METHOD}{path}` (path only, no host or query params)
- Three headers per request: `KALSHI-ACCESS-KEY`, `KALSHI-ACCESS-TIMESTAMP` (ms), `KALSHI-ACCESS-SIGNATURE` (Base64)
- REST client for account info, market listing, order placement
- **Auth testing**: round-trip self-test using a locally generated throwaway RSA keypair — sign with private key, verify with matching public key via `EVP_DigestVerify*`. No Kalshi credentials needed.
- **Reference vector test**: cross-check against the Kalshi Python SDK's signing output for a fixed (key, message) pair to prove byte-identical conformance with Kalshi's wire format.
- Base URL: configurable at runtime via `KALSHI_API_BASE` env var. Defaults to local simulator (`http://127.0.0.1:8443`); same code works against real Kalshi for anyone with credentials.
- Target: signature self-test passes, reference vector matches Python SDK byte-for-byte.

**1.3 WebSocket client**
- Boost.Beast WebSocket with TLS (Boost.Asio + OpenSSL)
- `TCP_NODELAY` enabled
- Auth headers (`KALSHI-ACCESS-KEY/TIMESTAMP/SIGNATURE`) sent during HTTP upgrade handshake; signed path is always `GET/trade-api/ws/v2`
- WebSocket URL: configurable via `KALSHI_WS_URL` env var; defaults to local simulator (`ws://127.0.0.1:8444/trade-api/ws/v2`).
- Subscribe to `orderbook_delta`, `ticker`, `trade` channels
- Parse incoming JSON with simdjson (2-4x faster than nlohmann/json)
- Target: consume a captured Kalshi feed replayed by the simulator (Phase 5), print parsed orderbook deltas.

**1.4 WebSocket reconnection**
- Connections drop: server maintenance (Thursdays 3-5 AM ET), network blips, idle timeouts
- Reconnection state machine: `Connected -> Disconnected -> Backoff -> Reconnecting -> Connected`
- Exponential backoff: 100ms, 200ms, 400ms, ... capped at 5s
- On reconnect: re-authenticate, re-subscribe to all channels, request fresh `orderbook_snapshot` (sequence numbers will have gaps — never trust stale book state)
- Strategy thread must handle a `BookStale` message type: pause order placement until a fresh snapshot is applied
- Kalshi WebSocket messages include `seq` (sequence number) per subscription; detect gaps to trigger re-snapshot even without a full disconnect

### Phase 2: Low-Latency Infrastructure (Week 3-4)

**2.1 Lock-free SPSC queue**
- Power-of-two ring buffer
- `std::atomic<size_t>` head/tail with `acquire`/`release` ordering (not `seq_cst`)
- `alignas(64)` on head and tail to prevent false sharing
- Locally cached indices to minimize atomic reads
- Target: benchmark against rigtorp/SPSCQueue (133ns RTT baseline)

**2.2 Custom memory allocators**
- **Arena allocator**: bump pointer, bulk reset per tick. Used for JSON parse buffers.
  - `allocate(size, align)`: bump offset, return pointer. O(1).
  - `reset()`: set offset to 0. O(1).
  - Backed by `mmap` (Linux). `MAP_HUGETLB` deferred to Phase 4 (requires `vm.nr_hugepages` sysctl).
  - Must allocate `SIMDJSON_PADDING` (64 bytes) extra past the end of each input buffer — simdjson reads past the input using SIMD and requires this padding to avoid segfaults.
  - **No pmr inheritance**: parser calls `arena.allocate()` directly; simdjson uses its own allocator. Skipping pmr keeps the arena lean (~40 lines, no vtable).
- **Pool allocator**: fixed-size blocks for Order objects.
  - Intrusive free-list through the blocks themselves (no separate node allocation).
  - O(1) allocate (pop), O(1) deallocate (push).
  - **No pmr inheritance**: no consumer in the codebase uses STL containers backed by the pool (the order map is `FlatHashMap`, not `std::pmr::unordered_map`). Skipping pmr saves ~15 lines, one vtable pointer, and indirect virtual calls. Trivial to add back later if needed.
- **Flat hash map**: open-addressing, fixed-capacity, linear probing with backshift deletion. Replaces `std::pmr::unordered_map` for order lookups.
  - Template `FlatHashMap<Key, Value, Capacity>` where `Capacity` is a power of two.
  - Default sizing for Kalshi: `Capacity = 524288` (2^19) — supports the full 200K open-order API limit at ~39% load factor, fits in 8 MB.
  - Sentinel key (e.g., `UINT64_MAX`) marks empty slots — no separate state array.
  - Lookup: hash key → probe forward until match or empty slot. Expected p50 ~15ns, p99 ~50ns at 200K entries (vs `std::unordered_map` p50 ~80ns, p99 ~500ns).
  - Never grows: insertion fails when ≥ 70% loaded. Predictable failure beats unpredictable rehash spikes (50ms+).
- **Verification**: override global `operator new`/`operator delete` in debug builds. Set a thread-local `bool hot_path_active` flag; when true, any call to `operator new` triggers `assert(false)` with a backtrace. This catches hidden allocations from STL containers, simdjson internals, Boost, or accidental `std::string` construction.
- Target: benchmark allocators against `malloc`/`new` showing deterministic latency (no jitter from syscalls). Benchmark `FlatHashMap` vs `std::unordered_map` at 200K entries.

**2.3 Internal message types**
- Flat POD structs for market data messages (no `std::string`, no heap)
- Fixed-size char arrays for symbols/tickers
- `static_assert` on struct sizes to catch padding issues

### Phase 3: Order Book + Strategy (Week 4-5)

**3.1 Order book reconstruction**
- Flat array indexed by price tick (Kalshi standard prices are 1-99 cents, so 99-element array)
- Each level: quantity (fixed-point integer to avoid float) + order count
- Apply `orderbook_delta` messages: `delta_fp` is a signed quantity change per `(price, side)` — add to existing level, remove level if quantity reaches zero, insert new level if absent
- Kalshi only returns YES bids and NO bids (not asks). A YES bid at price P implies a NO ask at (1.00 - P). Best YES ask = 1.00 - best NO bid.
- Best bid/ask as direct cached values (updated on every delta)
- Target: verify book state matches Kalshi REST snapshot periodically
- **Subpenny note**: Kalshi FIX supports 4-decimal-place prices (e.g., $0.4275). The REST/WS API currently uses cent-level pricing for standard markets. The flat array design covers this case. If subpenny markets are added to the WS feed, the book would need a sorted flat array or `std::pmr::flat_map` keyed by integer price-in-hundredths-of-cents (10000 possible levels) — still cache-friendly and allocation-free with pmr.

**3.2 Order manager**
- Order lifecycle: New -> Pending -> Acked -> Filled/Cancelled
- Pool-allocated Order objects
- Order ID -> Order* lookup via custom `FlatHashMap` (open-addressing, ~15ns p50 vs ~80ns for `std::unordered_map`)
  - Sized at 524,288 slots (covers Kalshi's 200K open-order API cap with headroom; 8 MB footprint)
  - Capacity is a template parameter — bump for higher-volume venues (options MM workloads can need 1M+)
- Pre-flight risk checks (position limit, max order size, max notional)

**3.3 Rate limiter**
- Kalshi uses a token-bucket rate limiter with separate read and write buckets
- Basic tier: 200 read tokens/sec, 100 write tokens/sec; default cost 10 tokens/request
- Effective limit at Basic tier: ~10 orders/sec. Our system will be faster than what the exchange permits.
- Implement a local token bucket that mirrors Kalshi's: reject or queue orders before sending to avoid 429 responses
- Higher tiers (Advanced: 300/300, Premier: 1000/1000) available on application — document the tier in config

**3.4 Basic spread strategy**
- Monitor bid-ask spread on selected markets
- Place limit orders when spread exceeds threshold
- Cancel stale orders on book updates
- Not meant to be profitable; meant to demonstrate the full order lifecycle

### Phase 4: OS- and Network-Level Optimization (Week 5-6)

Each subsection below ships with a before/after benchmark. The story is not "tuned values" but "measured per-knob latency contributions."

**4.1 Thread tuning**
- `sched_setaffinity()`: pin network thread to core 1, strategy thread to core 2. Avoid core 0 (handles hardware interrupts by default on most Linux kernels).
- `sched_setscheduler(SCHED_FIFO)`: real-time priority to avoid preemption. Requires `CAP_SYS_NICE` or root. If the thread has a bug and never yields (blocks on I/O or sleeps), it will starve other processes on that core.
- `mlockall(MCL_CURRENT | MCL_FUTURE)`: prevent page faults after startup. Without this, rarely-accessed pages can be swapped out; re-accessing them incurs ~10-100us page fault latency.
- **Isolated CPUs** (`isolcpus=1,2` kernel cmdline) — exclude trading cores from the general scheduler entirely. Documented as a deployment-time setting; not required for the benchmark suite.

**4.2 Memory tuning**
- Huge pages via `mmap(MAP_HUGETLB)` for arena and pool backing memory
- `madvise(MADV_HUGEPAGE)` for transparent huge pages on the order book array
- Pre-fault all pages at startup (touch every page to avoid runtime faults)

**4.3 Socket-level network tuning**
- **`TCP_NODELAY`** — disables Nagle's algorithm so single-message orders ship immediately. Expected ~5-10× latency improvement on small writes.
- **`SO_BUSY_POLL`** (Linux) — kernel busy-polls the NIC for incoming packets instead of waiting on interrupts. Cuts wakeup latency from ~20 µs to ~3-5 µs at the cost of one core's worth of CPU.
- **`TCP_QUICKACK`** — disable delayed ACKs on the receive side; reduces tail latency on the ack path.
- **`SO_RCVBUF` / `SO_SNDBUF`** sized large at startup to avoid kernel-side growth mid-stream.
- **`io_uring` backend** (Boost.Asio with `BOOST_ASIO_HAS_IO_URING`) — submission-queue/completion-queue model, syscall-free in the steady state. Expected 30-50% latency win on the receive path vs `epoll`.
- Bench plan: side-by-side comparison of {default sockopts, `TCP_NODELAY`, `+SO_BUSY_POLL`, `+io_uring`} configurations measured end-to-end against the simulator.

**4.4 Realistic network conditions via `tc netem`**
Loopback is too fast (~5 µs RTT) to be representative. We inject realistic conditions on the loopback interface using Linux's network emulator:
```bash
# Colocated trading link (~5 µs RTT, low jitter) — what HFT customers actually pay for
sudo tc qdisc add dev lo root netem delay 5us 1us

# LAN-class (~100 µs base, ±20 µs jitter)
sudo tc qdisc replace dev lo root netem delay 100us 20us

# Lossy WAN (0.1% drop, 1 ms base)
sudo tc qdisc replace dev lo root netem delay 1ms loss 0.1%

# Remove all rules
sudo tc qdisc del dev lo root
```
Every Phase 4 benchmark is run under each profile, so we can show how each optimization scales as the underlying transport gets slower or noisier.

**4.5 Production-only optimizations (documented, not implemented)**
Kernel-bypass and NIC-specific paths require physical hardware:
- **Solarflare OpenOnload / `ef_vi`**, **Mellanox `ibverbs`/`rdma`**, or **DPDK** — packets bypass the kernel and land directly in userspace, ~1 µs round trip vs ~15 µs through the kernel stack.
- **NIC hardware timestamping** for nanosecond-accurate wire timestamps.
- **PTP/PPS clock sync** for cross-host time alignment.

These are described in `docs/PRODUCTION_TUNING.md` with the specific API/sockopt changes that would be needed at deployment time. Interviewers care that you know the path; the loopback-vs-`netem` measurement framework is what would let you actually validate the gain once hardware is available.

**4.6 Compiler tuning**
- `-O3 -march=native -flto`: aggressive optimization + link-time optimization
- `-fno-exceptions -fno-rtti`: eliminate exception handling overhead on hot path
- **`-fno-exceptions` + Boost.Beast compatibility**: Beast throws exceptions in some error paths by default. All async operations must use the `error_code` overloads (e.g., `async_read(ws, buffer, yield[ec])` instead of the throwing variant). Audit every Beast/Asio call to ensure no throwing path is reachable — an uncaught exception with `-fno-exceptions` calls `std::abort()`.
- Profile-guided optimization (PGO): compile with `-fprofile-generate`, run against the simulator feed under load, recompile with `-fprofile-use`.

**4.7 Latency measurement**
- `rdtsc` inline assembly for nanosecond timestamps
- Calibrate TSC frequency against `clock_gettime(CLOCK_MONOTONIC)` at startup
- Record timestamps at: WebSocket recv, parse complete, book updated, order sent
- HDR histogram for p50/p99/p999/max percentiles
- Target metrics to report:
  - **Wire-to-parse**: time from socket read to parsed struct
  - **Parse-to-book**: time from parsed struct to book update
  - **Tick-to-order**: time from market data arrival to order submission
  - **Allocator latency**: arena alloc vs malloc comparison

### Phase 5: Exchange Simulator — `kalshi-sim` (Week 6-7)

A conformant Kalshi-protocol server, written in C++ for both correctness and apples-to-apples measurement. The simulator lets the client be benchmarked end-to-end without giving SSN to a regulated exchange, and unlocks adversarial-scenario testing that no real exchange would permit.

**5.1 REST server**
- Boost.Beast HTTPS server on `127.0.0.1:8443` (self-signed cert for TLS).
- Verifies the three `KALSHI-ACCESS-*` headers on every request:
  - Parses `KALSHI-ACCESS-KEY` UUID against the registered set.
  - Reconstructs the signing message `{timestamp}{method}{path}` and verifies `KALSHI-ACCESS-SIGNATURE` against the client's registered public key via `EVP_DigestVerify*`.
  - Rejects with 401 on signature mismatch, ±5 s timestamp skew, or unknown key.
- Implements endpoints: `GET /exchange/status`, `GET /portfolio/balance`, `POST /portfolio/orders`, `DELETE /portfolio/orders/{id}`.

**5.2 WebSocket feed**
- Boost.Beast WebSocket server on `127.0.0.1:8444`.
- Two modes:
  1. **Replay mode**: streams captured Kalshi feed payloads (recorded from public docs examples + simulated extensions) at recorded inter-message intervals.
  2. **Generative mode**: synthetic market-data generator (random walks with controlled volatility, configurable book depth) to drive load testing.
- Honors sequence numbers per subscription; supports `BookSnapshot` on (re)subscribe.

**5.3 Matching engine**
- Price-time priority CLOB, one book per market.
- Uses the same `FlatHashMap` from `core/` for order ID → Order* indexing.
- Limit, IOC, FOK, GTC support; matches against resting book, emits `trade` and `orderbook_delta` to all subscribers.

**5.4 Scenario injection (control channel)**
A separate localhost-only HTTP control endpoint exposes knobs that no real exchange would offer:
- `POST /sim/inject_delay {ms}` — stall responses for N ms.
- `POST /sim/drop_connection` — force-close the client's WS.
- `POST /sim/partial_fill_rate {0.0-1.0}` — fraction of orders to partial-fill.
- `POST /sim/inject_seq_gap` — skip a sequence number to trigger client's gap-detection / snapshot-request path.
- `POST /sim/rate_limit_burst` — simulate 429 throttle.

These let benchmarks measure how the client's reconnection FSM, snapshot-replay logic, and rate-limiter behave under adversarial conditions — work that's both unique and impossible to demonstrate against a live exchange.

**5.5 Simulator-side optimization**
Same low-latency techniques apply on the server side too: CPU pinning, `TCP_NODELAY`, `io_uring`, pool-allocated orders. This is what makes the end-to-end numbers meaningful — both sides are tuned, so the measured tick-to-trade latency reflects the protocol and network stack, not server-side sloppiness.

### Phase 6: Benchmarking + Documentation (Week 7)

**6.1 Micro-benchmarks (Google Benchmark)**
- SPSC queue: ops/sec, RTT latency at various batch sizes
- Arena allocator vs `malloc`: allocation latency histogram
- Pool allocator vs `new`: allocation + deallocation cycle
- `FlatHashMap` vs `std::unordered_map`: lookup/insert/delete at 1K, 10K, 100K, 200K entries
- simdjson vs nlohmann/json: parse latency per message
- Order book update: nanoseconds per `orderbook_delta` apply

**6.2 System benchmarks (against `kalshi-sim`)**
- End-to-end tick-to-order latency under sustained load, reported as p50 / p99 / p999 / max
- Tail-latency profile under each `tc netem` condition (colocated / LAN / lossy WAN)
- Per-knob ablation: which sockopt / kernel-bypass option contributed how many µs
- Latency stability: demonstrate no jitter spikes from malloc/page faults across an N-minute run
- Memory usage: peak RSS, allocation count verification (should be 0 on hot path)
- Adversarial-scenario tests: recovery time after forced WS drop, behavior under injected sequence gap, throttle-storm absorption

**6.3 Documentation**
- Architecture diagram with data flow (client + `kalshi-sim`)
- Latency results table with percentiles, per network profile
- Explanation of each optimization and its measured impact
- `docs/PRODUCTION_TUNING.md`: what would change in real production (kernel-bypass, hardware timestamping, NIC sockopt) with expected gains based on vendor data
- Build and run instructions for both `kalshi-cpp` and `kalshi-sim`

## Key Dependencies

| Library | Purpose | Why this one |
|---------|---------|-------------|
| Boost.Beast | WebSocket + HTTP | Zero-overhead async, integrates with Asio, no hidden allocations |
| Boost.Asio | Async I/O | Industry standard, io_uring backend available |
| OpenSSL | TLS + RSA-PSS signing | Required for Kalshi auth |
| simdjson | JSON parsing | 2-4x faster than alternatives, zero-copy parsing |
| Google Benchmark | Micro-benchmarks | Standard for C++ performance measurement |
| Google Test | Unit tests | Standard for C++ testing |

## Build

**All build and run commands target Linux (WSL2 on Windows).**

```bash
# Prerequisites (Ubuntu 22.04+ / WSL2)
sudo apt install cmake g++-12 libboost-all-dev libssl-dev

# Configure huge pages (optional, for Phase 4)
# Allocate 64 x 2MB huge pages = 128MB
echo 64 | sudo tee /proc/sys/vm/nr_hugepages

# Build
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Run (connect to Kalshi demo)
./kalshi-cpp --demo

# Run benchmarks
./bench/bench_spsc
./bench/bench_allocator

# Run tests
ctest
```

## Compiler Flags (Release)

```cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -fno-exceptions -fno-rtti")
```

## Local Development Setup

Default flow runs entirely against `kalshi-sim` — no Kalshi account, no SSN, no KYC. The client uses a locally generated throwaway RSA keypair; the simulator is registered with the matching public key.

1. Generate a throwaway RSA keypair (outside the repo so it's never committed):
   ```bash
   mkdir -p ~/.kalshi
   openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out ~/.kalshi/dev_private.pem
   openssl rsa -in ~/.kalshi/dev_private.pem -pubout -out ~/.kalshi/dev_public.pem
   chmod 600 ~/.kalshi/dev_private.pem
   ```
2. Generate a dummy access key ID (any UUID works — used to identify the client to `kalshi-sim`):
   ```bash
   uuidgen > ~/.kalshi/dev_key_id
   ```
3. Set env vars (put in `~/.bashrc` for persistence):
   ```bash
   export KALSHI_KEY_ID="$(cat ~/.kalshi/dev_key_id)"
   export KALSHI_KEY_PATH="$HOME/.kalshi/dev_private.pem"
   export KALSHI_API_BASE="http://127.0.0.1:8443"
   export KALSHI_WS_URL="ws://127.0.0.1:8444/trade-api/ws/v2"
   ```
4. Add `.kalshi/`, `*.pem`, `*.key` to `.gitignore` (belt-and-suspenders; keys already live outside the repo).
5. Start the simulator first, then the client:
   ```bash
   # terminal 1 — register the public key with kalshi-sim and start the server
   ./build/kalshi-sim --register ~/.kalshi/dev_public.pem --key-id "$KALSHI_KEY_ID"
   
   # terminal 2 — start the client
   ./build/kalshi-cpp
   ```

To run against real Kalshi instead of `kalshi-sim` (requires Kalshi account with KYC), only the env vars change — set `KALSHI_API_BASE` and `KALSHI_WS_URL` to Kalshi's URLs, point `KALSHI_KEY_PATH` at the key uploaded to Kalshi's dashboard. The C++ code is identical.

**Maintenance window note** (production only): real Kalshi pauses trading every Thursday 3:00-5:00 AM ET. WebSocket connections drop; orders with `cancel_order_on_pause=true` are auto-cancelled. `kalshi-sim`'s `POST /sim/drop_connection` simulates this so we can validate our reconnection FSM without waiting until Thursday.

## Measured Results

Microbenchmarks via Google Benchmark on x86-64 (22-core, 3072 MHz, 24 MB L3). Release build (`-O3 -march=native -flto`). Numbers are mean per-operation latency unless noted.

### SPSC Queue

| Benchmark | Time | vs alternative |
|---|---|---|
| `BM_PushPop` (single-thread, int) | **1.01 ns** | `std::queue` (no thread safety): 1.18 ns |
| `BM_PushPopFat` (single-thread, 108-byte struct) | 8.05 ns | dominated by memcpy of payload |
| `BM_StdQueueMutex_PushPop` (single-thread, no contention) | 30.5 ns | **30× slower than our SPSC** |
| `BM_CrossThread_Throughput` (two threads) | **44 M items/sec** | mutex-protected `std::queue`: 8.9 M items/sec (**5× slower**) |
| `BM_CrossThread_RTT` (ping-pong, two threads) | 348 ns/round-trip | no CPU pinning yet — expected to drop in Phase 4 |

### Pool Allocator

| Benchmark | Time | vs alternative |
|---|---|---|
| `BM_Pool_RealisticUsage` (alloc + field writes + dealloc) | **0.76 ns** | `malloc`/`free`: 8.97 ns (**12× slower**) |
| `BM_Pool_Churning` (1024-order steady-state turnover) | **1.80 ns** | `malloc`/`free`: 7.72 ns (**4× slower**); `std::list` push/pop: 14.9 ns (**8× slower**) |
| `BM_Pool_FillDrain` (sustained throughput) | **776 M ops/sec** | `malloc`/`free`: 163 M ops/sec (**4.8× slower**) |
| `BM_Pool_AllocDealloc` (raw allocate+deallocate) | 0.26 ns | malloc: 7.24 ns; new/delete: 8.55 ns (`Pool_Churning` is the more honest realistic number) |

Pool benchmarks use a 64-byte `Order` struct (one cache line). The pool never touches `malloc` after construction.

### Caveats

- Cross-thread RTT (348 ns) is higher than rigtorp's 133 ns Linux-pinned baseline because we don't yet have CPU pinning, `SCHED_FIFO`, or core isolation. Phase 4 (OS-level tuning) should close this gap.
- Single-op `BM_Pool_AllocDealloc` (0.26 ns) likely reflects compiler optimization of the unused result; the churning number is what to cite externally.
- Google Benchmark's "DEBUG" warning on MinGW/MSYS2 is spurious — Release flags are confirmed applied via `compile_commands.json`.

## Target Resume Bullet

> **kalshi-cpp** | *Low-latency C++ prediction-market trading client + conformant exchange simulator* [GitHub]
> - Built a complete low-latency trading stack in C++20: `kalshi-cpp`, a client implementing Kalshi's REST + WebSocket protocol with RSA-PSS request signing, and `kalshi-sim`, a **conformant exchange simulator** speaking the same protocol — used for reproducible end-to-end benchmarks and adversarial-scenario testing (forced disconnects, sequence-number gaps, partial fills, throttle storms).
> - Lock-free SPSC queues, custom arena/pool/hash-map allocators, custom open-addressing Robin Hood hash table; zero heap allocations on the hot path. End-to-end p99 tick-to-trade latency under X µs measured against `kalshi-sim` with `tc netem`-injected 100 µs RTT (representative of colocated LAN).
> - Custom SPSC queue: 1 ns single-thread push/pop, 44 M items/sec cross-thread throughput — 30× faster than mutex-protected `std::queue` (single-thread, no contention).
> - Custom pool allocator: 1.8 ns per order alloc/dealloc cycle — 4× faster than `malloc`, 8× faster than `std::list`, fully deterministic.
> - Custom Robin Hood `FlatHashMap` (open addressing, backshift deletion, mmap-backed): 2× faster insert/erase and 1.2× faster steady-state churn vs `std::unordered_map`; find within 50% of std with strictly bounded tail latency (no rehash, no per-op allocation) — architecture aligned with Optiver/Jane Street public guidance for HFT containers.
> - Network-stack optimization with measured per-knob impact: `TCP_NODELAY`, `SO_BUSY_POLL`, `TCP_QUICKACK`, `io_uring` backend, CPU pinning, huge pages, `mlockall`. Production deployment path to kernel-bypass NIC (Solarflare ef_vi / DPDK) documented with expected latency gains.

## Kalshi API Quick Reference

| Resource | Endpoint / URL |
|---|---|
| REST (demo) | `https://external-api.demo.kalshi.co/trade-api/v2` |
| REST (prod) | `https://external-api.kalshi.com/trade-api/v2` |
| WebSocket (demo) | `wss://external-api-ws.demo.kalshi.co/trade-api/ws/v2` |
| WebSocket (prod) | `wss://external-api-ws.kalshi.com/trade-api/ws/v2` |
| OpenAPI spec | `https://docs.kalshi.com/openapi.yaml` |
| AsyncAPI spec (WS) | `https://docs.kalshi.com/asyncapi.yaml` |
| Rate limits | Basic: 200 read/100 write tokens/sec (10 tokens/req default) |
| Order types | Limit only; TIF: GTC, IOC, FOK, GTD; modifiers: post_only, reduce_only |
| Matching | CLOB, price-time priority (FIFO) |
| Auth | RSA-PSS SHA-256, per-request signing (no session tokens) |
| FIX | FIXT.1.1/FIX50SP2, Premier tier+, port 8228/8230 (contact institutional@kalshi.com) |

## References

- [Kalshi API Docs](https://docs.kalshi.com/welcome)
- [Kalshi API Environments](https://docs.kalshi.com/getting_started/api_environments)
- [Kalshi Authentication](https://docs.kalshi.com/getting_started/quick_start_authenticated_requests)
- [Kalshi WebSocket](https://docs.kalshi.com/websockets/websocket-connection)
- [Kalshi Orderbook Responses](https://docs.kalshi.com/getting_started/orderbook_responses)
- [Kalshi Rate Limits](https://docs.kalshi.com/getting_started/rate_limits)
- [Kalshi FIX Protocol](https://docs.kalshi.com/fix)
- [Kalshi Demo Environment](https://docs.kalshi.com/getting_started/demo_env)
- [Kalshi Market Lifecycle](https://docs.kalshi.com/getting_started/market_lifecycle)
- [Kalshi OpenAPI Spec](https://docs.kalshi.com/openapi.yaml) (machine-readable, use for endpoint details)
- [Kalshi AsyncAPI Spec](https://docs.kalshi.com/asyncapi.yaml) (machine-readable, use for WS message schemas)
- [rigtorp/SPSCQueue](https://github.com/rigtorp/SPSCQueue) (SPSC benchmark baseline)
- [kalshi-rs](https://github.com/rmadev01/kalshi-rs) (Rust HFT client — architectural reference for lock-free orderbook, integer prices, BTreeMap levels)
- [simdjson](https://github.com/simdjson/simdjson)
- [Boost.Beast](https://www.boost.org/doc/libs/release/libs/beast/)
- [C++ Design Patterns for Low-Latency Applications (arXiv:2309.04259)](https://arxiv.org/pdf/2309.04259)
- [Memory Management in C++ HFT Systems](https://cppforquants.com/memory-management-in-c-high-frequency-trading-systems/)
