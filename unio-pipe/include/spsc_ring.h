#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace unio {

// Fixed-capacity single-producer single-consumer ring with
// drop-oldest semantics on overflow. Depth 2 per the plan: one
// frame being consumed, one frame waiting. Over-depth would let
// stale frames queue up behind a stalled consumer — we want the
// freshest frame on the wire, not the oldest.
//
// Lock-free in the happy path (load-acquire + store-release on
// head / tail indices). The slot payload is a unique_ptr<T> so
// ownership moves atomically and the consumer never blocks on
// memory release — the producer-side replace() returns the
// previous occupant to the caller, who destroys it on their
// own thread at their leisure.
template <typename T, std::size_t Capacity = 2>
class SpscRing {
    static_assert(Capacity >= 2, "ring depth 1 has no slack");

public:
    // Producer side. Drops the previous occupant of the target
    // slot (if any) — returned to the caller. Callers who care
    // about the drop count can sum up the returned values; most
    // just let the unique_ptr destruct inline.
    std::unique_ptr<T> replace(std::unique_ptr<T> item) {
        const auto head = head_.load(std::memory_order_relaxed);
        auto& slot = slots_[head % Capacity];
        auto prev = std::move(slot);
        slot = std::move(item);
        head_.store(head + 1, std::memory_order_release);
        return prev;
    }

    // Consumer side. Returns nullptr when the ring is empty —
    // caller typically sleeps (or waits on an event) and retries.
    std::unique_ptr<T> pop() {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);
        if (tail == head) return nullptr;
        // Skip any slots the producer has already overwritten —
        // the ring only promises the LATEST frame, not every
        // frame. Fast-forward tail to head-Capacity, then read
        // the freshest entry.
        auto start = tail;
        if (head - tail > Capacity) {
            start = head - Capacity;
        }
        auto& slot = slots_[(head - 1) % Capacity];
        auto item = std::move(slot);
        tail_.store(head, std::memory_order_release);
        (void)start;
        return item;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire)
             == tail_.load(std::memory_order_acquire);
    }

private:
    std::array<std::unique_ptr<T>, Capacity> slots_;
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
};

}  // namespace unio
