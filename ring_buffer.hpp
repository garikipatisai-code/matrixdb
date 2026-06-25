#pragma once

#include "types.hpp"
#include <atomic>
#include <array>
#include <utility>

/**
 * @brief Ultra-low latency, lock-free, zero-allocation SPSC Ring Buffer.
 * Enforces power-of-two capacities to replace expensive modulo operations with bitwise AND masking.
 */
template <typename T, size_t Capacity>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "SPSCRingBuffer Capacity must be a power of two.");
    static_assert(Capacity >= 2, "SPSCRingBuffer Capacity must be at least two elements.");

public:
    SPSCRingBuffer()
        : head_(0)
        , tail_(0)
        , cached_head_(0)
        , cached_tail_(0) {}

    ~SPSCRingBuffer() = default;

    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer(SPSCRingBuffer&&) = delete;
    SPSCRingBuffer& operator=(SPSCRingBuffer&&) = delete;

    /**
     * @brief Emplaces a new item at the tail of the buffer. Only the Producer may call this method.
     */
    template <typename... Args>
    bool try_emplace(Args&&... args) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);

        // Check local cached head before loading the atomic head_ cursor from shared memory
        if (current_tail - cached_head_ >= Capacity) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (current_tail - cached_head_ >= Capacity) {
                return false; // Queue is genuinely full
            }
        }

        const size_t index = current_tail & MASK;
        buffer_[index] = T{std::forward<Args>(args)...};

        tail_.store(current_tail + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Pops an item from the head of the buffer. Only the Consumer may call this method.
     */
    bool try_pop(T& value) {
        const size_t current_head = head_.load(std::memory_order_relaxed);

        // Check local cached tail before loading the atomic tail_ cursor from shared memory
        if (current_head >= cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (current_head >= cached_tail_) {
                return false; // Queue is genuinely empty
            }
        }

        const size_t index = current_head & MASK;
        value = std::move(buffer_[index]);

        head_.store(current_head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] size_t size() const noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t h = head_.load(std::memory_order_relaxed);
        return (t >= h) ? (t - h) : 0;
    }

private:
    static constexpr size_t MASK = Capacity - 1;

    // Isolate variables on separate cache lines to eliminate false sharing
    alignas(MATRIX_CACHE_LINE) std::array<T, Capacity> buffer_{};
    alignas(MATRIX_CACHE_LINE) std::atomic<size_t> head_;
    alignas(MATRIX_CACHE_LINE) std::atomic<size_t> tail_;

    // Thread-local shadow indices
    alignas(MATRIX_CACHE_LINE) size_t cached_head_;
    alignas(MATRIX_CACHE_LINE) size_t cached_tail_;
};
