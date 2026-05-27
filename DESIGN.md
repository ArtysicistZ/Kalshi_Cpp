# kalshi-cpp

Low-latency prediction market trading client in C++20. Connects to Kalshi's WebSocket and REST APIs with lock-free inter-thread communication, custom memory allocators, and OS-level performance tuning. Zero heap allocation on the hot path.

## Why This Exists

There is no C++ client for any prediction market. Every existing Kalshi/Polymarket client is Python, TypeScript, or Go. This project fills that gap while demonstrating the systems programming techniques used in production trading infrastructure: async networking, lock-free queues, arena/pool allocators, CPU pinning, and nanosecond-resolution latency measurement.

## Architecture

```
                    ┌─────────────────────────────────────────────┐
                    │              kalshi-cpp                      │
                    │                                             │
  Kalshi WSS ──────>│  ┌──────────┐  SPSC Queue  ┌────────────┐  │
                    │  │ Network  │ ────────────> │  Strategy  │  │
  Kalshi REST <────>│  │ Thread   │              │  Thread    │  │
                    │  │          │  SPSC Queue  │            │  │
                    │  │ Asio I/O │ <──────────── │ Order Mgr  │  │
                    │  │ (Note 1) │              │ + Book     │  │
                    │  └──────────┘              └────────────┘  │
                    │       │                          │         │
                    │       v                          v         │
                    │  ┌──────────────────────────────────────┐  │
                    │  │         Latency Logger (rdtsc)       │  │
                    │  │    tick-to-process, tick-to-order     │  │
                    │  │    p50 / p99 / p999 histograms        │  │
                    │  └──────────────────────────────────────┘  │
                    │                                             │
                    │  Memory: Arena (msg parse) + Pool (orders)  │
                    │  OS: CPU pin, huge pages, mlockall, FIFO    │
                    └─────────────────────────────────────────────┘

Note 1: Boost.Asio abstracts the I/O backend. On Linux: epoll (default) or
io_uring (Asio 1.80+/Boost 1.80+ with BOOST_ASIO_HAS_IO_URING). On Windows:
IOCP. The code is identical — Asio selects the best backend at compile time.
```

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
├── src/
│   ├── main.cpp
│   ├── net/                  # Networking layer
│   │   ├── ws_client.h/cpp       # WebSocket client (Boost.Beast)
│   │   ├── ws_reconnect.h/cpp    # Reconnection state machine + backoff
│   │   ├── rest_client.h/cpp     # REST client (order placement)
│   │   ├── rate_limiter.h        # Token-bucket rate limiter (mirrors Kalshi tiers)
│   │   └── auth.h/cpp            # RSA-PSS SHA256 signing
│   ├── feed/                 # Market data processing
│   │   ├── parser.h/cpp          # JSON -> internal structs
│   │   └── book.h/cpp            # Order book reconstruction
│   ├── core/                 # Low-latency primitives
│   │   ├── spsc_queue.h          # Lock-free SPSC ring buffer
│   │   ├── arena_alloc.h         # Arena (bump) allocator
│   │   ├── pool_alloc.h          # Fixed-size pool allocator
│   │   ├── flat_hash_map.h       # Open-addressing fixed-capacity hash map
│   │   └── clock.h               # rdtsc + calibration
│   ├── strategy/             # Trading logic
│   │   ├── signal.h/cpp          # Signal generation
│   │   └── order_manager.h/cpp   # Order lifecycle
│   ├── system/               # OS-level optimization
│   │   └── tuning.h/cpp          # CPU pin, huge pages, mlockall
│   └── util/
│       ├── log.h                 # Lock-free logging
│       └── histogram.h           # Latency percentile tracking
├── bench/                    # Micro-benchmarks
│   ├── bench_spsc.cpp
│   ├── bench_allocator.cpp
│   └── bench_parser.cpp
├── test/                     # Unit tests
│   ├── test_spsc.cpp
│   ├── test_book.cpp
│   └── test_auth.cpp
└── scripts/
    └── isolate_cpus.sh           # CPU isolation helper
```

**1.2 Kalshi auth + REST client**
- RSA-PSS SHA256 signature generation using OpenSSL EVP API
- Signature message format: `{timestamp_ms}{HTTP_METHOD}{path}` (path only, no host or query params)
- Three headers per request: `KALSHI-ACCESS-KEY`, `KALSHI-ACCESS-TIMESTAMP` (ms), `KALSHI-ACCESS-SIGNATURE` (Base64)
- REST client for account info, market listing, order placement
- Base URLs:
  - Demo: `https://external-api.demo.kalshi.co/trade-api/v2`
  - Production: `https://external-api.kalshi.com/trade-api/v2`
- Target: successfully authenticate and fetch market list from Kalshi demo API

**1.3 WebSocket client**
- Boost.Beast WebSocket with TLS (Boost.Asio + OpenSSL)
- `TCP_NODELAY` enabled
- Auth headers (`KALSHI-ACCESS-KEY/TIMESTAMP/SIGNATURE`) sent during HTTP upgrade handshake; signed path is always `GET/trade-api/ws/v2`
- WebSocket URLs:
  - Demo: `wss://external-api-ws.demo.kalshi.co/trade-api/ws/v2`
  - Production: `wss://external-api-ws.kalshi.com/trade-api/ws/v2`
