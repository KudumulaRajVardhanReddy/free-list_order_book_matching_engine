// ItchParser.hpp

#pragma once

#include "Types.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <byteswap.h>

namespace orderbook {
namespace protocol {

// Compiler intrinsics for fast big-endian to host byte conversion
inline uint16_t bswap16(uint16_t val) noexcept { return __builtin_bswap16(val); }
inline uint32_t bswap32(uint32_t val) noexcept { return __builtin_bswap32(val); }
inline uint64_t bswap64(uint64_t val) noexcept { return __builtin_bswap64(val); }

inline uint64_t decode_timestamp(const std::array<uint8_t, 6>& ts) noexcept {
    return (static_cast<uint64_t>(ts[0]) << 40) |
           (static_cast<uint64_t>(ts[1]) << 32) |
           (static_cast<uint64_t>(ts[2]) << 24) |
           (static_cast<uint64_t>(ts[3]) << 16) |
           (static_cast<uint64_t>(ts[4]) << 8)  |
           static_cast<uint64_t>(ts[5]);
}

#pragma pack(push, 1)
struct ItchHeader {
    uint16_t message_length;
    char message_type;
};

struct AddOrderMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    std::array<uint8_t, 6> timestamp;
    uint64_t order_reference_number;
    char buy_sell_indicator;
    uint32_t shares;
    std::array<char, 8> stock;
    uint32_t price;
};

struct AddOrderMPIDMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    std::array<uint8_t, 6> timestamp;
    uint64_t order_reference_number;
    char buy_sell_indicator;
    uint32_t shares;
    std::array<char, 8> stock;
    uint32_t price;
    std::array<char, 4> attribution;
};

struct OrderExecutedMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    std::array<uint8_t, 6> timestamp;
    uint64_t order_reference_number;
    uint32_t executed_shares;
    uint64_t match_number;
};

struct OrderExecutedWithPriceMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    std::array<uint8_t, 6> timestamp;
    uint64_t order_reference_number;
    uint32_t executed_shares;
    uint64_t match_number;
    char printable;
    uint32_t execution_price;
};

struct OrderCancelMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    std::array<uint8_t, 6> timestamp;
    uint64_t order_reference_number;
    uint32_t canceled_shares;
};

struct OrderDeleteMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    std::array<uint8_t, 6> timestamp;
    uint64_t order_reference_number;
};

struct OrderReplaceMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    std::array<uint8_t, 6> timestamp;
    uint64_t original_order_reference_number;
    uint64_t new_order_reference_number;
    uint32_t shares;
    uint32_t price;
};
#pragma pack(pop)

// Compile-time layout verification against NASDAQ ITCH 5.0 spec (payload sizes excluding 1-byte message_type)
static_assert(sizeof(AddOrderMessage) == 35, "ITCH 'A' payload must be 35 bytes");
static_assert(sizeof(AddOrderMPIDMessage) == 39, "ITCH 'F' payload must be 39 bytes");
static_assert(sizeof(OrderExecutedMessage) == 30, "ITCH 'E' payload must be 30 bytes");
static_assert(sizeof(OrderExecutedWithPriceMessage) == 35, "ITCH 'C' payload must be 35 bytes");
static_assert(sizeof(OrderCancelMessage) == 22, "ITCH 'X' payload must be 22 bytes");
static_assert(sizeof(OrderDeleteMessage) == 18, "ITCH 'D' payload must be 18 bytes");
static_assert(sizeof(OrderReplaceMessage) == 34, "ITCH 'U' payload must be 34 bytes");

enum class ParsedAction : uint8_t {
    Add,
    PartialCancel, // 'X': reduce resting quantity by canceled_shares
    Delete,        // 'D': remove resting order completely
    Execute,       // 'E' / 'C': reduce resting quantity by executed_shares
    Replace,       // 'U': cancel original_order_id and insert new order_id
    Ignore
};

