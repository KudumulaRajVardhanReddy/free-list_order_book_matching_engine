// BenchmarkEngine.cpp

#include "orderbook/Types.hpp"
#include "orderbook/Order.hpp"
#include "orderbook/FreeList.hpp"
#include "orderbook/PriceLevel.hpp"
#include "orderbook/OrderBook.hpp"

#include <iostream>
#include <chrono>
#include <vector>
#include <numeric>
#include <algorithm>
#include <iomanip>

using namespace orderbook;

// Add only Benchmark
void benchmark_add_only(size_t iterations) {
    Orderbook<1000000> book;
    std::vector<double> latencies_ns;
    latencies_ns.reserve(iterations);

    // Cache warming
    for (size_t i = 1; i <= 1000; ++i) book.add_order(i, 10000 + (i % 100), 10, Side::Buy, OrderType::Limit);

    for (size_t i = 1001; i <= 1000 + iterations; ++i) {
        auto start = std::chrono::high_resolution_clock::now();

        book.add_order(i, 10000 + (i % 100), 10, Side::Buy, OrderType::Limit);

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> elapsed = end - start;
        latencies_ns.push_back(elapsed.count());
    }

    std::sort(latencies_ns.begin(), latencies_ns.end());

    double sum = std::accumulate(latencies_ns.begin(), latencies_ns.end(), 0.0);
    double avg = sum / iterations;

    std::cout << "\n\n";
    std::cout << "ISOLATED ADD_ORDER BENCHMARK (" << iterations << " ops)";
    std::cout << "\n\n";
    std::cout << "Avg Latency: " << std::fixed << std::setprecision(2) << avg << " ns/op\n";
    std::cout << "Throughout: " << (1000.0 / avg) << " Million ops/sec\n";
    std::cout << "\n\n";
}

// Add + Cancel Pair Benchmark
void benchmark_add_cancel_hotpath(size_t iterations) {
    Orderbook<100000> book;
    std::vector<double> latencies_ns;
    latencies_ns.reserve(iterations);

    for (size_t i = 1; i <= 1000; ++i) {
        book.add_order(i, 10000, 100, Side::Buy, OrderType::Limit);
        book.cancel_order(i);
    }

    for (size_t i = 1001; i <= 1000 + iterations; ++i) {
        auto start = std::chrono::high_resolution_clock::now();

        book.add_order(i, 10000, 100, Side::Buy, OrderType::Limit);
        book.cancel_order(i);

        auto end = std::chrono::high_resolution_clock::now();

        std::chrono::duration<double, std::nano> elapsed = end - start;
        latencies_ns.push_back(elapsed.count());
    }

    std::sort(latencies_ns.begin(), latencies_ns.end());
    double sum = std::accumulate(latencies_ns.begin(), latencies_ns.end(), 0.0);

    double avg_pair_latency = sum / iterations;

    double avg_per_op_latency = avg_pair_latency / 2.0;
    double total_ops = static_cast<double>(iterations * 2);
    double total_time_sec = sum / 1e9;
    double throughput_mops = (total_ops / total_time_sec) / 1e6;

    double p50_pair = latencies_ns[static_cast<size_t>(iterations * 0.5)];
    double p99_pair = latencies_ns[static_cast<size_t>(iterations * 0.99)];

    std::cout << "\n\n";
    std::cout << "ADD + CANCEL PAIR BENCHMARK (" << iterations << " pairs = " << (iterations * 2) << " ops)\n";
    std::cout << "\n\n";
    std::cout << "Avg Pair Latency:   " << avg_pair_latency << " ns/pair\n";
    std::cout << "Avg Op Latency:     " << avg_per_op_latency << " ns/op\n";
    std::cout << "p50 (Pair Window):  " << p50_pair << " ns\n";
    std::cout << "p99 (Pair Window):  " << p99_pair << " ns\n";
    std::cout << "Pairs Throughput:   " << (1000.0 / avg_pair_latency) << " M pairs/sec";
    std::cout << "Raw Ops Throughput: " << throughput_mops << " Million ops/sec\n";
    std::cout << "\n\n";
}

void benchmark_crossing_match(size_t iterations) {
    Orderbook<1000000> book;
    std::vector<double> latencies_ns;
    latencies_ns.reserve(iterations);

    for (size_t i = 1; i <= 1000; ++i) book.add_order(i, 10050, 10, Side::Sell, OrderType::Limit);

    OrderId ask_id = 1001;
    OrderId bid_id = 1000001;

    for (size_t i = 0; i < iterations; ++i) {
        book.add_order(ask_id++, 10050, 100, Side::Sell, OrderType::Limit);
        auto start = std::chrono::high_resolution_clock::now();

        book.add_order(bid_id++, 10050, 100, Side::Buy, OrderType::Limit);

        auto end = std::chrono::high_resolution_clock::now();

        std::chrono::duration<double, std::nano> elapsed = end - start;
        latencies_ns.push_back(elapsed.count());
    }

    std::sort(latencies_ns.begin(), latencies_ns.end());
    double sum = std::accumulate(latencies_ns.begin(), latencies_ns.end(), 0.0);
    double avg = sum / iterations;

    std::cout << "\n\n";
    std::cout << "CROSSING MATCH LOOP BENCHMARK (" << iterations << " fills)";
    std::cout << "\n\n";
    std::cout << "Avg Match Latency: " << avg << " ns/fill\n";
    std::cout << "Fill Throughput:   " << (1000.0 / avg) << " Million fills/sec\n";
    std::cout << "\n\n";
}

int main() {
    benchmark_add_only(500'000);
    benchmark_add_cancel_hotpath(1'000'000);
    benchmark_crossing_match(500'000);
}
