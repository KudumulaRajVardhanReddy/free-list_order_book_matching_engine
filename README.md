# Order Book & Matching Engine

A multithreaded order book matching engine implemented in C++20. The system utilizes lock-free concurrency primitives, pre-allocated object pools, and hardware-aware memory design to ensure zero dynamic allocations and ultra-low deterministic latency on the execution path.

## Technical Specifications

### Memory Management

* **FreeList**: A custom object pool allocator. Initializes contiguous memory for order structures at startup. Order insertion and deletion fetch and return pointers to this pool in $O(1)$ time, bypassing OS-level `malloc`/`free` or `new`/`delete` calls during runtime.

### Data Structures & Complexity

* **Order Lookup (Flat Array)**: Maps `OrderId` directly to `Order*` via contiguous array indexing for deterministic $O(1)$ lookup and state verification, replacing standard library hash maps.
* **Price Level Indexing (Hierarchical Bitmask)**: Finding the next best available price level uses a 2-level $O(1)$ hierarchical bitmask. A 64-bit L2 summary mask combined with hardware intrinsics (`__builtin_ctzll` / `tzcnt`) instantly identifies populated price ranges, eliminating worst-case linear scanning across sparse books.
* **Resting Orders (Intrusive Doubly Linked List)**: Each price level maintains an intrusive doubly linked list of `Order` nodes. This permits $O(1)$ order cancellation and removal from the middle of a queue without traversing the list or requiring secondary pointer allocations.
* **Multi-Instrument Routing & Price Offset**: Supports per-`stock_locate` order books with a configurable `base_price` offset so 4-decimal ITCH integer prices (e.g., `$150.0000` = `1,500,000` ticks) map into a compact price window without wasting memory.

### Concurrency & Pipeline Model

* **Protocol Ingestion**: Includes a zero-allocation binary parser for the **NASDAQ ITCH 5.0** protocol using exact 6-byte big-endian timestamps and compile-time `static_assert` layout verification (handling `'A'`/`'F'` Add, `'X'` Partial Cancel, `'D'` Delete, `'E'`/`'C'` Execute, and `'U'` Replace).
* **SPSCRingBuffer<T, Capacity>**: A Single-Producer Single-Consumer lock-free queue passing by-value commands between threads.
* **Cache Alignment**: Utilizes `alignas(64)` on read/write indices to prevent false sharing and destructive interference across CPU cache lines.
* **Thread Decoupling**: A pinned ingest thread parses binary feeds and pushes `EngineCommand` structs across the ring buffer. A pinned engine thread consumes these commands and modifies the book sequentially.

## Build Instructions

The project uses CMake and requires a compiler supporting C++20.

```bash
# Generate build files
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..

# Compile the application and benchmarks
make -j\$(nproc)
```

## Full-Day NASDAQ ITCH 5.0 Replay Verification

To validate end-to-end book-building correctness against real market data, the engine replays historical binary NASDAQ TotalView-ITCH 5.0 day files (`12302019.NASDAQ_ITCH50`), resolving `stock_locate` via `'R'` (*Stock Directory*) messages, dynamically anchoring `base_price` on initial order ingress, and asserting book invariants every 10,000 events (`best_bid < best_ask`, level volume/count sums, and order lookup consistency).

```bash
./itch_day_replay ../data/12302019.NASDAQ_ITCH50 AAPL
```

| Replay Metric | Verified Result (`AAPL`, `stock_locate = 13`) |
| :--- | :--- |
| **Total File Messages Parsed** | 268,744,780 |
| **Target Symbol Events Replayed** | 1,512,179 (`698,744` Adds, `60,543` Executes, `1,467` Partial Cancels, `654,457` Deletes, `96,968` Replaces) |
| **Wall-Clock Replay Time** | 70.74 s (~3.80 M msgs/sec) |
| **Invariant Checkpoints Passed** | 152 / 152 (`Final: PASS`) |
| **Drift Crossings Detected** | 0 |
| **End-of-Day Resting Orders** | 0 (matches NASDAQ session close state) |

## Benchmark Results

The benchmarking suite isolates both the core execution cost and the cross-core pipeline latency. Measurements bypass OS timer quantization by utilizing the hardware timestamp counter (`rdtsc`) to record elapsed CPU cycles for **every single operation** individually, exposing true tail-latency distributions instead of hiding spikes inside batched averages.

Benchmarks are run across isolated P-cores using `taskset -c 2,4` with a tight spin (no `_mm_pause()`) to measure inter-core cache transfer (MESI invalidation) without queue depth saturation (head-of-line blocking).

### Environment

