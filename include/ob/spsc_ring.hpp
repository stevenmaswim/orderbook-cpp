#pragma once

#include <atomic>
#include <cstddef>

// Bounded single-producer single-consumer ring buffer (upgrade E).
//
// Exactly one thread may call try_push and exactly one (other) thread may call
// try_pop. With that rule, no locks are needed:
//   - the producer is the only writer of tail_, the consumer the only writer
//     of head_, so each index has a single writer and needs no read-modify-write
//   - the producer writes the slot, then publishes it with a release store of
//     tail_; the consumer's acquire load of tail_ makes that slot write visible
//     before it reads the slot. The same pairing on head_ tells the producer a
//     slot is free to reuse.
// try_push and try_pop finish in a bounded number of steps whatever the other
// thread is doing (no loops, no CAS retries): wait-free per call. A caller that
// must not drop data spins on a full or empty ring; that spin is in the caller.
//
// Layout: each thread's index and its cached copy of the other thread's index
// sit together on their own cache-line-sized block, so the producer's writes do
// not invalidate the consumer's line and vice versa (false sharing). `Align`
// is a template parameter only so the benchmark can build a deliberately
// unpadded ring and measure the difference.
namespace ob {

// The M5 reports a 128-byte cache line (sysctl hw.cachelinesize), larger than
// the 64 bytes most x86 parts use; 128 covers both.
inline constexpr std::size_t kCacheLine = 128;

template <class T, std::size_t Capacity, std::size_t Align = kCacheLine>
class SpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "power-of-two capacity lets index & mask replace a modulo");
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "the ring relies on lock-free atomic indices");

public:
    // Producer thread only.
    bool try_push(const T& value) {
        const std::size_t tail = prod_.tail.load(std::memory_order_relaxed);  // we are its only writer
        if (tail - prod_.cached_head == Capacity) {
            // Looks full. Refresh our copy of the consumer's index; reading the
            // shared line only when needed is what keeps cross-core traffic low.
            prod_.cached_head = cons_.head.load(std::memory_order_acquire);
            if (tail - prod_.cached_head == Capacity) return false;
        }
        slots_[tail & kMask] = value;
        prod_.tail.store(tail + 1, std::memory_order_release);  // publish the slot
        return true;
    }

    // Consumer thread only.
    bool try_pop(T& out) {
        const std::size_t head = cons_.head.load(std::memory_order_relaxed);
        if (head == cons_.cached_tail) {
            cons_.cached_tail = prod_.tail.load(std::memory_order_acquire);
            if (head == cons_.cached_tail) return false;  // empty
        }
        out = slots_[head & kMask];
        cons_.head.store(head + 1, std::memory_order_release);  // free the slot
        return true;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    // Indices only grow; size_t wrap-around after 2^64 pushes is harmless
    // because only differences and the low bits are used.
    struct alignas(Align) Producer {
        std::atomic<std::size_t> tail{0};
        std::size_t cached_head = 0;
    };
    struct alignas(Align) Consumer {
        std::atomic<std::size_t> head{0};
        std::size_t cached_tail = 0;
    };

    Producer prod_;
    Consumer cons_;
    // Value-initialized once at construction. A slot is only ever read after
    // the producer wrote it, but starting from defined values costs one
    // memset up front and keeps gcc's maybe-uninitialized analysis quiet.
    alignas(Align) T slots_[Capacity]{};
};

// Tell the core we are spinning (lets an SMT sibling run, saves power).
inline void cpu_relax() {
#if defined(__aarch64__)
    asm volatile("yield");
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}

}  // namespace ob
