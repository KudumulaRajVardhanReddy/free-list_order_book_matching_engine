// BenchmarkEngine.cpp

#include "orderbook/Types.hpp"
#include "orderbook/Order.hpp"
#include "orderbook/FreeList.hpp"
#include "orderbook/PriceLevel.hpp"
#include "orderbook/OrderBook.hpp"
#include "orderbook/Rdtsc.hpp"
#include "WorkloadGenerator.hpp"

#include <iostream>
#include <vector>
#include <numeric>
#include <algorithm>
#include <iomanip>
#include <memory>

using namespace orderbook;

template <typename T>
inline void suppress_unused(T&&) noexcept {}

void benchmark_add_only_single_op(size_t total_ops) {
    auto book = std::make_unique<OrderBook<1000000, 200000>>();
    std::vector<uint64_t> latencies;
    latencies.resize(total_ops);

    for (size_t i = 1; i <= 1000; ++i) suppress_unused(book->add_order(i, 10000 + (i % 100), 10, Side::Buy, OrderType::Limit));

    OrderId order_id = 1001;

    for (size_t i = 0; i < total_ops; ++i) {
        uint64_t t0 = rdtsc_start();
        suppress_unused(book->add_order(order_id++, 10000 + (i % 100), 10, Side::Buy, OrderType::Limit));
        uint64_t t1 = rdtsc_end();

        latencies[i] = t1 - t0;
    }

    std::sort(latencies.begin(), latencies.end());
    uint64_t sum = std::accumulate(latencies.begin(), latencies.end(), 0ULL);
    double avg_cycles = static_cast<double>(sum) / total_ops;

    std::cout << "\n\n";
    std::cout << "SINGLE-OP ADD_ORDER LATENCY HISTOGRAM (" << total_ops << " ops)";
    std::cout << "\n\n";
    std::cout << "Avg Latency   : " << std::fixed << std::setprecision(2) << avg_cycles << " cycles\n";
    std::cout << "p50 Latency   : " << latencies[static_cast<size_t>(total_ops * 0.50)] << " cycles\n";
    std::cout << "p90 Latency   : " << latencies[static_cast<size_t>(total_ops * 0.90)] << " cycles\n";
    std::cout << "p99 Latency   : " << latencies[static_cast<size_t>(total_ops * 0.99)] << " cycles\n";
    std::cout << "p99.9 Latency : " << latencies[static_cast<size_t>(total_ops * 0.999)] << " cycles\n";
    std::cout << "p99.99 Latency: " << latencies[static_cast<size_t>(total_ops * 0.9999)] << " cycles\n";
    std::cout << "Max Latency   : " << latencies.back() << " cycles\n\n";
}

void benchmark_add_cancel_pair_single_op(size_t total_pairs) {
    auto book = std::make_unique<OrderBook<1000000, 200000>>();
    std::vector<uint64_t> pair_latencies;
    pair_latencies.resize(total_pairs);

    // Warm-up phase
    for (size_t i = 1; i <= 1000; ++i) {
        suppress_unused(book->add_order(i, 10000, 100, Side::Buy, OrderType::Limit));
        suppress_unused(book->cancel_order(i));
    }

    OrderId order_id = 1001;

    for (size_t i = 0; i < total_pairs; ++i) {
        OrderId id = order_id++;

        uint64_t t0 = rdtsc_start();
        suppress_unused(book->add_order(id, 10000, 100, Side::Buy, OrderType::Limit));
        suppress_unused(book->cancel_order(id));
        uint64_t t1 = rdtsc_end();

        pair_latencies[i] = t1 - t0;
    }

    std::sort(pair_latencies.begin(), pair_latencies.end());
    uint64_t sum = std::accumulate(pair_latencies.begin(), pair_latencies.end(), 0ULL);
    double avg_pair_cycles = static_cast<double>(sum) / total_pairs;

    std::cout << "\n\n";
    std::cout << "SINGLE-PAIR ADD + CANCEL LATENCY HISTOGRAM (" << total_pairs << " pairs)";
    std::cout << "\n\n";
    std::cout << "Avg Pair Latency   : " << std::fixed << std::setprecision(2) << avg_pair_cycles << " cycles\n";
    std::cout << "p50 Pair Latency   : " << pair_latencies[static_cast<size_t>(total_pairs * 0.50)] << " cycles\n";
    std::cout << "p90 Pair Latency   : " << pair_latencies[static_cast<size_t>(total_pairs * 0.90)] << " cycles\n";
    std::cout << "p99 Pair Latency   : " << pair_latencies[static_cast<size_t>(total_pairs * 0.99)] << " cycles\n";
    std::cout << "p99.9 Pair Latency : " << pair_latencies[static_cast<size_t>(total_pairs * 0.999)] << " cycles\n";
    std::cout << "p99.99 Pair Latency: " << pair_latencies[static_cast<size_t>(total_pairs * 0.9999)] << " cycles\n";
    std::cout << "Max Pair Latency   : " << pair_latencies.back() << " cycles\n\n";
}

