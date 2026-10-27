// OrderBook.hpp

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
 * Uses a flat array mapping coupled with a hierarchical 64-bit bitmask index
 * for deterministic O(1) matching, insertions, cancellations, and best-price tracking.
 */
template <size_t MaxOrders = 100000, Price MaxPrice = 200000>
class OrderBook {
private:
    static constexpr size_t BITS_PER_WORD = 64;
    static constexpr size_t BITMASK_SIZE = (MaxPrice + BITS_PER_WORD) / BITS_PER_WORD;

public:
    static constexpr Price INVALID_ASK_PRICE = MaxPrice + 1;

public:
    OrderBook() {
        order_lookup_.fill(nullptr);
        bid_bitmask_.fill(0);
        ask_bitmask_.fill(0);
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
                set_bit(bid_bitmask_, p);
                if (p > best_bid_price_) best_bid_price_ = p;
            }
            else {
                asks_[p].push_back(incoming);
                set_bit(ask_bitmask_, p);
                if (p < best_ask_price_) best_ask_price_ = p;
            }

            order_lookup_[id] = incoming;
            ++active_orders_count_;
        }
        else free_list_.deallocate(incoming);

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
                clear_bit(bid_bitmask_, p);
                if (p == best_bid_price_) update_best_bid();
            }
        } else {
            asks_[p].remove(order);
            if (asks_[p].empty()) {
                clear_bit(ask_bitmask_, p);
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
                    clear_bit(ask_bitmask_, best_ask_price_);
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
                    clear_bit(bid_bitmask_, best_bid_price_);
                    update_best_bid();
                }
            }
        }
    }

    inline void update_best_bid() noexcept {
        if (best_bid_price_ == 0) return;

        size_t word_idx = best_bid_price_ / BITS_PER_WORD;
        uint64_t word = bid_bitmask_[word_idx];

        if (word != 0) {
            best_bid_price_ = static_cast<Price>((word_idx * BITS_PER_WORD) + (BITS_PER_WORD - 1 - std::countl_zero(word)));
            return;
        }

        while (word_idx > 0) {
            --word_idx;
            if (bid_bitmask_[word_idx] != 0) {
                best_bid_price_ = static_cast<Price>((word_idx * BITS_PER_WORD) + (BITS_PER_WORD - 1 - std::countl_zero(bid_bitmask_[word_idx])));
                return;
            }
        }

        best_bid_price_ = 0;
    }

    inline void update_best_ask() noexcept {
        if (best_ask_price_ > MaxPrice) return;

        size_t word_idx = best_ask_price_ / BITS_PER_WORD;
        uint64_t word = ask_bitmask_[word_idx];

        if (word != 0) {
            best_ask_price_ = static_cast<Price>((word_idx * BITS_PER_WORD) + std::countr_zero(word));
            if (best_ask_price_ > MaxPrice) [[unlikely]] best_ask_price_ = INVALID_ASK_PRICE;
            return;
        }

        while (++word_idx < BITMASK_SIZE) {
            if (ask_bitmask_[word_idx] != 0) {
                best_ask_price_ = static_cast<Price>((word_idx * BITS_PER_WORD) + std::countr_zero(ask_bitmask_[word_idx]));
                if (best_ask_price_ > MaxPrice) [[unlikely]] best_ask_price_ = INVALID_ASK_PRICE;
                return;
            }
        }

        best_ask_price_ = INVALID_ASK_PRICE;
    }

    static inline void set_bit(std::array<uint64_t, BITMASK_SIZE>& mask, Price price) noexcept {
        mask[price / BITS_PER_WORD] |= (1ULL << (price % BITS_PER_WORD));
    }

    static inline void clear_bit(std::array<uint64_t, BITMASK_SIZE>& mask, Price price) noexcept {
        mask[price / BITS_PER_WORD] &= ~(1ULL << (price % BITS_PER_WORD));
    }

private:
    FreeList<MaxOrders> free_list_;

    std::array<Order*, MaxOrders + 1> order_lookup_;
    std::array<PriceLevel, MaxPrice + 1> bids_;
    std::array<PriceLevel, MaxPrice + 1> asks_;

    std::array<uint64_t, BITMASK_SIZE> bid_bitmask_;
    std::array<uint64_t, BITMASK_SIZE> ask_bitmask_;

    alignas(64) Price best_bid_price_{0};
    alignas(64) Price best_ask_price_{INVALID_ASK_PRICE};
    alignas(64) size_t active_orders_count_{0};
};

} 