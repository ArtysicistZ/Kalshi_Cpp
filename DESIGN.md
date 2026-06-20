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

## Implementation Progress

Snapshot of what's actually built vs. designed. Updated as commits land.

| Area | Status | Notes |
|---|---|---|
| `src/core/spsc_queue` | DONE | 1.01 ns push/pop, 44 M items/sec cross-thread |
| `src/core/pool_alloc` | DONE | 1.8 ns realistic alloc/dealloc, 776 M ops/sec fill/drain |
| `src/core/arena_alloc` | DONE | bench pending |
| `src/core/flat_hash_map` | DONE | Robin Hood, 524 288 capacity default |
| `src/core/clock` | DONE | rdtsc + calibration |
| `src/core/json_io` | scaffolded | shared JSON helpers; simdjson integration pending CMake hookup |
| `src/net/auth` | scaffolded | RSA-PSS signing API in place |
| `src/net/rest_client` | scaffolded | round-trip test exists (`test_rest_client.cpp`) |
| `src/net/ws_client` + `ws_reconnect` | scaffolded | reconnection FSM design only |
| `src/net/rate_limiter` | scaffolded | header only, no impl yet |
| `src/feed/parser`, `src/feed/book` | scaffolded | |
| `src/strategy/*`, `src/system/tuning` | scaffolded | |
| `sim/domain/matching_engine` | DONE | price-time priority CLOB, covered by `test_matching_engine.cpp` |
| `sim/domain/market_registry` | in progress | Block 1 |
| `sim/domain/account_book` | in progress | Block 2 |
| `sim/domain/types.h` | empty | to populate with Side / OrderId / ClientId / Price / Qty / Ticker / Fill |
| `sim/services/exchange_service` | in progress | Block 2 |
| `sim/http/rest_server` | DONE | custom epoll HTTP/1.1, no TLS |
| `sim/http/handlers` | in progress | Block 1 — order/orderbook/cancel endpoints |
| `sim/http/auth_middleware` | in progress | Block 3 |
| `sim/auth/auth_verify` | scaffolded | Block 3 |
| `sim/ws/*` | scaffolded | Block 4 |
| `sim/scenario/*` | scaffolded | Block 5 |
| `simdjson` in `CMakeLists.txt` | not yet | `FetchContent_Declare` is commented out; uncomment when `json_io` needs it |
| `docs/PRODUCTION_TUNING.md`, `docs/BENCHMARK_RESULTS.md` | not yet created | |

**Implementation block sequence (parallel to Phase numbering, but pedagogy-driven):**

