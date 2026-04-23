#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
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

// FIFO ring for encoded packets: preserves GOP order and never
// silently evicts a keyframe. The frame-ring SpscRing above can
// drop-latest because the sink always wants the freshest frame;
// the packet ring CANNOT, because each encoded packet references
// previous ones (IPPP reference chain). If we drop a packet
// mid-GOP, every subsequent packet decodes to garbage until the
// next IDR — and we don't emit periodic IDRs, so "until the next
// request_idr." That bug manifested on the Windows → Linux path
// as a sink that received 15 MB of bytes but never decoded a
// single frame because the first 253 KB IDR got evicted under a
// send-thread stall while the ring was only 4 deep.
//
// Semantics:
//   push(): append at head. If the ring is full AND the oldest
//           entry is a keyframe, refuse — return the NEW item
//           as "dropped." If the oldest is not a keyframe, evict
//           it. Either way, the caller increments its drop
//           counter from the returned non-null pointer.
//   pop():  read the OLDEST entry (true FIFO).
//
// Storage is a fixed std::array<unique_ptr, Capacity> indexed by
// head / tail counters. Zero heap allocations on every push/pop —
// only the payload T's destructor runs, never a deque-block alloc.
// A single std::mutex serialises the two threads:
//   - A pure lock-free SPSC ring can't implement the keyframe-
//     conditional drop cleanly. The producer must read the tail
//     slot to check `oldest->key_frame` and then maybe advance
//     tail; the consumer also writes tail via pop(). That's two
//     writers racing on the same slot (move-from-nullptr UB if
//     they collide), and the obvious CAS retries still race on
//     the payload itself. Keeping the mutex is a deliberate
//     choice — it's uncontended in practice (capacity 32, ring
//     is 0–1 deep in steady state) and costs ~25 ns of futex-op
//     per push on Linux, vs. 33 ms per frame at 30 fps.
//
// If the send thread ever becomes fast enough relative to encode
// that the mutex cost actually matters, revisit: change the
// semantics to pure drop-oldest (matches SpscRing) and track
// dropped keyframes via an atomic counter the consumer polls +
// issues request_idr on. That's cleaner but changes behaviour
// under full-queue — we don't need it today.
template <typename T, std::size_t Capacity = 32>
class FifoPacketRing {
    static_assert(Capacity >= 2, "ring depth 1 has no slack");

public:
    // item.key_frame must be readable. Returns the dropped item
    // if one got dropped (either evicted oldest, or rejected new
    // because oldest was a keyframe). nullptr on clean push.
    std::unique_ptr<T> push(std::unique_ptr<T> item) {
        std::lock_guard<std::mutex> lk(mu_);
        const std::size_t size = head_ - tail_;
        if (size < Capacity) {
            slots_[head_ % Capacity] = std::move(item);
            ++head_;
            return nullptr;
        }
        // Full. Protect keyframes at the oldest slot.
        auto& oldest_slot = slots_[tail_ % Capacity];
        if (oldest_slot && oldest_slot->key_frame) {
            // Drop the incoming P-frame rather than the anchor
            // keyframe. The new packet's decode would have
            // depended on the queued IDR anyway, so the loss
            // here is strictly smaller than losing the IDR.
            return item;
        }
        auto evicted = std::move(oldest_slot);
        ++tail_;
        slots_[head_ % Capacity] = std::move(item);
        ++head_;
        return evicted;
    }

    std::unique_ptr<T> pop() {
        std::lock_guard<std::mutex> lk(mu_);
        if (head_ == tail_) return nullptr;
        auto item = std::move(slots_[tail_ % Capacity]);
        ++tail_;
        return item;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lk(mu_);
        return head_ == tail_;
    }

private:
    mutable std::mutex mu_;
    std::array<std::unique_ptr<T>, Capacity> slots_{};
    std::uint64_t head_ = 0;  // next slot to write
    std::uint64_t tail_ = 0;  // next slot to read
};

}  // namespace unio