* **Compiler**: GCC 16.2.1 (Linux)
* **Target**: Release (`-O3`, `-march=native`)
* **OS**: Linux x86_64
* **Measurement**: `rdtsc` at Invariant TSC rate **`2419.200 MHz` (`2.4192 GHz`)** verified via `dmesg | grep -i tsc`

### 1. Consumer Service Latency (Core Execution)

Measures the exact execution cycles of the engine thread popping a command from the L1 cache, routing by `stock_locate`, and updating the book + L2 bitmask state.

| Metric | CPU Cycles | Exact Time (@ 2.4192 GHz TSC) |
| --- | --- | --- |
| **Avg** | 67.79 | ~28.0 ns |
| **p50 (Median)** | 59 | ~24.4 ns |
| **p99** | 116 | ~47.9 ns |
| **Max Spike** | 8,540 | ~3.53 µs |

### 2. Un-backlogged One-Way Pipeline Latency (Wire-to-Book)

Measures the one-way traversal from ingest start to book update completion: Binary ITCH Parse $\to$ SPSC Push on Core 2 $\to$ L3 Interconnect MESI Transfer $\to$ Book Update on Core 4 (measured prior to producer ACK).

| Metric | CPU Cycles | Exact Time (@ 2.4192 GHz TSC) |
| --- | --- | --- |
| **p50 (Median)** | 1,087 | ~449.3 ns |
| **p90** | 1,289 | ~532.8 ns |
| **p99** | 1,546 | ~639.1 ns |
| **p99.9** | 1,827 | ~755.2 ns |

> **Note:** The stopwatch stops immediately after `OrderBook` completes the update on Core 4 (before `completed_ack` returns to Core 2), so this reflects true one-way wire-to-book latency.

### 3. Bare-Metal Linux Hardware Profiling (`perf stat`)
Captured using Linux `perf` pinned to physical P-cores via `taskset -c 2,4`:

| Hardware Performance Counter | Measured Event Count | Technical Significance |
| :--- | :--- | :--- |
| **P-Core Execution (`cpu_core/cycles`)** | 543,780,977 | 100% execution pinned to Performance Cores |
| **E-Core Execution (`cpu_atom/cycles`)** | 0 (`<not counted>`) | Confirms zero thread migration to Efficiency Cores |
| **L1 Data Cache Misses** | 907,712 | ~9 L1 misses/msg (dominated by SPSC slot transfer) |
| **Branch Misprediction Rate** | 2.34% (539,898 / 23.03M) | High branch predictor accuracy on matching hot-paths |

---

## What I improved from earlier iterations

1. **Eliminated Linear Price Scanning:**
   * *Before:* Scanning for the next available price in a sparse limit order book relied on linear iteration, resulting in worst-case unbounded latency spikes.
   * *After:* Implemented a two-level hierarchical bitmask. Hardware bit-scan-forward instructions (`tzcnt`) now locate the next populated price level in deterministic $O(1)$ time.

2. **Fixed Benchmark Methodology (Eliminated Batched Averages & Corrected TSC Rate):**
   * *Before:* Measured latency by timing batches of 1,000,000 operations and dividing by $N$, and converted cycles using turbo frequency (~5.0 GHz).
   * *After:* Transitioned to single-operation `rdtsc` timestamping and converted ticks using the hardware Invariant TSC rate (`2419.200 MHz` from `dmesg`), exposing true $p50 \to Max$ distributions.

3. **True Pipeline Flow Control & One-Way Latency Measurement:**
   * *Before:* Producers slammed the lock-free queue instantly (measuring queue backlog), and `_mm_pause()` stalled modern Intel P-cores during spin-waits.
   * *After:* Implemented paced, tight-spin pipeline benchmarking using binary NASDAQ ITCH 5.0 payloads and accurately labeled the pre-ACK measurement as one-way latency.

4. **ITCH 5.0 Wire Layout, Full Lifecycle Semantics & Full-Day Book Replay:**
   * *Before:* Used 8-byte timestamps (shifting wire fields by 2 bytes), treated `'X'` as a full delete, ignored `'E'`/`'C'`/`'U'`, left `PriceLevel::total_volume_` stale on partial fills, and capped prices at `200000` (`$20.0000`).
   * *After:* Fixed 6-byte ITCH timestamps (`static_assert` verified), implemented partial cancel (`'X'`), execution (`'E'`/`'C'`), and replace (`'U'`), synced `PriceLevel::total_volume_` on every partial fill, added `base_price` offset + `stock_locate` book routing, and verified 152/152 invariant checkpoints with 0 drift crossings across a full 268.7M-message NASDAQ ITCH 5.0 day file (`12302019.NASDAQ_ITCH50`, 1.51M `AAPL` events).
