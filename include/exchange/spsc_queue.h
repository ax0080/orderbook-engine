#pragma once

#include "constants.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace exchange {

// Bounded single-producer / single-consumer ring buffer. Wait-free on both
// sides: each operation is a fixed number of loads and stores, no CAS loops.
//
// Cache-line traffic is what limits a cross-core queue, so:
//  - each index sits on its own cache line with the other side's cached copy,
//    and a side re-reads the shared index only when its copy says the queue
//    looks full (producer) or empty (consumer);
//  - the consumer hands slots back in batches rather than one at a time, so a
//    producer waiting on a full queue does not pull the consumer's index line
//    away on every message, and never writes into the line being read.
template <typename T, std::size_t Capacity>
    requires (Capacity >= 2 && (Capacity & (Capacity - 1)) == 0 &&
              std::is_nothrow_copy_assignable_v<T> &&
              std::is_nothrow_default_constructible_v<T>)
class SpscQueue {
public:
    SpscQueue() : slots_(std::make_unique<T[]>(Capacity)) {}

    SpscQueue(const SpscQueue&)            = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Producer thread only.
    bool try_push(const T& value) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_cache_ == Capacity) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail - head_cache_ == Capacity) return false;
        }
        slots_[tail & kMask] = value;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only. Returning false also releases every slot read so
    // far, so a consumer that keeps polling never holds space back.
    bool try_pop(T& out) noexcept {
        if (read_ == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (read_ == tail_cache_) {
                release();
                return false;
            }
        }
        out = slots_[read_ & kMask];
        ++read_;
        if (read_ - released_ >= kReleaseBatch) release();
        return true;
    }

    // Producer's view: may count up to kReleaseBatch - 1 slots the consumer
    // has read but not yet released. Approximate while the other side runs.
    std::size_t size() const noexcept {
        return tail_.load(std::memory_order_acquire) -
               head_.load(std::memory_order_acquire);
    }
    bool empty() const noexcept { return size() == 0; }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

    static constexpr std::size_t kReleaseBatch =
        std::clamp<std::size_t>(512 / sizeof(T), 1, Capacity / 4 ? Capacity / 4 : 1);

private:
    static constexpr std::size_t kMask = Capacity - 1;

    void release() noexcept {
        if (read_ != released_) {
            head_.store(read_, std::memory_order_release);
            released_ = read_;
        }
    }

    // Shared: written by the consumer, read by the producer when full.
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> head_{0};

    // Consumer-private.
    alignas(CACHE_LINE_SIZE) std::size_t read_ = 0;
    std::size_t released_   = 0;
    std::size_t tail_cache_ = 0;

    // Shared: written by the producer, read by the consumer when empty.
    alignas(CACHE_LINE_SIZE) std::atomic<std::size_t> tail_{0};

    // Producer-private.
    alignas(CACHE_LINE_SIZE) std::size_t head_cache_ = 0;

    alignas(CACHE_LINE_SIZE) std::unique_ptr<T[]> slots_;
};

} // namespace exchange
