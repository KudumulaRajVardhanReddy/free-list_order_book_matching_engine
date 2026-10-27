# Order Book & Matching Engine

A multithreaded order book matching engine implemented in C++20. The system utilizes lock-free concurrency primitives and pre-allocated object pools to ensure zero dynamic memory allocations on the execution path.

## Technical Specifications

### Memory Management
- **FreeList<Capacity>**: A custom object pool allocator. Initializes contiguous memory for order structures at startup. Order insertion and deletion fetch and return pointers to this pool in O(1) time, bypassing OS-level malloc/free or new/delete calls during runtime.

### Data Structures & Complexity
- **Order Lookup (Flat Array)**: Maps `OrderId` directly to `Order*` via contiguous array indexing for deterministic O(1) lookup and state verification, replacing standard library hash maps.
- **Price Levels**: Bids and asks are managed in ordered structures.
- **Resting Orders (Intrusive Doubly Linked List)**: Each price level maintains an intrusive doubly linked list of `Order` nodes. This permits O(1) order cancellation and removal from the middle of a queue without traversing the list or requiring secondary pointer allocations.

### Concurrency Model
- **SPSCRingBuffer<T, Capacity>**: A Single-Producer Single-Consumer lock-free queue.
- **Memory Ordering**: Uses `std::memory_order_acquire` and `std::memory_order_release` for thread synchronization.
- **Cache Alignment**: Utilizes `alignas` (hardware destructive interference size, typically 64 bytes) on read/write indices to prevent false sharing across CPU cache lines.
- **Thread Decoupling**: An ingestion thread passes by-value `OrderCommand` structs across the ring buffer. The matching engine thread consumes these commands and modifies the order book state sequentially.

## Build Instructions

The project uses CMake and requires a compiler supporting C++20.

```bash
# Generate build files (Ninja or Make)
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..

# Compile the application and benchmarks
cmake --build . -j
```

## Benchmark Results

The benchmarking suite isolates the core execution paths of the engine. Measurements bypass OS timer quantization (`std::chrono`) by utilizing the hardware timestamp counter (`rdtsc`) to record elapsed CPU cycles.

To prevent aggressive Dead Code Elimination (DCE) and algebraic simplification by modern compilers (specifically GCC 15+), benchmarks utilize volatile variables for order parameters, hardware memory clobbers (`asm volatile("" ::: "memory")`), and state sinks to enforce execution.

### Environment
- **Compiler**: GCC 15.2.0 (MinGW-w64)
- **Target**: Release (-O3)
- **OS**: Windows x86_64
- **Measurement**: `rdtsc` (CPU Cycles)

### Metrics

| Workload | Iterations | Avg Cycles/Op | p50 Cycles | p99 Cycles |
| :--- | :--- | :--- | :--- | :--- |
| **Batched Add Order (Limit Rests)** | 1,000,000 | 9.66 | 9.38 | 15.31 |
| **Batched Add + Cancel (Pair)** | 1,000,000 | 98.08 (49.04/op) | 95.17 | 128.88 |
| **Batched Crossing Match (Fill)** | 1,000,000 | 10.36 | 9.85 | 21.23 |
| **Deep Book Sweep (5 Levels, 5 Orders)** | 10,000 | 100.36 (per sweep) | 85.00 | 114.00 |

> **Note:** At a reference clock of 3.0 GHz, 10 CPU cycles equates to approximately 3.3 nanoseconds.

## Scope & Future Plans

This iteration establishes the zero-allocation memory model, deterministic execution paths, and hardware-level benchmarking. Future development will focus on:

- **Network Ingestion Layer**: Integrating a lock-free socket polling mechanism (e.g., epoll or io_uring) to feed the `SPSCRingBuffer`.
- **Market Data Dissemination**: Implementing outbound multicast message generation for order book state updates (L2/L3 feeds) corresponding to execution events.
- **NUMA Awareness**: Enforcing CPU core pinning and ensuring memory allocations are constrained to the local NUMA node of the matching thread to prevent cross-socket memory latency.