// include/tt/concurrency/spsc_queue.hpp
//
// A single-producer single-consumer bounded queue built on a power-of-two
// ring buffer. Designed for low-latency message passing between threads.
//
// Memory ordering:
//   - The producer publishes new tail with release.
//   - The consumer reads head with acquire; loads tail with acquire too.
//
// This is single-producer / single-consumer; for the gateway<->worker path
// we use one queue per direction.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace tt {

template <typename T, std::size_t N>
class SPSCQueue {
    static_assert(N > 1, "SPSCQueue requires at least two slots.");
    static_assert((N & (N - 1U)) == 0U,
                  "SPSCQueue capacity must be a power of two.");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SPSCQueue payloads must be trivially copyable.");

    struct alignas(64) Cursor {
        std::atomic<std::uint64_t> value{0};
    };

public:
    SPSCQueue() noexcept = default;

    bool push(const T& value) noexcept {
        const std::uint64_t tail = tail_.value.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.value.load(std::memory_order_acquire);
        if ((tail - head) == N) return false;        // full
        buffer_[tail & MASK] = value;
        tail_.value.store(tail + 1U, std::memory_order_release);
        return true;
    }

    bool pop(T& value) noexcept {
        const std::uint64_t head = head_.value.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.value.load(std::memory_order_acquire);
        if (head == tail) return false;             // empty
        value = buffer_[head & MASK];
        head_.value.store(head + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.value.load(std::memory_order_acquire) ==
               tail_.value.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::uint64_t head = head_.value.load(std::memory_order_acquire);
        const std::uint64_t tail = tail_.value.load(std::memory_order_acquire);
        return static_cast<std::size_t>(tail - head);
    }

private:
    static constexpr std::size_t MASK = N - 1U;

    std::array<T, N> buffer_{};
    Cursor head_{};
    Cursor tail_{};
};

}  // namespace tt