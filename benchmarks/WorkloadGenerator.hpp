// WorkloadGenerator.hpp

#pragma once

#include "orderbook/OrderBook.hpp"
#include "orderbook/Rdtsc.hpp"

#include <vector>
#include <numeric>
#include <iostream>
#include <iomanip>
#include <algorithm>

namespace orderbook {
namespace benchmark {

class WorkloadGenerator {
public:
    static void run_deep_book_crossing(size_t iterations = 10000) {
        OrderBook<2000000> book;
        std::vector<double> cycles_per_sweep;
        cycles_per_sweep.reserve(iterations);

        OrderId id_gen = 1;

        Price mid_price = 100000;
        for (Price p = mid_price + 1; p <= mid_price + 10000; ++p) {
            for (int i = 0; i < 5; ++i) book.add_order(id_gen++, p, 10, Side::Sell, OrderType::Limit);
        }
        for (Price p = mid_price - 10000; p <= mid_price - 1; ++p) {
            for (int i = 0; i < 5; ++i) book.add_order(id_gen++, p, 10, Side::Buy, OrderType::Limit);
        }

        std::cout << "Deep Book Built. Resting Orders: " << book.size() << "\n";

        for (size_t i = 0; i < iterations; ++i) {
            for (Price p = mid_price + 1; p <= mid_price + 5; ++p) book.add_order(id_gen++, p, 10, Side::Sell, OrderType::Limit);

            uint64_t t0 = rdtsc_start();
            book.add_order(id_gen++, mid_price + 5, 50, Side::Buy, OrderType::Limit);
            uint64_t t1 = rdtsc_end();
            cycles_per_sweep.push_back(static_cast<double>(t1 - t0));
        }

        std::sort(cycles_per_sweep.begin(), cycles_per_sweep.end());
        double sum = std::accumulate(cycles_per_sweep.begin(), cycles_per_sweep.end(), 0.0);
        double avg = sum / iterations;

        std::cout << "\n\n";
        std::cout << "DEEP BOOK CROSSING WORKLOAD (" << iterations << " sweeps)";
        std::cout << "\n\n";
        std::cout << "Sweeps 5 price levels & 5 resting orders per iteration\n";
        std::cout << "Avg Cycles/Sweep: " << std::fixed << std::setprecision(2) << avg << " cycles\n";
        std::cout << "p50 Cycles/Sweep: " << cycles_per_sweep[static_cast<size_t>(iterations * 0.50)] << " cycles\n";
        std::cout << "p99 Cycles/Sweep: " << cycles_per_sweep[static_cast<size_t>(iterations * 0.99)] << " cycles\n";
        std::cout << "\n\n";
    }
};

}
}
