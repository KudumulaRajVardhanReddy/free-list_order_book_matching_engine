// PriceLevel.hpp

#pragma once

#include "Order.hpp"
#include <cstddef>
#include <cassert>

namespace orderbook {

class PriceLevel {
public:
    PriceLevel() = default;

    void push_back(Order* order) noexcept {
        assert(order);

        order->next = nullptr;
        order->prev = tail_;

        if (tail_) tail_->next = order;
        else head_ = order;

        tail_ = order;
        total_volume_ += order->quantity;
        ++order_count_;
    }

    void remove(Order* order) noexcept {
        assert(order);
        assert(total_volume_ >= order->quantity);
        assert(order_count_ > 0);

        if (order->prev) order->prev->next = order->next;
        else head_ = order->next;

        if (order->next) order->next->prev = order->prev;
        else tail_ = order->prev;

        total_volume_ -= order->quantity;
        --order_count_;

        order->prev = nullptr;
        order->next = nullptr;
    }

    void reduce_volume(Quantity qty) noexcept {
        assert(total_volume_ >= qty);
        total_volume_ -= qty;
    }

    [[nodiscard]] Order* head() const noexcept { return head_; }
    [[nodiscard]] Order* tail() const noexcept { return tail_; }
    [[nodiscard]] Quantity quantity() const noexcept { return total_volume_; }
    [[nodiscard]] size_t order_count() const noexcept { return order_count_; }
    [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

private:
    Order* head_{nullptr};
    Order* tail_{nullptr};
    Quantity total_volume_{0};
    size_t order_count_{0};
};

}
