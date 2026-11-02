#pragma once

#include "Types.hpp"
#include "Order.hpp"
#include "FreeList.hpp"
#include "PriceLevel.hpp"

#include <cstdint>
#include <cassert>
#include <algorithm>
#include <array>
#include <limits>
#include <bit>

namespace orderbook {

/**
 * Uses a flat array mapping coupled with a 2-level hierarchical 64-bit bitmask index
 * for deterministic O(1) matching, insertions, cancellations, and best-price tracking.
 */
template <size_t MaxOrders = 100000, Price MaxPrice = 200000>
class OrderBook {
private:
    static constexpr size_t BITS_PER_WORD = 64;
    static constexpr size_t L1_SIZE = (MaxPrice + BITS_PER_WORD) / BITS_PER_WORD;
    static constexpr size_t L2_SIZE = (L1_SIZE + BITS_PER_WORD) / BITS_PER_WORD;

public:
    static constexpr Price INVALID_ASK_PRICE = MaxPrice + 1;

public:
    OrderBook() {
        order_lookup_.fill(nullptr);
        bid_bitmask_.fill(0);
        ask_bitmask_.fill(0);
        bid_summary_.fill(0);
        ask_summary_.fill(0);
    }

    // Disabling copy/move operations
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    [[nodiscard]] bool add_order(OrderId id, Price price, Quantity qty, Side side, OrderType type = OrderType::Limit) noexcept {
        if (id == 0 || id > MaxOrders || price == 0 || price > MaxPrice || qty == 0) [[unlikely]] return false;
        if (order_lookup_[id] != nullptr) [[unlikely]] return false;

        Order* incoming = free_list_.allocate();
        if (!incoming) [[unlikely]] return false;

        *incoming = Order(id, price, qty, side, type);
        match_order(incoming);

        if (incoming->quantity > 0 && incoming->type == OrderType::Limit) {
            const Price p = incoming->price;
            if (incoming->side == Side::Buy) {
                bids_[p].push_back(incoming);
                set_bid_bit(p);
                if (p > best_bid_price_) best_bid_price_ = p;
            }
            else {
                asks_[p].push_back(incoming);
                set_ask_bit(p);
                if (p < best_ask_price_) best_ask_price_ = p;
            }

            order_lookup_[id] = incoming;
            ++active_orders_count_;
        }
        else {
            free_list_.deallocate(incoming);
        }

        return true;
    }

    [[nodiscard]] bool cancel_order(OrderId id) noexcept {
        if (id == 0 || id > MaxOrders) [[unlikely]] return false;

        Order* order = order_lookup_[id];
        if (order == nullptr) [[unlikely]] return false;

        const Price p = order->price;
        order_lookup_[id] = nullptr;

        if (order->side == Side::Buy) {
            bids_[p].remove(order);
            if (bids_[p].empty()) {
                clear_bid_bit(p);
                if (p == best_bid_price_) update_best_bid();
            }
        } else {
            asks_[p].remove(order);
            if (asks_[p].empty()) {
                clear_ask_bit(p);
                if (p == best_ask_price_) update_best_ask();
            }
        }

        free_list_.deallocate(order);
        --active_orders_count_;
        return true;
    }

    [[nodiscard]] Price best_bid() const noexcept { return best_bid_price_; }
    [[nodiscard]] Price best_ask() const noexcept { return best_ask_price_; }
    [[nodiscard]] bool empty() const noexcept { return active_orders_count_ == 0; }
    [[nodiscard]] size_t size() const noexcept { return active_orders_count_; }

private:
    void match_order(Order* incoming) noexcept {
        if (incoming->side == Side::Buy) {
            while (best_ask_price_ <= incoming->price && incoming->quantity > 0 && best_ask_price_ <= MaxPrice) {
                PriceLevel& level = asks_[best_ask_price_];
                Order* resting = level.head();

                while (resting && incoming->quantity > 0) {
                    Order* next_resting = resting->next;
                    Quantity fill_qty = std::min(incoming->quantity, resting->quantity);

                    incoming->quantity -= fill_qty;
                    resting->quantity -= fill_qty;

                    if (resting->quantity == 0) {
                        level.remove(resting);
                        order_lookup_[resting->id] = nullptr;
                        free_list_.deallocate(resting);
                        --active_orders_count_;
                    }
                    resting = next_resting;
                }

                if (level.empty()) {
                    clear_ask_bit(best_ask_price_);
                    update_best_ask();
                }
            }
        } else {
            while (best_bid_price_ >= incoming->price && incoming->quantity > 0 && best_bid_price_ > 0) {
                PriceLevel& level = bids_[best_bid_price_];
                Order* resting = level.head();

                while (resting && incoming->quantity > 0) {
                    Order* next_resting = resting->next;
                    Quantity fill_qty = std::min(incoming->quantity, resting->quantity);

                    incoming->quantity -= fill_qty;
                    resting->quantity -= fill_qty;

                    if (resting->quantity == 0) {
                        level.remove(resting);
                        order_lookup_[resting->id] = nullptr;
                        free_list_.deallocate(resting);
                        --active_orders_count_;
                    }
                    resting = next_resting;
                }

                if (level.empty()) {
                    clear_bid_bit(best_bid_price_);
                    update_best_bid();
                }
            }
        }
    }