- Subscribe to `orderbook_delta`, `ticker`, `trade` channels
- Parse incoming JSON with simdjson (2-4x faster than nlohmann/json)
- Target: print live market data from Kalshi demo

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
  - Inherits from `std::pmr::memory_resource` so it can plug into any STL container that wants pool-backed allocations.
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

### Phase 4: OS-Level Optimization (Week 5-6)

**4.1 Thread tuning**
- `sched_setaffinity()`: pin network thread to core 1, strategy thread to core 2. Avoid core 0 (handles hardware interrupts by default on most Linux kernels).
- `sched_setscheduler(SCHED_FIFO)`: real-time priority to avoid preemption. Requires `CAP_SYS_NICE` or root. If the thread has a bug and never yields (blocks on I/O or sleeps), it will starve other processes on that core.
- `mlockall(MCL_CURRENT | MCL_FUTURE)`: prevent page faults after startup. Without this, rarely-accessed pages can be swapped out; re-accessing them incurs ~10-100us page fault latency.

**4.2 Memory tuning**
- Huge pages via `mmap(MAP_HUGETLB)` for arena and pool backing memory
- `madvise(MADV_HUGEPAGE)` for transparent huge pages on the order book array
- Pre-fault all pages at startup (touch every page to avoid runtime faults)

**4.3 Compiler tuning**
- `-O3 -march=native -flto`: aggressive optimization + link-time optimization
- `-fno-exceptions -fno-rtti`: eliminate exception handling overhead on hot path
- **`-fno-exceptions` + Boost.Beast compatibility**: Beast throws exceptions in some error paths by default. All async operations must use the `error_code` overloads (e.g., `async_read(ws, buffer, yield[ec])` instead of the throwing variant). Audit every Beast/Asio call to ensure no throwing path is reachable — an uncaught exception with `-fno-exceptions` calls `std::abort()`.
- Profile-guided optimization (PGO): compile with `-fprofile-generate`, run against live demo feed, recompile with `-fprofile-use`

**4.4 Latency measurement**
- `rdtsc` inline assembly for nanosecond timestamps
- Calibrate TSC frequency against `clock_gettime(CLOCK_MONOTONIC)` at startup
- Record timestamps at: WebSocket recv, parse complete, book updated, order sent
- HDR histogram for p50/p99/p999/max percentiles
- Target metrics to report:
  - **Wire-to-parse**: time from socket read to parsed struct
  - **Parse-to-book**: time from parsed struct to book update
  - **Tick-to-order**: time from market data arrival to order submission
  - **Allocator latency**: arena alloc vs malloc comparison

### Phase 5: Benchmarking + Documentation (Week 6)

**5.1 Micro-benchmarks (Google Benchmark)**
- SPSC queue: ops/sec, RTT latency at various batch sizes
- Arena allocator vs `malloc`: allocation latency histogram
- Pool allocator vs `new`: allocation + deallocation cycle
- `FlatHashMap` vs `std::unordered_map`: lookup/insert/delete at 1K, 10K, 100K, 200K entries
- simdjson vs nlohmann/json: parse latency per message
- Order book update: nanoseconds per `orderbook_delta` apply

**5.2 System benchmarks**
- End-to-end tick-to-order latency under sustained load
- Latency stability: demonstrate no jitter spikes from malloc/page faults
- Memory usage: peak RSS, allocation count verification (should be 0 on hot path)

**5.3 Documentation**
- Architecture diagram with data flow
- Latency results table with percentiles
- Explanation of each optimization and its measured impact
- Build and run instructions

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

## Kalshi Demo Setup

Demo and production are fully isolated — keys and accounts do not cross environments.

1. Create account at [demo.kalshi.co](https://demo.kalshi.co)
2. Generate RSA key pair:
   ```bash
   openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:4096 -out kalshi_private.pem
   ```
3. Extract public key:
   ```bash
   openssl rsa -in kalshi_private.pem -pubout -out kalshi_public.pem
   ```
4. Upload public key in the Kalshi demo dashboard (Account > API Keys)
5. Copy the API Key ID (UUID) from the dashboard
6. Set env vars:
   ```bash
   export KALSHI_API_KEY="your-api-key-uuid"
   export KALSHI_PRIVATE_KEY_PATH="./kalshi_private.pem"
   ```
7. Run: `./kalshi-cpp --demo`

**Maintenance window**: Kalshi pauses trading every Thursday 3:00-5:00 AM ET. WebSocket connections may drop. Orders with `cancel_order_on_pause=true` are auto-cancelled.

## Target Resume Bullet

> **kalshi-cpp** | *Low-latency C++ prediction market trading client* [GitHub]
> - Built a C++ trading client connecting to Kalshi WebSocket and REST APIs with lock-free SPSC queues, custom arena/pool allocators, and OS-level tuning (CPU pinning, huge pages, mlockall); zero heap allocation on hot path, p99 tick-to-order latency under X us.

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
