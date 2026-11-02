// ItchParser.hpp

#pragma once

#include "Types.hpp"
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

#pragma pack(push, 1)
struct ItchHeader {
    uint16_t message_length;
    char message_type;
};

struct AddOrderMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;
    uint64_t order_reference_number;
    char buy_sell_indicator;
    uint32_t shares;
    char stock[8];
    uint32_t price;
};

struct OrderExecutedMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;
    uint64_t order_reference_number;
    uint32_t executed_shares;
    uint64_t match_number;
};

struct OrderCancelMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;
    uint64_t order_reference_number;
    uint32_t canceled_shares;
};

struct OrderDeleteMessage {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;
    uint64_t order_reference_number;
};
#pragma pack(pop)

enum class ParsedAction : uint8_t {
    Add,
    Cancel,
    Execute,
    Ignore
};

struct ParsedItchCommand {
    ParsedAction action{ParsedAction::Ignore};
    OrderId order_id{0};
    Price price{0};
    Quantity quantity{0};
    Side side{Side::Buy};
};

class ItchParser {
public:
    static inline ParsedItchCommand parse_message(const char* buffer) noexcept {
        const char msg_type = buffer[0];

        switch (msg_type) {
            case 'A': {
                const auto* msg = reinterpret_cast<const AddOrderMessage*>(buffer + 1);
                ParsedItchCommand cmd;
                cmd.action = ParsedAction::Add;
                cmd.order_id = bswap64(msg->order_reference_number);
                cmd.price = static_cast<Price>(bswap32(msg->price));
                cmd.quantity = bswap32(msg->shares);
                cmd.side = (msg->buy_sell_indicator == 'B') ? Side::Buy : Side::Sell;
                return cmd;
            }
            case 'X': {
                const auto* msg = reinterpret_cast<const OrderCancelMessage*>(buffer + 1);
                ParsedItchCommand cmd;
                cmd.action = ParsedAction::Cancel;
                cmd.order_id = bswap64(msg->order_reference_number);
                cmd.quantity = bswap32(msg->canceled_shares);
                return cmd;
            }
            case 'D': {
                const auto* msg = reinterpret_cast<const OrderDeleteMessage*>(buffer + 1);
                ParsedItchCommand cmd;
                cmd.action = ParsedAction::Cancel;
                cmd.order_id = bswap64(msg->order_reference_number);
                return cmd;
            }
            default:
                return ParsedItchCommand{ParsedAction::Ignore, 0, 0, 0, Side::Buy};
        }
    }
};

}
}