    inline void update_best_bid() noexcept {
        if (best_bid_price_ == 0) return;

        size_t l1_idx = best_bid_price_ / BITS_PER_WORD;
        uint64_t l1_word = bid_bitmask_[l1_idx];

        if (l1_word != 0) {
            best_bid_price_ = static_cast<Price>((l1_idx * BITS_PER_WORD) + (BITS_PER_WORD - 1 - std::countl_zero(l1_word)));
            return;
        }

        size_t l2_idx = l1_idx / BITS_PER_WORD;
        size_t l2_bit_offset = l1_idx % BITS_PER_WORD;

        uint64_t l2_word = bid_summary_[l2_idx] & ((1ULL << l2_bit_offset) - 1ULL);

        if (l2_word != 0) {
            size_t target_l1_idx = (l2_idx * BITS_PER_WORD) + (BITS_PER_WORD - 1 - std::countl_zero(l2_word));
            uint64_t target_l1_word = bid_bitmask_[target_l1_idx];
            best_bid_price_ = static_cast<Price>((target_l1_idx * BITS_PER_WORD) + (BITS_PER_WORD - 1 - std::countl_zero(target_l1_word)));
            return;
        }

        while (l2_idx > 0) {
            --l2_idx;
            if (bid_summary_[l2_idx] != 0) {
                size_t target_l1_idx = (l2_idx * BITS_PER_WORD) + (BITS_PER_WORD - 1 - std::countl_zero(bid_summary_[l2_idx]));
                uint64_t target_l1_word = bid_bitmask_[target_l1_idx];
                best_bid_price_ = static_cast<Price>((target_l1_idx * BITS_PER_WORD) + (BITS_PER_WORD - 1 - std::countl_zero(target_l1_word)));
                return;
            }
        }

        best_bid_price_ = 0;
    }

    inline void update_best_ask() noexcept {
        if (best_ask_price_ > MaxPrice) return;

        size_t l1_idx = best_ask_price_ / BITS_PER_WORD;
        uint64_t l1_word = ask_bitmask_[l1_idx];

        if (l1_word != 0) {
            best_ask_price_ = static_cast<Price>((l1_idx * BITS_PER_WORD) + std::countr_zero(l1_word));
            if (best_ask_price_ > MaxPrice) [[unlikely]] best_ask_price_ = INVALID_ASK_PRICE;
            return;
        }

        size_t l2_idx = l1_idx / BITS_PER_WORD;
        size_t l2_bit_offset = l1_idx % BITS_PER_WORD;

        uint64_t l2_word = (l2_bit_offset == 63) ? 0ULL : (ask_summary_[l2_idx] & (~0ULL << (l2_bit_offset + 1)));

        if (l2_word != 0) {
            size_t target_l1_idx = (l2_idx * BITS_PER_WORD) + std::countr_zero(l2_word);
            uint64_t target_l1_word = ask_bitmask_[target_l1_idx];
            best_ask_price_ = static_cast<Price>((target_l1_idx * BITS_PER_WORD) + std::countr_zero(target_l1_word));
            if (best_ask_price_ > MaxPrice) [[unlikely]] best_ask_price_ = INVALID_ASK_PRICE;
            return;
        }
        
        while (++l2_idx < L2_SIZE) {
            if (ask_summary_[l2_idx] != 0) {
                size_t target_l1_idx = (l2_idx * BITS_PER_WORD) + std::countr_zero(ask_summary_[l2_idx]);
                uint64_t target_l1_word = ask_bitmask_[target_l1_idx];
                best_ask_price_ = static_cast<Price>((target_l1_idx * BITS_PER_WORD) + std::countr_zero(target_l1_word));
                if (best_ask_price_ > MaxPrice) [[unlikely]] best_ask_price_ = INVALID_ASK_PRICE;
                return;
            }
        }

        best_ask_price_ = INVALID_ASK_PRICE;
    }

    inline void set_bid_bit(Price price) noexcept {
        const size_t l1_idx = price / BITS_PER_WORD;
        bid_bitmask_[l1_idx] |= (1ULL << (price % BITS_PER_WORD));
        bid_summary_[l1_idx / BITS_PER_WORD] |= (1ULL << (l1_idx % BITS_PER_WORD));
    }

    inline void clear_bid_bit(Price price) noexcept {
        const size_t l1_idx = price / BITS_PER_WORD;
        bid_bitmask_[l1_idx] &= ~(1ULL << (price % BITS_PER_WORD));
        if (bid_bitmask_[l1_idx] == 0) {
            bid_summary_[l1_idx / BITS_PER_WORD] &= ~(1ULL << (l1_idx % BITS_PER_WORD));
        }
    }

    inline void set_ask_bit(Price price) noexcept {
        const size_t l1_idx = price / BITS_PER_WORD;
        ask_bitmask_[l1_idx] |= (1ULL << (price % BITS_PER_WORD));
        ask_summary_[l1_idx / BITS_PER_WORD] |= (1ULL << (l1_idx % BITS_PER_WORD));
    }

    inline void clear_ask_bit(Price price) noexcept {
        const size_t l1_idx = price / BITS_PER_WORD;
        ask_bitmask_[l1_idx] &= ~(1ULL << (price % BITS_PER_WORD));
        if (ask_bitmask_[l1_idx] == 0) {
            ask_summary_[l1_idx / BITS_PER_WORD] &= ~(1ULL << (l1_idx % BITS_PER_WORD));
        }
    }

private:
    FreeList<MaxOrders> free_list_;

    std::array<Order*, MaxOrders + 1> order_lookup_;
    std::array<PriceLevel, MaxPrice + 1> bids_;
    std::array<PriceLevel, MaxPrice + 1> asks_;

    // Hierarchical Bitmask Structures
    std::array<uint64_t, L1_SIZE> bid_bitmask_;
    std::array<uint64_t, L1_SIZE> ask_bitmask_;

    std::array<uint64_t, L2_SIZE> bid_summary_;
    std::array<uint64_t, L2_SIZE> ask_summary_;

    alignas(64) Price best_bid_price_{0};
    alignas(64) Price best_ask_price_{INVALID_ASK_PRICE};
    alignas(64) size_t active_orders_count_{0};
};

}