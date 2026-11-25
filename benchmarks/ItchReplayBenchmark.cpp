// ItchReplayBenchmark.cpp

#include "orderbook/Types.hpp"
#include "orderbook/OrderBook.hpp"
#include "orderbook/RingBuffer.hpp"
#include "orderbook/ItchParser.hpp"
#include "orderbook/Rdtsc.hpp"

#include <iostream>
#include <thread>
#include <vector>
#include <array>
#include <numeric>
#include <algorithm>
#include <iomanip>
#include <atomic>
#include <memory>
#include <cstring>
#include <cassert>

using namespace orderbook;
using namespace orderbook::protocol;

struct EngineCommand {
    ParsedAction action{ParsedAction::Ignore};
    uint16_t stock_locate{0};
    OrderId id{0};
    OrderId original_id{0};
    Price price{0};
    Quantity quantity{0};
    Side side{Side::Buy};
    uint64_t ingress_tsc{0};
};

void run_itch_service_latency_benchmark(size_t total_messages) {
    constexpr size_t RING_BUFFER_CAPACITY = 65536;
    auto command_queue = std::make_unique<SPSCRingBuffer<EngineCommand, RING_BUFFER_CAPACITY>>();

    std::atomic<bool> producer_done{false};
    std::vector<uint64_t> service_latencies(total_messages, 0);
    std::vector<uint64_t> one_way_latencies(total_messages, 0);

    std::atomic<uint64_t> completed_ack{0};
    std::atomic<size_t> processed_count{0};

    // Consumer (Matching / Book-Building Engine Thread pinned on Core 4)
    std::thread engine_thread([&]() {
        using InstrumentBook = OrderBook<2000000, 200000>;
        constexpr size_t MAX_STOCK_LOCATES = 65536;
        std::vector<std::unique_ptr<InstrumentBook>> books(MAX_STOCK_LOCATES);

        // Pre-allocate book for stock_locate = 1 with reference base_price = $140.0000 (1,400,000 ticks),
        // covering $140.0001 to $160.0000 in 4-decimal ITCH integer ticks.
        constexpr uint16_t TEST_STOCK_LOCATE = 1;
        constexpr Price REFERENCE_BASE_PRICE = 1400000;
        books[TEST_STOCK_LOCATE] = std::make_unique<InstrumentBook>(REFERENCE_BASE_PRICE);

        EngineCommand cmd;

        while (!producer_done.load(std::memory_order_relaxed) || processed_count.load(std::memory_order_relaxed) < total_messages) {
            if (command_queue->pop(cmd)) {
                uint64_t service_start = rdtsc_start();

                InstrumentBook* book = books[cmd.stock_locate].get();
                if (book != nullptr) [[likely]] {
                    switch (cmd.action) {
                        case ParsedAction::Add:
                            (void)book->add_order(cmd.id, cmd.price, cmd.quantity, cmd.side, OrderType::Limit, true);
                            break;
                        case ParsedAction::PartialCancel:
                        case ParsedAction::Execute:
                            (void)book->reduce_order(cmd.id, cmd.quantity);
                            break;
                        case ParsedAction::Delete:
                            (void)book->cancel_order(cmd.id);
                            break;
                        case ParsedAction::Replace:
                            (void)book->replace_order(cmd.original_id, cmd.id, cmd.price, cmd.quantity, true);
                            break;
                        case ParsedAction::Ignore:
                            break;
                    }
                }

                uint64_t service_end = rdtsc_end();

                size_t idx = processed_count.fetch_add(1, std::memory_order_relaxed);
                if (idx < total_messages) {
                    service_latencies[idx] = service_end - service_start;
                    one_way_latencies[idx] = (service_end > cmd.ingress_tsc) ? (service_end - cmd.ingress_tsc) : 0;
                }

                completed_ack.store(cmd.id, std::memory_order_release);
            }
        }

        // Verify Round 4 book invariants and zero drift after replay
        assert(books[TEST_STOCK_LOCATE]->verify_invariants());
        assert(books[TEST_STOCK_LOCATE]->drift_crossings() == 0);
    });

    // Producer (Binary Stream Feeder Thread pinned on Core 2)
    std::thread producer_thread([&]() {
        // Pre-allocate raw ITCH binary buffers
        std::vector<std::vector<char>> raw_itch_messages;
        raw_itch_messages.reserve(total_messages);

        for (size_t i = 0; i < total_messages; ++i) {
            std::vector<char> buf(40, 0);
            if (i % 3 != 2) {
                buf.front() = 'A';
                AddOrderMessage msg{};
                msg.stock_locate = bswap16(1);
                msg.order_reference_number = bswap64(i + 1);
                const bool is_buy = (i % 2 == 0);
                msg.buy_sell_indicator = is_buy ? 'B' : 'S';
                msg.shares = bswap32(100);
                const uint32_t raw_price = is_buy
                    ? (1495000 + static_cast<uint32_t>((i % 50) * 100))
                    : (1500100 + static_cast<uint32_t>((i % 50) * 100));
                msg.price = bswap32(raw_price);
                std::memcpy(buf.data() + 1, &msg, sizeof(AddOrderMessage));
            } else {
                buf.front() = 'D';
                OrderDeleteMessage msg{};
                msg.stock_locate = bswap16(1);
                msg.order_reference_number = bswap64(i - 1);
                std::memcpy(buf.data() + 1, &msg, sizeof(OrderDeleteMessage));
            }
            raw_itch_messages.push_back(std::move(buf));
        }

        for (size_t i = 0; i < total_messages; ++i) {
            const char* msg_buf = raw_itch_messages[i].data();

            uint64_t t0 = rdtsc_start();
            ParsedItchCommand parsed = ItchParser::parse_message(msg_buf);

            if (parsed.action == ParsedAction::Ignore) continue;

            EngineCommand cmd;
            cmd.action = parsed.action;
            cmd.stock_locate = parsed.stock_locate;
            cmd.id = parsed.order_id;
            cmd.original_id = parsed.original_order_id;
            cmd.price = parsed.price;
            cmd.quantity = parsed.quantity;
            cmd.side = parsed.side;
            cmd.ingress_tsc = t0;

            while (!command_queue->push(cmd)) {
            }

            while (completed_ack.load(std::memory_order_acquire) != cmd.id) {
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    if (producer_thread.joinable()) producer_thread.join();
    if (engine_thread.joinable()) engine_thread.join();

    // Compute Service Latency
    std::sort(service_latencies.begin(), service_latencies.end());
    uint64_t sum_service = std::accumulate(service_latencies.begin(), service_latencies.end(), 0ULL);
    double avg_service = static_cast<double>(sum_service) / total_messages;

    // Compute One-Way Pipeline Latency
    std::sort(one_way_latencies.begin(), one_way_latencies.end());
    uint64_t sum_ow = std::accumulate(one_way_latencies.begin(), one_way_latencies.end(), 0ULL);
    double avg_ow = static_cast<double>(sum_ow) / total_messages;

    std::cout << "\n\n";
    std::cout << "CONSUMER SERVICE LATENCY (Pop + Book Update)\n";
    std::cout << "\n\n";
    std::cout << "Avg Service Latency   : " << std::fixed << std::setprecision(2) << avg_service << " cycles\n";
    std::cout << "p50 Service Latency   : " << service_latencies[static_cast<size_t>(total_messages * 0.50)] << " cycles\n";
    std::cout << "p99 Service Latency   : " << service_latencies[static_cast<size_t>(total_messages * 0.99)] << " cycles\n";
    std::cout << "Max Service Latency   : " << service_latencies.back() << " cycles\n\n";

    std::cout << "\n\n";
    std::cout << "UN-BACKLOGGED ONE-WAY PIPELINE LATENCY (Parse -> RingBuffer -> Book)\n";
    std::cout << "\n\n";
    std::cout << "Avg One-Way Latency   : " << std::fixed << std::setprecision(2) << avg_ow << " cycles\n";
    std::cout << "p50 One-Way Latency   : " << one_way_latencies[static_cast<size_t>(total_messages * 0.50)] << " cycles\n";
    std::cout << "p90 One-Way Latency   : " << one_way_latencies[static_cast<size_t>(total_messages * 0.90)] << " cycles\n";
    std::cout << "p99 One-Way Latency   : " << one_way_latencies[static_cast<size_t>(total_messages * 0.99)] << " cycles\n";
    std::cout << "p99.9 One-Way         : " << one_way_latencies[static_cast<size_t>(total_messages * 0.999)] << " cycles\n";
    std::cout << "Max One-Way Latency   : " << one_way_latencies.back() << " cycles\n\n";
}

int main() { run_itch_service_latency_benchmark(100000); }