- **Block 1** — wire matching engine into REST server (populate `types.h`, finish `market_registry`, write `http/handlers` for POST order / GET orderbook / DELETE order). Builds JSON parsing, schema-validation, and composition-root concepts.
- **Block 2** — `account_book` + `exchange_service`: introduce the service layer and exchange-wide invariants (balance ≥ 0, position bookkeeping, fill fan-out).
- **Block 3** — `http/auth_middleware` + `auth/auth_verify`: RSA-PSS verification end-to-end, KALSHI-ACCESS-* header semantics.
- **Block 4** — `ws/`: WebSocket handshake (reusing `auth/`), subscription state, snapshot + delta fan-out from `exchange_service`.
- **Block 5** — `scenario/`: control endpoint for delay/drops/partial fills/seq gaps; exercise the client's recovery paths.
- **Block 6** — TCP / Wireshark deep dive against the running sim (kernel-level investigation, not new features).
- **Block 7** — MPSC refactor where the matching engine becomes a single-consumer with multiple producing transports.

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
│   ├── core/                       # Low-latency primitives + shared utilities
│   │   ├── spsc_queue.h                # Lock-free SPSC ring buffer [DONE]
│   │   ├── arena_alloc.h               # Arena (bump) allocator [DONE]
│   │   ├── pool_alloc.h                # Fixed-size pool allocator [DONE]
│   │   ├── flat_hash_map.h             # Robin Hood open-addressing hash map [DONE]
│   │   ├── clock.h                     # rdtsc + calibration [DONE]
│   │   └── json_io.h/cpp               # Project-wide JSON helpers (used by client + sim)
│   ├── strategy/                   # Trading logic
│   │   ├── signal.h/cpp                # Signal generation
│   │   └── order_manager.h/cpp         # Order lifecycle
│   ├── system/                     # OS-level optimization
│   │   └── tuning.h/cpp                # CPU pin, huge pages, mlockall, SCHED_FIFO
│   └── util/
│       ├── log.h                       # Lock-free logging
│       └── histogram.h                 # Latency percentile tracking (HDR)
├── sim/                            # Exchange simulator `kalshi-sim` (Phase 5)
│   ├── main.cpp                        # Server entrypoint, composition root (wires deps)
│   ├── domain/                         # Pure business logic — no I/O, no protocol, no JSON
│   │   ├── types.h                         # Side, OrderId, ClientId, Price, Qty, Ticker, Fill
│   │   ├── matching_engine.h/cpp           # CLOB, price-time priority [DONE]
│   │   ├── market_registry.h/cpp           # Owns per-ticker MatchingEngine instances (unique_ptr for pointer stability)
│   │   └── account_book.h/cpp              # Per-client balance + position bookkeeping
│   ├── services/                       # Orchestration — composes domain calls, threads cross-cutting rules
│   │   └── exchange_service.h/cpp          # POST order → engine.match → account.apply → fan-out fills
│   ├── http/                           # HTTP transport adapter
│   │   ├── rest_server.h/cpp               # Custom epoll-based HTTP/1.1 server (raw sockets, no Beast, no TLS yet) [DONE]
│   │   ├── handlers.h/cpp                  # Request → ExchangeService call → Response (JSON serialization lives here)
│   │   └── auth_middleware.h/cpp           # HTTP adapter for auth/: parses KALSHI-* headers, calls auth_verify, threads ClientId into Request
│   ├── ws/                             # WebSocket transport adapter (Phase 5.2)
│   │   ├── ws_server.h/cpp                 # WS feed (replay + generative modes)
│   │   └── ws_handlers.h/cpp               # subscribe / unsubscribe / snapshot dispatch
│   ├── auth/                           # Protocol-agnostic crypto — reused by http/ and ws/
│   │   └── auth_verify.h/cpp               # EVP_DigestVerify against registered pubkeys
│   ├── scenario/                       # Adversarial-scenario control endpoint
│   │   └── scenario.h/cpp                  # /sim/* knobs (delay, drops, partial fills, seq gaps, throttle)
│   └── replay/                         # Captured Kalshi payload samples
│       └── *.json
├── bench/                          # Micro-benchmarks
│   ├── bench_spsc.cpp                  # DONE
│   ├── bench_pool.cpp                  # DONE
│   ├── bench_flat_hash_map.cpp         # DONE
│   ├── bench_arena.cpp                 # (planned)
│   ├── bench_parser.cpp                # simdjson vs nlohmann (planned)
│   ├── bench_book.cpp                  # orderbook_delta apply latency (planned)
│   └── bench_e2e.cpp                   # tick-to-trade against kalshi-sim under tc netem (planned)
├── test/                           # Unit tests
│   ├── test_spsc.cpp                   # DONE
│   ├── test_pool.cpp                   # DONE
│   ├── test_flat_hash_map.cpp          # DONE
│   ├── test_rest_client.cpp            # DONE — REST client round-trip
│   ├── test_matching_engine.cpp        # DONE — kalshi-sim CLOB correctness
│   ├── test_auth.cpp                   # Self-test + Kalshi reference vector (planned, Phase 1.2)
│   ├── test_book.cpp                   # (planned, Phase 3.1)
│   └── test_parser.cpp                 # (planned, Phase 1.3)
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

**Architectural layering (sim/)**

The simulator is intentionally split into a strict DAG of layers so that adding WS (Phase 5.2), TLS, or alternative transports doesn't ripple through unrelated code:

```
domain/   ← pure business logic; depends on nothing else in sim/
  ↑
services/ ← orchestrates domain objects; threads cross-cutting policies (auth identity, fan-out)
  ↑
http/  ws/  scenario/   ← transport adapters; turn bytes into a service call and back
  ↑
auth/  ← protocol-agnostic crypto; called from http/auth_middleware and ws/ handshake
```

- `domain/` contains the matching engine, market registry, account book, and the shared `types.h`. No JSON, no sockets, no Boost. Tested in isolation by `test_matching_engine.cpp`.
- `services/exchange_service` is the *only* place where an order-placement call composes engine matching with account bookkeeping and downstream notification — preventing transports from acquiring direct write access to domain objects.
- `http/` is a transport adapter: `rest_server` knows about sockets and HTTP/1.1; `handlers` knows about JSON and the service API; `auth_middleware` is the HTTP-side adapter for `auth/` (parses headers, calls `auth_verify`, stamps `Request.authenticated_client_id`).
- `auth/auth_verify` is deliberately protocol-agnostic so the WS handshake (Phase 5.2) can reuse it without depending on HTTP types.
- `main.cpp` is the composition root: it constructs the registries, the service, wires middleware around the server, and registers routes. No other file knows the full object graph.

