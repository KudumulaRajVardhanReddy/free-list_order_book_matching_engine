// ItchReplayBenchmark.cpp

#include "orderbook/Types.hpp"
#include "orderbook/OrderBook.hpp"
#include "orderbook/RingBuffer.hpp"
#include "orderbook/ItchParser.hpp"
#include "orderbook/Rdtsc.hpp"

#include <iostream>
#include <thread>
#include <vector>
#include <numeric>
#include <algorithm>
#include <iomanip>
#include <atomic>
#include <memory>
#include <cstring>
#include <x86intrin.h>

using namespace orderbook;
using namespace orderbook::protocol;

struct EngineCommand {
    enum class Action : uint8_t { Add, Cancel } action;
    OrderId id{0};
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
    std::vector<uint64_t> roundtrip_latencies(total_messages, 0);

    std::atomic<uint64_t> completed_ack{0};
    std::atomic<size_t> processed_count{0};

    // Consumer (Matching Engine Thread pinned on Core 4)
    std::thread engine_thread([&]() {
        auto book = std::make_unique<OrderBook<2000000, 200000>>();
        EngineCommand cmd;

        while (!producer_done.load(std::memory_order_relaxed) || processed_count.load(std::memory_order_relaxed) < total_messages) {
            if (command_queue->pop(cmd)) {
                uint64_t service_start = rdtsc_start();

                if (cmd.action == EngineCommand::Action::Add) {
                    (void)book->add_order(cmd.id, cmd.price, cmd.quantity, cmd.side, OrderType::Limit);
                } else if (cmd.action == EngineCommand::Action::Cancel) {
                    (void)book->cancel_order(cmd.id);
                }

                uint64_t service_end = rdtsc_end();

                size_t idx = processed_count.fetch_add(1, std::memory_order_relaxed);
                if (idx < total_messages) {
                    service_latencies[idx] = service_end - service_start;
                    roundtrip_latencies[idx] = (service_end > cmd.ingress_tsc) ? (service_end - cmd.ingress_tsc) : 0;
                }

                completed_ack.store(cmd.id, std::memory_order_release);
            } else {
                #if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
                #endif
            }
        }
    });

    // Producer (Binary Stream Feeder Thread pinned on Core 2)
    std::thread producer_thread([&]() {
        // Pre-allocate raw ITCH binary buffers
        std::vector<std::vector<char>> raw_itch_messages;
        raw_itch_messages.reserve(total_messages);

        for (size_t i = 0; i < total_messages; ++i) {
            std::vector<char> buf(40, 0);
            if (i % 3 != 2) {
                buf[0] = 'A';
                AddOrderMessage msg{};
                msg.order_reference_number = bswap64(i + 1);
                msg.buy_sell_indicator = (i % 2 == 0) ? 'B' : 'S';
                msg.shares = bswap32(100);
                msg.price = bswap32(10000 + (i % 500));
                std::memcpy(buf.data() + 1, &msg, sizeof(AddOrderMessage));
            } else {
                buf[0] = 'D';
                OrderDeleteMessage msg{};
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
            cmd.action = (parsed.action == ParsedAction::Add) ? EngineCommand::Action::Add : EngineCommand::Action::Cancel;
            cmd.id = parsed.order_id;
            cmd.price = parsed.price;
            cmd.quantity = parsed.quantity;
            cmd.side = parsed.side;
            cmd.ingress_tsc = t0;

            while (!command_queue->push(cmd)) {
                #if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
                #endif
            }

            while (completed_ack.load(std::memory_order_acquire) != cmd.id) {
                #if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
                #endif
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

    // Compute Roundtrip Latency
    std::sort(roundtrip_latencies.begin(), roundtrip_latencies.end());
    uint64_t sum_rt = std::accumulate(roundtrip_latencies.begin(), roundtrip_latencies.end(), 0ULL);
    double avg_rt = static_cast<double>(sum_rt) / total_messages;

    std::cout << "\n\n";
    std::cout << "CONSUMER SERVICE LATENCY (Pop + Match + Book Update)\n";
    std::cout << "\n\n";
    std::cout << "Avg Service Latency   : " << std::fixed << std::setprecision(2) << avg_service << " cycles\n";
    std::cout << "p50 Service Latency   : " << service_latencies[static_cast<size_t>(total_messages * 0.50)] << " cycles\n";
    std::cout << "p99 Service Latency   : " << service_latencies[static_cast<size_t>(total_messages * 0.99)] << " cycles\n";
    std::cout << "Max Service Latency   : " << service_latencies.back() << " cycles\n\n";

    std::cout << "\n\n";
    std::cout << "UN-BACKLOGGED PIPELINE ROUNDTRIP (Parse -> RingBuffer -> Book)\n";
    std::cout << "\n\n";
    std::cout << "Avg Roundtrip Latency : " << std::fixed << std::setprecision(2) << avg_rt << " cycles\n";
    std::cout << "p50 Roundtrip Latency : " << roundtrip_latencies[static_cast<size_t>(total_messages * 0.50)] << " cycles\n";
    std::cout << "p90 Roundtrip Latency : " << roundtrip_latencies[static_cast<size_t>(total_messages * 0.90)] << " cycles\n";
    std::cout << "p99 Roundtrip Latency : " << roundtrip_latencies[static_cast<size_t>(total_messages * 0.99)] << " cycles\n";
    std::cout << "p99.9 Roundtrip       : " << roundtrip_latencies[static_cast<size_t>(total_messages * 0.999)] << " cycles\n";
    std::cout << "Max Roundtrip Latency : " << roundtrip_latencies.back() << " cycles\n\n";
}

int main() { run_itch_service_latency_benchmark(100000); }