struct ParsedItchCommand {
    ParsedAction action{ParsedAction::Ignore};
    uint16_t stock_locate{0};
    OrderId order_id{0};
    OrderId original_order_id{0}; 
    Price price{0};
    Quantity quantity{0};
    Side side{Side::Buy};
};

class ItchParser {
public:
    static inline ParsedItchCommand parse_message(const char* buffer) noexcept {
        const char msg_type = *buffer;

        switch (msg_type) {
            case 'A': {
                const auto* msg = reinterpret_cast<const AddOrderMessage*>(buffer + 1);
                ParsedItchCommand cmd;
                cmd.action = ParsedAction::Add;
                cmd.stock_locate = bswap16(msg->stock_locate);
                cmd.order_id = bswap64(msg->order_reference_number);
                cmd.price = static_cast<Price>(bswap32(msg->price));
                cmd.quantity = bswap32(msg->shares);
                cmd.side = (msg->buy_sell_indicator == 'B') ? Side::Buy : Side::Sell;
                return cmd;
            }
            case 'F': {
                const auto* msg = reinterpret_cast<const AddOrderMPIDMessage*>(buffer + 1);
                ParsedItchCommand cmd;
                cmd.action = ParsedAction::Add;
                cmd.stock_locate = bswap16(msg->stock_locate);
                cmd.order_id = bswap64(msg->order_reference_number);
                cmd.price = static_cast<Price>(bswap32(msg->price));
                cmd.quantity = bswap32(msg->shares);
                cmd.side = (msg->buy_sell_indicator == 'B') ? Side::Buy : Side::Sell;
                return cmd;
            }
            case 'E': {
                const auto* msg = reinterpret_cast<const OrderExecutedMessage*>(buffer + 1);
                ParsedItchCommand cmd;
                cmd.action = ParsedAction::Execute;
                cmd.stock_locate = bswap16(msg->stock_locate);
                cmd.order_id = bswap64(msg->order_reference_number);
                cmd.quantity = bswap32(msg->executed_shares);
                return cmd;
            }
            case 'C': {
                const auto* msg = reinterpret_cast<const OrderExecutedWithPriceMessage*>(buffer + 1);
                ParsedItchCommand cmd;
                cmd.action = ParsedAction::Execute;
                cmd.stock_locate = bswap16(msg->stock_locate);
                cmd.order_id = bswap64(msg->order_reference_number);
                cmd.price = static_cast<Price>(bswap32(msg->execution_price));
                cmd.quantity = bswap32(msg->executed_shares);
                return cmd;
            }
            case 'X': {
                const auto* msg = reinterpret_cast<const OrderCancelMessage*>(buffer + 1);
                ParsedItchCommand cmd;
                cmd.action = ParsedAction::PartialCancel;
                cmd.stock_locate = bswap16(msg->stock_locate);
                cmd.order_id = bswap64(msg->order_reference_number);
                cmd.quantity = bswap32(msg->canceled_shares);
                return cmd;
            }
            case 'D': {
                const auto* msg = reinterpret_cast<const OrderDeleteMessage*>(buffer + 1);
                ParsedItchCommand cmd;
                cmd.action = ParsedAction::Delete;
                cmd.stock_locate = bswap16(msg->stock_locate);
                cmd.order_id = bswap64(msg->order_reference_number);
                return cmd;
            }
            case 'U': {
                const auto* msg = reinterpret_cast<const OrderReplaceMessage*>(buffer + 1);
                ParsedItchCommand cmd;
                cmd.action = ParsedAction::Replace;
                cmd.stock_locate = bswap16(msg->stock_locate);
                cmd.original_order_id = bswap64(msg->original_order_reference_number);
                cmd.order_id = bswap64(msg->new_order_reference_number);
                cmd.quantity = bswap32(msg->shares);
                cmd.price = static_cast<Price>(bswap32(msg->price));
                return cmd;
            }
            default:
                return ParsedItchCommand{};
        }
    }
};

}
}