**5.1 REST server**
- Custom **epoll-based HTTP/1.1 server** on `127.0.0.1:8443` — raw `socket`/`bind`/`listen`/`accept` plus `epoll_wait`. No Boost.Beast, no TLS in the current cut. This is a deliberate teaching choice: walking the kernel-level path (file descriptors, `epoll_ctl`, partial reads, per-connection buffers, write-readiness) is part of the Phase-5 systems-deep-dive value of the simulator.
- `RestServer` exposes a tiny core: `Request { method, path, headers, body, optional<ClientId> authenticated_client_id }`, `Response { status, content_type, body }`, and `using Handler = std::function<Response(const Request&)>`. Exact-match routing keyed by `method + " " + path`.
- TLS deferred — the design parity argument (matching Kalshi's wire surface byte-for-byte) is preserved through `auth/` and the handler set; adding TLS is purely a `rest_server` upgrade later (either by switching to Beast + OpenSSL or by adding a manual `SSL_*` wrap around the socket).
- `http/auth_middleware` verifies the three `KALSHI-ACCESS-*` headers on every request:
  - Parses `KALSHI-ACCESS-KEY` UUID against the registered set.
  - Reconstructs the signing message `{timestamp}{method}{path}` and verifies `KALSHI-ACCESS-SIGNATURE` against the client's registered public key via `EVP_DigestVerify*` (delegated to `auth/auth_verify`).
  - Rejects with 401 on signature mismatch, ±5 s timestamp skew, or unknown key.
  - On success, stamps `Request.authenticated_client_id` so downstream handlers don't re-parse headers.
- Endpoints (implemented in `http/handlers`): `GET /exchange/status`, `GET /portfolio/balance`, `POST /portfolio/orders`, `DELETE /portfolio/orders/{id}`, and orderbook reads.

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

### Hot Path Pipeline (parse → orderbook → signal → reconcile → serialize)

Two benches measure the application-internal compute path. Both rotate over 1024 distinct synthetic orderbook-delta JSONs (xorshift64, fixed seed) and enforce zero-allocation in the measurement window via a global `new`/`delete` interposer (a counter bumps on any `malloc` during the timed loop; PASS = 0 bumps across all threads).

- **`bench_hotpath`** (single-thread, 100 K iterations): full pipeline on one thread bracketed by `rdtsc()` / `rdtscp()`. Validates the per-stage compute floor with hot cache.
- **`bench_hotpath_pipe`** (two-thread SPSC, 1 M iterations, single ticker): producer parses then pushes a 72-byte `{OrderbookDelta, produce_tsc}` message over the existing `SPSCQueue`; consumer pops then applies → signals → reconciles → serializes; per-message latency = consumer's `rdtscp` minus producer's `rdtsc`. Producer and consumer pinned to distinct physical cores via `pthread_setaffinity_np`. `SCHED_FIFO` (priority 50) and `mlockall(MCL_CURRENT | MCL_FUTURE)` applied when CAP_SYS_NICE / CAP_IPC_LOCK granted; otherwise warn-and-continue.
- **`bench_hotpath_multi`** (two-thread SPSC, 1 M iterations, 256 tickers): same pipeline as `bench_hotpath_pipe`, but registers 256 distinct ticker symbols `MKT-000`..`MKT-255`, pre-allocates one `MarketState` (Book + Signal + OrderManager) per ticker from a `Pool<MarketState, 256>`, and stores them in a `FlatHashMap<TickerKey, MarketState*, 1024>` keyed on FNV-1a 64-bit hashes of the ticker bytes. Every message hashes the ticker, looks up the corresponding `MarketState*`, and dispatches into that market's book/signal/OM. Exercises the FlatHashMap and Pool primitives on the critical path; zero-alloc enforcement now spans **three** composed primitives (SPSC + FlatHashMap + Pool).

| Bench | Platform | min | p50 | p90 | p99 | p999 | max | throughput | zero-alloc |
|---|---|---:|---:|---:|---:|---:|---:|---:|:---:|
| `bench_hotpath` | WSL2 (i7-1370P) | 249 ns | **318 ns** | 399 ns | 618 ns | 1.02 µs | 186 µs | — | ✓ |
| `bench_hotpath_pipe` (SCHED_FIFO, cores 16/18) | WSL2 (i7-1370P) | 282 ns | 571 ns | 720 ns | 94 µs | 348 µs | 533 µs | 2.92 M/s | ✓ |
| `bench_hotpath_pipe` (SCHED_OTHER, cores 16/18 same-CCX, 1 ticker) | Azure VM (EPYC 7V12) | 395 ns | 466 ns | 486 ns | 526 ns | 16.2 µs | 105 µs | 3.15 M/s | ✓ |
| `bench_hotpath_multi` (SCHED_OTHER, cores 16/18 same-CCX, 256 tickers) | Azure VM (EPYC 7V12) | 435 ns | **486 ns** | **536 ns** | **576 ns** | **3.00 µs** | 81 µs | **3.18 M/s** | ✓ (3 primitives) |

Notes:
- Two-thread p99 collapses from 94 µs (WSL2 hypervisor-bounded) to 526 ns on a dedicated VM — the WSL2 tail was Hyper-V scheduler preemption, not pipeline jitter.
- EPYC same-CCX (shared L3) pairing chosen via `lscpu --extended` to minimize cross-core handoff cost (~30 cycles L3-to-L1d migration vs ~80+ for cross-CCX).
- 99.7 % of the 1 M single-ticker EPYC iterations fell within a single 419–837 ns histogram bucket; the residual tail (kernel timer ticks) would tighten further with `isolcpus` / `nohz_full` on a bare-metal box.
- **Multi-ticker dispatch cost is small and predictable**: +20 ns at p50 (466 → 486 ns), +50 ns at p99 (526 → 576 ns) for one FNV-1a hash of a 32-byte ticker plus one Robin Hood probe at ~25 % load factor. 99.8 % (998 052) of multi-ticker iterations landed in the same 419–837 ns bucket as single-ticker. The dispatch is *not* on the critical-path bottleneck; throughput slightly improved (3.15 → 3.18 M msg/s).
- **Zero-allocation enforcement now covers three composed primitives**: SPSC queue + FlatHashMap dispatch + Pool-backed `MarketState`. The global `new`/`delete` interposer counter stayed at 0 across all 1 M iterations on both producer and consumer threads.

### Caveats

- Cross-thread RTT (348 ns) is higher than rigtorp's 133 ns Linux-pinned baseline because the SPSC microbench was run without pinning. The hot-path pipeline bench (above) uses `pthread_setaffinity_np` and shows the cross-core handoff is not the bottleneck in the full path.
- Single-op `BM_Pool_AllocDealloc` (0.26 ns) likely reflects compiler optimization of the unused result; the churning number is what to cite externally.
- Google Benchmark's "DEBUG" warning on MinGW/MSYS2 is spurious — Release flags are confirmed applied via `compile_commands.json`.
- Hot-path numbers measure **application-internal compute latency** (parsed JSON in → wire bytes out), not tick-to-trade. Production tick-to-trade adds NIC↔userspace traversal (~1–5 µs with kernel-bypass, ~10–20 µs with TCP fast path), exchange RTT (sub-µs colocated, double-digit µs at LAN distance), and is what real HFT shops report with hardware-timestamped NICs.
- Hardware-counter introspection (per-iteration cycles/IPC/cache-miss/branch-mispredict via `perf_event_open`) was deferred: deepx-3's `perf_event_paranoid=4` blocks userspace perf collection without root, and WSL2's PMU exposure under Hyper-V is uneven.

## Target Resume Bullet

> **kalshi-cpp** | *Low-latency C++ prediction-market trading client + conformant exchange simulator* [GitHub]
> - Built a complete low-latency trading stack in C++20: `kalshi-cpp`, a client implementing Kalshi's REST + WebSocket protocol with RSA-PSS request signing, and `kalshi-sim`, a **conformant exchange simulator** speaking the same protocol — used for reproducible end-to-end benchmarks and adversarial-scenario testing (forced disconnects, sequence-number gaps, partial fills, throttle storms).
> - Lock-free SPSC queues, custom arena/pool/hash-map allocators, custom open-addressing Robin Hood hash table; zero heap allocations on the hot path, enforced via a global `new`/`delete` interposer that asserts zero `malloc` calls across all threads over the timed window.
> - Zero-allocation cross-thread hot path (parse → FlatHashMap dispatch over 256 tickers → Pool-backed Book + Signal + OrderManager → JSON serialize): **486 ns p50 / 536 ns p90 / 576 ns p99 / 3.18 M msgs/sec** end-to-end latency measured via `rdtsc`/`rdtscp` over 1 M iterations on AMD EPYC 7V12, with producer and consumer pinned to distinct physical cores on a shared-L3 CCX (`pthread_setaffinity_np`, `SCHED_FIFO`-ready, `mlockall`). Zero-alloc enforcement spans three composed primitives — SPSC queue, FlatHashMap, Pool — verified via a global `new`/`delete` interposer (counter = 0 across all 1 M iterations on both threads).
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
