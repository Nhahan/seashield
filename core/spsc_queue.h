#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace seashield {

// Bounded lock-free single-producer/single-consumer ring buffer — the ONLY
// cross-thread channel between the I/O thread and the simulation thread
// (charter §4.6). Confining shared mutable state to this one queue is the
// whole threading argument: "not race-free by luck, but exactly one
// verifiable racy spot" (TSan covers the queue tests in CI).
//
// Design notes:
//  - Monotonic 64-bit head/tail counters masked into a power-of-two ring: no
//    index wraparound handling, no wasted slot, full = tail - head == capacity.
//  - Lamport queue with cached counterpart indices (Rigtorp's refinement):
//    the producer re-reads `head_` only when the ring looks full, the
//    consumer re-reads `tail_` only when it looks empty — one acquire load
//    per batch instead of per element in the steady state.
//  - alignas(64) keeps producer-written and consumer-written state on
//    different cache lines (false-sharing control). The cached index lives
//    WITH its owner: cached_tail_ is consumer-private, so it shares the
//    consumer's line, not the producer's.
//  - Slots are default-constructed T; pop moves out of the slot. Payloads at
//    this scale (snapshot/command structs) make placement-new storage an
//    unjustified complexity.
//
// Thread contract: push() from exactly one producer thread, pop() from
// exactly one consumer thread. size()/empty() are approximate snapshots, safe
// from any thread, for stats only. The single MPSC extension path (multiple
// I/O threads) is documented in the network design doc §7, not built.
template <typename T>
class SpscQueue {
 public:
  // capacity must be a power of two (mask arithmetic), at least 2.
  explicit SpscQueue(std::size_t capacity)
      : capacity_(capacity), mask_(capacity - 1), buffer_(capacity) {
    assert(capacity >= 2 && (capacity & (capacity - 1)) == 0);
  }

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  // Producer only. False = ring full (caller decides: drop + count, never
  // block). Takes an rvalue reference rather than by-value so a FAILED push
  // leaves the caller's object untouched — retriers don't lose the payload.
  bool push(T&& value) {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - cached_head_ == capacity_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail - cached_head_ == capacity_) {
        return false;
      }
    }
    buffer_[tail & mask_] = std::move(value);
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Consumer only. False = ring empty.
  bool pop(T& out) {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (cached_tail_ == head) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (cached_tail_ == head) {
        return false;
      }
    }
    out = std::move(buffer_[head & mask_]);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  std::size_t capacity() const { return capacity_; }

  // Approximate (two independent atomic reads) — stats/logging only.
  std::size_t size() const {
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    return tail >= head ? static_cast<std::size_t>(tail - head) : 0;
  }
  bool empty() const { return size() == 0; }

 private:
  static constexpr std::size_t kCacheLine = 64;

  const std::size_t capacity_;
  const std::size_t mask_;
  std::vector<T> buffer_;

  // Consumer-owned line: consumer writes head_ and its private cached_tail_.
  alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};
  std::uint64_t cached_tail_ = 0;

  // Producer-owned line: producer writes tail_ and its private cached_head_.
  alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};
  std::uint64_t cached_head_ = 0;
};

}  // namespace seashield
