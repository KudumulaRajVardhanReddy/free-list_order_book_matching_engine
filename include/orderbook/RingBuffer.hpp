// RingBuffer.hpp

#pragma once
#include <atomic>
#include <cstddef>
#include <new>
#include <utility>

namespace orderbook {

namespace detail {
#ifdef __cpp_lib_hardware_interference_size
using std::hardware_destructive_interference_size;
#else
constexpr std::size_t hardware_destructive_interference_size = 64;
#endif
}

template<typename T, size_t Capacity>
class SPSCRingBuffer {
    static_assert(((Capacity & (Capacity - 1)) == 0) && Capacity > 0, "Capacity must be a power of 2.");

public:
    SPSCRingBuffer() : write_index_(0), cached_write_index(0), read_index_(0), cached_read_index(0) {}

    // Disable copy and move semantics on l-value and r-values.
    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;

    // Disable copy and move semantics on r-values.
    SPSCRingBuffer(SPSCRingBuffer&&) = delete;
    SPSCRingBuffer& operator=(SPSCRingBuffer&&) = delete;

    // Push via copy.
    [[nodiscard]] bool push(const T& item) noexcept { return emplace(item); }
    // Push via move semantics
    [[nodiscard]] bool push(T&& item) noexcept { return emplace(std::move(item)); }

    template<typename... Args>
    [[nodiscard]] bool emplace(Args&&... args) noexcept {
        const size_t current_write = write_index_.load(std::memory_order_relaxed);

        if (current_write - cached_read_index >= Capacity) {
            cached_read_index = read_index_.load(std::memory_order_acquire);
            if (current_write - cached_read_index >= Capacity) return false;
        }

        buffer_[current_write & kMask].value = T(std::forward<Args>(args)...);
        write_index_.store(current_write + 1, std::memory_order_release);

        return true;
    }

    [[nodiscard]] bool pop(T& item) {
        const size_t current_read = read_index_.load(std::memory_order_relaxed);

        if (current_read == cached_write_index) {
            cached_write_index = write_index_.load(std::memory_order_acquire);
            if (current_read == cached_write_index) return false;
        }

        item = std::move(buffer_[current_read & kMask].value);
        read_index_.store(current_read + 1, std::memory_order_release);

        return true;
    }

    [[nodiscard]] constexpr size_t capacity() const noexcept { return Capacity; }

private:
    static constexpr size_t kMask = Capacity - 1;

    // custom struct to align individual slot in buffer.
    struct alignas(detail::hardware_destructive_interference_size) AlignedSlot { T value; };

    alignas(detail::hardware_destructive_interference_size) std::atomic<size_t> write_index_;
    size_t cached_write_index;
    alignas(detail::hardware_destructive_interference_size) std::atomic<size_t> read_index_;
    size_t cached_read_index;

    // align individual slots of buffer using AlignedSlot custom struct.
    alignas(detail::hardware_destructive_interference_size) AlignedSlot buffer_[Capacity];
};
}
