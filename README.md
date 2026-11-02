# Order Book & Matching Engine

A multithreaded order book matching engine implemented in C++20. The system utilizes lock-free concurrency primitives, pre-allocated object pools, and hardware-aware memory design to ensure zero dynamic allocations and ultra-low deterministic latency on the execution path.

## Technical Specifications

### Memory Management

* **FreeList**: A custom object pool allocator. Initializes contiguous memory for order structures at startup. Order insertion and deletion fetch and return pointers to this pool in $O(1)$ time, bypassing OS-level `malloc`/`free` or `new`/`delete` calls during runtime.

### Data Structures & Complexity

* **Order Lookup (Flat Array)**: Maps `OrderId` directly to `Order*` via contiguous array indexing for deterministic $O(1)$ lookup and state verification, replacing standard library hash maps.
* **Price Level Indexing (Hierarchical Bitmask)**: Finding the next best available price level uses a 2-level $O(1)$ hierarchical bitmask. A 64-bit L2 summary mask combined with hardware intrinsics (`__builtin_ctzll` / `tzcnt`) instantly identifies populated price ranges, eliminating worst-case linear scanning across sparse books.
* **Resting Orders (Intrusive Doubly Linked List)**: Each price level maintains an intrusive doubly linked list of `Order` nodes. This permits $O(1)$ order cancellation and removal from the middle of a queue without traversing the list or requiring secondary pointer allocations.

### Concurrency & Pipeline Model

* **Protocol Ingestion**: Includes a zero-allocation binary parser for the **NASDAQ ITCH 5.0** protocol (handling Add, Cancel, Execute messages).
* **SPSCRingBuffer<T, Capacity>**: A Single-Producer Single-Consumer lock-free queue passing by-value commands between threads.
* **Cache Alignment**: Utilizes `alignas(64)` on read/write indices to prevent false sharing and destructive interference across CPU cache lines.
* **Thread Decoupling**: A pinned ingest thread parses binary feeds and pushes `OrderCommand` structs across the ring buffer. A pinned engine thread consumes these commands and modifies the book sequentially.

## Build Instructions

The project uses CMake and requires a compiler supporting C++20.

```bash
# Generate build files
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..

# Compile the application and benchmarks
make -j$(nproc)
```

## Benchmark Results

The benchmarking suite isolates both the core execution cost and the cross-core pipeline latency. Measurements bypass OS timer quantization by utilizing the hardware timestamp counter (`rdtsc`) to record elapsed CPU cycles for **every single operation** individually, exposing true tail-latency distributions instead of hiding spikes inside batched averages.

Benchmarks are run across isolated P-cores using `taskset -c 2,4` to measure pure L3 inter-core cache transfer (MESI invalidation) without queue depth saturation (head-of-line blocking).

### Environment

* **Compiler**: GCC (Linux)
* **Target**: Release (-O3, -march=native)
* **OS**: Linux x86_64
* **Measurement**: `rdtsc` (CPU Cycles)

### 1. Consumer Service Latency (Core Execution)

Measures the exact execution cycles of the engine thread popping a command from the L1 cache, matching the order, and updating the L2 bitmask state.

| Metric | CPU Cycles | Approx. Time (@ 5.0 GHz) |
| --- | --- | --- |
| **p50 (Median)** | 23 | ~4.6 ns |
| **p90** | 24 | ~4.8 ns |
| **p99** | 39 | ~7.8 ns |
| **Max Spike** | 1,378 | ~275.6 ns |

### 2. Un-backlogged Pipeline Roundtrip (Wire-to-Book)

Measures the true end-to-end traversal: Binary ITCH Parse $\to$ SPSC Push on Core 2 $\to$ L3 Interconnect MESI Transfer $\to$ Match Execution on Core 4 $\to$ Sync ACK.

| Metric | CPU Cycles | Approx. Time (@ 5.0 GHz) |
| --- | --- | --- |
| **p50 (Median)** | 388 | ~77.6 ns |
| **p90** | 414 | ~82.8 ns |
| **p99** | 459 | ~91.8 ns |

> **Note:** The ~360 cycle delta between the Service Latency and the Pipeline Roundtrip represents the physical silicon limit of Intel's inter-core cache invalidation protocol over the hardware bus.

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
   * *After:* Implemented a two-level hierarchical bitmask. Hardware bit-scan-forward instructions (`tzcnt`) now locate the next populated price level in deterministic $O(1)$ time (~10-25 cycles).

2. **Fixed Benchmark Methodology (Eliminated Batched Averages):**
   * *Before:* Measured latency by timing batches of 1,000,000 operations and dividing by $N$. This critically masked tail-latency spikes and queueing delays.
   * *After:* Transitioned to single-operation `rdtsc` timestamping and histogram generation. This exposed the true $p50 \to Max$ distribution, revealing the impact of C-state transitions and OS jitter.

3. **True Pipeline Flow Control vs. Queue Backlog:**
   * *Before:* Producers slammed the lock-free queue instantly, meaning measured latency was actually 99% queue waiting time (head-of-line blocking).
   * *After:* Implemented paced, ping-pong pipeline benchmarking using real binary NASDAQ ITCH 5.0 payloads. This isolates the physical MESI cross-core cache transfer latency (~70-80ns) from artificial queue saturation.
