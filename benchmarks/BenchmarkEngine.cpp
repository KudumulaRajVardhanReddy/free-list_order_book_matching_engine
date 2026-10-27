// BenchmarkEngine.cpp

#include "orderbook/Types.hpp"
#include "orderbook/Order.hpp"
#include "orderbook/FreeList.hpp"
#include "orderbook/PriceLevel.hpp"
#include "orderbook/OrderBook.hpp"
#include "orderbook/Rdtsc.hpp"
#include "WorkloadGenerator.hpp"

#include <iostream>
#include <chrono>
#include <vector>
#include <numeric>
#include <algorithm>
#include <iomanip>
#include <memory>

using namespace orderbook;
constexpr size_t BATCH_SIZE = 1000;


template <typename T>
inline void suppress_unused(T&&) noexcept {}

void benchmark_add_only_batched(size_t total_batches) {

    auto book = std::make_unique<OrderBook<1000000>>();
    std::vector<double> cycles_per_op;
    cycles_per_op.reserve(total_batches);

    for (size_t i = 1; i <= 1000; ++i) {
        suppress_unused(book->add_order(i, 10000 + (i % 100), 10, Side::Buy, OrderType::Limit));
    }

    OrderId order_id = 1001;

    for (size_t b = 0; b < total_batches; ++b) {
        uint64_t t0 = rdtsc_start();

        for (size_t i = 0; i < BATCH_SIZE; ++i) {
            suppress_unused(book->add_order(order_id++, 10000 + (i % 100), 10, Side::Buy, OrderType::Limit));
        }

        uint64_t t1 = rdtsc_end();

        double cycles_batch = static_cast<double>(t1 - t0);
        cycles_per_op.push_back(cycles_batch / static_cast<double>(BATCH_SIZE));
    }

    std::sort(cycles_per_op.begin(), cycles_per_op.end());
    double sum = std::accumulate(cycles_per_op.begin(), cycles_per_op.end(), 0.0);
    double avg_cycles = sum / total_batches;

    std::cout << "\n\n";
    std::cout << "BATCHED ADD_ORDER BENCHMARK (" << (total_batches * BATCH_SIZE) << " ops)\n";
    std::cout << "\n\n";
    std::cout << "Avg Cycles/Op: " << std::fixed << std::setprecision(2) << avg_cycles << " cycles\n";
    std::cout << "p50 Cycles/Op: " << cycles_per_op[static_cast<size_t>(total_batches * 0.50)] << " cycles\n";
    std::cout << "p99 Cycles/Op: " << cycles_per_op[static_cast<size_t>(total_batches * 0.99)] << " cycles\n";
    std::cout << "\n\n";
}

void benchmark_add_cancel_batched(size_t total_batches) {
    auto book = std::make_unique<OrderBook<1000000>>();
    std::vector<double> cycles_per_pair;
    cycles_per_pair.reserve(total_batches);

    for (size_t i = 1; i <= 1000; ++i) {
        suppress_unused(book->add_order(i, 10000, 100, Side::Buy, OrderType::Limit));
        suppress_unused(book->cancel_order(i));
    }

    OrderId order_id = 1001;

    for (size_t b = 0; b < total_batches; ++b) {
        uint64_t t0 = rdtsc_start();

        for (size_t i = 0; i < BATCH_SIZE; ++i) {
            OrderId id = order_id++;
            suppress_unused(book->add_order(id, 10000, 100, Side::Buy, OrderType::Limit));
            suppress_unused(book->cancel_order(id));
        }

        uint64_t t1 = rdtsc_end();

        double cycles_batch = static_cast<double>(t1 - t0);
        cycles_per_pair.push_back(cycles_batch / static_cast<double>(BATCH_SIZE));
    }

    std::sort(cycles_per_pair.begin(), cycles_per_pair.end());
    double sum = std::accumulate(cycles_per_pair.begin(), cycles_per_pair.end(), 0.0);
    double avg_pair_cycles = sum / total_batches;
    double avg_single_op_cycles = avg_pair_cycles / 2.0;

    std::cout << "\n\n";
    std::cout << "BATCHED ADD + CANCEL BENCHMARK (" << (total_batches * BATCH_SIZE) << " pairs)\n";
    std::cout << "\n\n";
    std::cout << "Avg Cycles/Pair: " << avg_pair_cycles << " cycles\n";
    std::cout << "Avg Cycles/Op:   " << avg_single_op_cycles << " cycles\n";
    std::cout << "p50 Cycles/Pair: " << cycles_per_pair[static_cast<size_t>(total_batches * 0.50)] << " cycles\n";
    std::cout << "p99 Cycles/Pair: " << cycles_per_pair[static_cast<size_t>(total_batches * 0.99)] << " cycles\n";
    std::cout << "\n\n";
}

void benchmark_crossing_match_batched(size_t total_batches) {
    std::vector<double> cycles_per_fill;
    cycles_per_fill.reserve(total_batches);

    volatile size_t state_sink = 0;
    volatile Price v_price = 10050;
    volatile Quantity v_qty = 100;

    for (size_t b = 0; b < total_batches; ++b) {
        auto book = std::make_unique<OrderBook<1000000>>();

        OrderId ask_id = 1;
        OrderId bid_id = 10000;

        for (size_t i = 0; i < BATCH_SIZE; ++i) {
            suppress_unused(book->add_order(ask_id++, v_price, v_qty, Side::Sell, OrderType::Limit));
        }

        uint64_t t0 = rdtsc_start();

        for (size_t i = 0; i < BATCH_SIZE; ++i) {
            suppress_unused(book->add_order(bid_id++, v_price, v_qty, Side::Buy, OrderType::Limit));
            #if defined(__GNUC__) || defined(__clang__)
            asm volatile("" : : : "memory");
            #endif
        }

        uint64_t t1 = rdtsc_end();

        state_sink += book->size();

        double cycles_batch = static_cast<double>(t1 - t0);
        cycles_per_fill.push_back(cycles_batch / static_cast<double>(BATCH_SIZE));
    }

    std::sort(cycles_per_fill.begin(), cycles_per_fill.end());
    double sum = std::accumulate(cycles_per_fill.begin(), cycles_per_fill.end(), 0.0);
    double avg_cycles = sum / total_batches;

    std::cout << "\n\n";
    std::cout << "BATCHED CROSSING MATCH BENCHMARK (" << (total_batches * BATCH_SIZE) << " fills)\n";
    std::cout << "\n\n";
    std::cout << "Avg Cycles/Fill: " << std::fixed << std::setprecision(2) << avg_cycles << " cycles\n";
    std::cout << "p50 Cycles/Fill: " << cycles_per_fill[static_cast<size_t>(total_batches * 0.50)] << " cycles\n";
    std::cout << "p99 Cycles/Fill: " << cycles_per_fill[static_cast<size_t>(total_batches * 0.99)] << " cycles\n";
    std::cout << "[Anti-Optimization Sink: " << state_sink << "]\n";
    std::cout << "\n\n";
}

int main() {
    benchmark_add_only_batched(1000);
    benchmark_add_cancel_batched(1000);
    benchmark_crossing_match_batched(1000);
    orderbook::benchmark::WorkloadGenerator::run_deep_book_crossing(10000);
}
