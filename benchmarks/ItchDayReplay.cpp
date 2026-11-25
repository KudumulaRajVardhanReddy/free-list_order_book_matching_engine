// ItchDayReplay.cpp

#include "orderbook/Types.hpp"
#include "orderbook/OrderBook.hpp"
#include "orderbook/ItchParser.hpp"

#include <iostream>
#include <fstream>
#include <vector>
#include <array>
#include <string>
#include <string_view>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <iomanip>
#include <cassert>

using namespace orderbook;
using namespace orderbook::protocol;

#pragma pack(push, 1)
struct StockDirectoryMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    std::array<uint8_t, 6> timestamp;
    std::array<char, 8> stock;
    char market_category;
    char financial_status_indicator;
    uint32_t round_lot_size;
    char round_lots_only;
    char issue_classification;
    std::array<char, 2> issue_sub_type;
    char authenticity;
    char short_sale_threshold_indicator;
    char ipo_flag;
    char luld_reference_price_tier;
    char etp_flag;
    uint32_t etp_leverage_factor;
    char inverse_indicator;
};
#pragma pack(pop)

static_assert(sizeof(StockDirectoryMessage) == 38, "ITCH 'R' payload must be 38 bytes");

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path_to_unzipped_ITCH50_file> [TICKER (default: AAPL)]\n";
        return 1;
    }

    const std::string file_path = argv[1];
    std::string target_ticker = (argc >= 3) ? argv[2] : "AAPL";
    target_ticker.resize(8, ' ');

    std::ifstream in(file_path, std::ios::binary);
    if (!in) {
        std::cerr << "Failed to open ITCH file: " << file_path << "\n";
        return 1;
    }

    using DayBook = OrderBook<400000000, 4000000>;
    std::unique_ptr<DayBook> book;

    uint16_t target_locate = 0;
    bool locate_found = false;

    uint64_t total_file_messages = 0;
    uint64_t target_messages = 0;
    uint64_t add_count = 0;
    uint64_t exec_count = 0;
    uint64_t partial_cancel_count = 0;
    uint64_t delete_count = 0;
    uint64_t replace_count = 0;
    uint64_t invariant_checks = 0;

    std::vector<char> msg_buf(1024, 0);
    auto wall_start = std::chrono::steady_clock::now();

    while (true) {
        uint16_t raw_len = 0;
        in.read(reinterpret_cast<char*>(&raw_len), sizeof(raw_len));
        if (!in) break;

        const uint16_t msg_len = bswap16(raw_len);
        if (msg_len == 0) continue;
        if (msg_len > msg_buf.size()) msg_buf.resize(msg_len);

        in.read(msg_buf.data(), msg_len);
        if (!in) break;

        ++total_file_messages;
        const char msg_type = msg_buf[0];

        if (msg_type == 'R' && msg_len >= 39) {
            const auto* dir = reinterpret_cast<const StockDirectoryMessage*>(msg_buf.data() + 1);
            if (std::memcmp(dir->stock.data(), target_ticker.data(), 8) == 0) {
                target_locate = bswap16(dir->stock_locate);
                locate_found = true;
                std::cout << "[Directory] Found ticker '" << target_ticker
                          << "' -> stock_locate = " << target_locate << "\n";
            }
            continue;
        }

        if (!locate_found) continue;

        ParsedItchCommand cmd = ItchParser::parse_message(msg_buf.data());
        if (cmd.action == ParsedAction::Ignore || cmd.stock_locate != target_locate) {
            continue;
        }

        ++target_messages;

        if (!book && cmd.action == ParsedAction::Add && cmd.price > 0) {
            const Price half_span = 2000000;
            const Price base = (cmd.price > half_span) ? (cmd.price - half_span) : 0;
            book = std::make_unique<DayBook>(base);
            std::cout << "[Init Book] base_price = $" << std::fixed << std::setprecision(4)
                      << (static_cast<double>(base) / 10000.0)
                      << " | Window: [$" << (static_cast<double>(base) / 10000.0)
                      << ", $" << (static_cast<double>(base + 4000000) / 10000.0) << "]\n";
        }

        if (!book) continue;

        switch (cmd.action) {
            case ParsedAction::Add:
                (void)book->add_order(cmd.order_id, cmd.price, cmd.quantity, cmd.side, OrderType::Limit, true);
                ++add_count;
                break;
            case ParsedAction::Execute:
                (void)book->reduce_order(cmd.order_id, cmd.quantity);
                ++exec_count;
                break;
            case ParsedAction::PartialCancel:
                (void)book->reduce_order(cmd.order_id, cmd.quantity);
                ++partial_cancel_count;
                break;
            case ParsedAction::Delete:
                (void)book->cancel_order(cmd.order_id);
                ++delete_count;
                break;
            case ParsedAction::Replace:
                (void)book->replace_order(cmd.original_order_id, cmd.order_id, cmd.price, cmd.quantity, true);
                ++replace_count;
                break;
            case ParsedAction::Ignore:
                break;
        }

        if ((target_messages % 10000) == 0) {
            if (!book->verify_invariants()) {
                std::cerr << "[ERROR] Book invariant failed at target message #" << target_messages << "\n";
                return 2;
            }
            ++invariant_checks;
        }
    }

    auto wall_end = std::chrono::steady_clock::now();
    const double elapsed_sec = std::chrono::duration<double>(wall_end - wall_start).count();

    if (!book) {
        std::cerr << "No orders processed for ticker '" << target_ticker << "'.\n";
        return 1;
    }

    const bool final_invariants_ok = book->verify_invariants();
    ++invariant_checks;

    std::cout << "\n\n";
    std::cout << "NASDAQ ITCH 5.0 FULL-DAY REPLAY REPORT (" << target_ticker << ")\n";
    std::cout << "\n";
    std::cout << "Total ITCH File Messages : " << total_file_messages << "\n";
    std::cout << "Target Symbol Events     : " << target_messages << "\n";
    std::cout << "  - Adds ('A'/'F')       : " << add_count << "\n";
    std::cout << "  - Executes ('E'/'C')   : " << exec_count << "\n";
    std::cout << "  - Partial Cancels ('X'): " << partial_cancel_count << "\n";
    std::cout << "  - Deletes ('D')        : " << delete_count << "\n";
    std::cout << "  - Replaces ('U')       : " << replace_count << "\n";
    std::cout << "Replay Wall Time         : " << std::fixed << std::setprecision(2) << elapsed_sec << " s ("
              << std::setprecision(2) << (static_cast<double>(total_file_messages) / 1e6 / elapsed_sec)
              << " M msgs/sec)\n";
    std::cout << "Invariant Checks Passed  : " << invariant_checks << " (Final: "
              << (final_invariants_ok ? "PASS" : "FAIL") << ")\n";
    std::cout << "Drift Crossings Detected : " << book->drift_crossings() << "\n";
    std::cout << "End-of-Day Resting Orders: " << book->size() << "\n";
    std::cout << "End-of-Day Best Bid      : $" << std::fixed << std::setprecision(4)
              << (static_cast<double>(book->best_bid()) / 10000.0) << "\n";
    std::cout << "End-of-Day Best Ask      : $" << std::fixed << std::setprecision(4)
              << (static_cast<double>(book->best_ask()) / 10000.0) << "\n";
    std::cout << "\n";

    return (final_invariants_ok && book->drift_crossings() == 0) ? 0 : 1;
}