void benchmark_crossing_match_single_op(size_t total_fills) {
    std::vector<uint64_t> fill_latencies;
    fill_latencies.resize(total_fills);

    volatile size_t state_sink = 0;
    volatile Price v_price = 10050;
    volatile Quantity v_qty = 100;

    auto book = std::make_unique<OrderBook<1000000, 200000>>();

    OrderId ask_id = 1;
    OrderId bid_id = 1000000;

    for (size_t i = 0; i < total_fills; ++i) {
        suppress_unused(book->add_order(ask_id++, v_price, v_qty, Side::Sell, OrderType::Limit));
    }

    for (size_t i = 0; i < total_fills; ++i) {
        uint64_t t0 = rdtsc_start();
        suppress_unused(book->add_order(bid_id++, v_price, v_qty, Side::Buy, OrderType::Limit));
        uint64_t t1 = rdtsc_end();

#if defined(__GNUC__) || defined(__clang__)
        asm volatile("" : : : "memory");
#endif
        fill_latencies[i] = t1 - t0;
    }

    state_sink += book->size();

    std::sort(fill_latencies.begin(), fill_latencies.end());
    uint64_t sum = std::accumulate(fill_latencies.begin(), fill_latencies.end(), 0ULL);
    double avg_cycles = static_cast<double>(sum) / total_fills;

    std::cout << "\n\n";
    std::cout << "SINGLE-OP CROSSING MATCH LATENCY HISTOGRAM (" << total_fills << " fills)";
    std::cout << "\n\n";
    std::cout << "Avg Fill Latency   : " << std::fixed << std::setprecision(2) << avg_cycles << " cycles\n";
    std::cout << "p50 Fill Latency   : " << fill_latencies[static_cast<size_t>(total_fills * 0.50)] << " cycles\n";
    std::cout << "p90 Fill Latency   : " << fill_latencies[static_cast<size_t>(total_fills * 0.90)] << " cycles\n";
    std::cout << "p99 Fill Latency   : " << fill_latencies[static_cast<size_t>(total_fills * 0.99)] << " cycles\n";
    std::cout << "p99.9 Fill Latency : " << fill_latencies[static_cast<size_t>(total_fills * 0.999)] << " cycles\n";
    std::cout << "p99.99 Fill Latency: " << fill_latencies[static_cast<size_t>(total_fills * 0.9999)] << " cycles\n";
    std::cout << "Max Fill Latency   : " << fill_latencies.back() << " cycles\n";
    std::cout << "[Anti-Optimization Sink: " << state_sink << "]\n\n";
}

int main() {
    benchmark_add_only_single_op(100000);
    benchmark_add_cancel_pair_single_op(100000);
    benchmark_crossing_match_single_op(100000);
    orderbook::benchmark::WorkloadGenerator::run_deep_book_crossing(10000);
}
