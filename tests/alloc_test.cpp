// Proof of "zero heap allocation on the hot path" for PoolArrayBook.
//
// This binary replaces the global operator new / operator delete (every
// variant: plain, array, nothrow, aligned) with versions that count calls and
// then forward to malloc/free. Anything that allocates through new, including
// every standard container, goes through these. The test builds the book and
// the op stream first, resets the counter, replays the ops, and requires the
// count to still be zero.
//
// It lives in its own executable because replacing global new affects the
// whole program.
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <new>
#include <vector>

#include "ob/books.hpp"
#include "ob/op.hpp"
#include "ob/rng.hpp"
#include "workloads.hpp"

namespace {
std::atomic<std::size_t> g_news{0};

void* counted_alloc(std::size_t n, std::size_t align = 0) {
    g_news.fetch_add(1, std::memory_order_relaxed);
    if (n == 0) n = 1;
    void* p = nullptr;
    if (align > alignof(std::max_align_t)) {
        // aligned_alloc needs the size to be a multiple of the alignment.
        p = std::aligned_alloc(align, (n + align - 1) / align * align);
    } else {
        p = std::malloc(n);
    }
    if (!p) throw std::bad_alloc();
    return p;
}
}  // namespace

void* operator new(std::size_t n) { return counted_alloc(n); }
void* operator new[](std::size_t n) { return counted_alloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return counted_alloc(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return counted_alloc(n); } catch (...) { return nullptr; }
}
void* operator new(std::size_t n, std::align_val_t a) { return counted_alloc(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return counted_alloc(n, static_cast<std::size_t>(a)); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

using namespace ob;

namespace {

// Every op kind, every order type, plus invalid input, cancels of dead ids and
// modifies both ways: the hot path is more than the happy path.
std::vector<Op> mixed_ops(std::uint64_t seed, std::size_t n) {
    Rng rng(seed);
    std::vector<Op> ops;
    ops.reserve(n);
    OrderId next = 1;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t r = rng.below(100);
        if (r < 60 || next < 10) {
            const std::uint64_t t = rng.below(4);
            const auto type = static_cast<OrderType>(t);
            const Price px = type == OrderType::Market ? kNoPrice : rng.uniform(9990, 10010);
            ops.push_back(submit_op({next++, rng.percent(50) ? Side::Buy : Side::Sell, type, px,
                                     rng.percent(1) ? 0 : rng.uniform(1, 50)}));
        } else if (r < 80) {
            ops.push_back(cancel_op(next - 1 - rng.below(std::min<OrderId>(next - 1, 50))));
        } else {
            ops.push_back(modify_op(next - 1 - rng.below(std::min<OrderId>(next - 1, 50)),
                                    rng.uniform(9990, 10010), rng.uniform(1, 50)));
        }
    }
    return ops;
}

template <class Book>
std::size_t allocations_during_replay(const std::vector<Op>& ops) {
    const BookConfig cfg{1, 20'000, 1 << 18};
    Book book(cfg);  // construction may allocate (slab, table, ladder): that is setup
    CountingSink sink;
    const std::size_t before = g_news.load();
    for (const Op& op : ops) apply(book, op, sink);
    const std::size_t after = g_news.load();
    EXPECT_GT(sink.trades, 0u);  // make sure the replay really matched orders
    return after - before;
}

}  // namespace

TEST(ZeroAllocation, CounterSeesAllocations) {
    // Positive control: if this failed, a zero below would prove nothing.
    const std::size_t before = g_news.load();
    auto* v = new std::vector<int>(100);
    delete v;
    EXPECT_GE(g_news.load() - before, 2u);
}

TEST(ZeroAllocation, HeapBookAllocatesOnTheHotPath) {
    // Second control: the non-pooled book should show allocations.
    const auto ops = obbench::generate_shape({3, 20'000, 9950, 10050});
    EXPECT_GT(allocations_during_replay<IntrusiveArrayBook>(ops), 1000u);
}

TEST(ZeroAllocation, PoolArrayBookNeverAllocatesOnShapeWorkload) {
    const auto ops = obbench::generate_shape({3, 200'000, 9950, 10050});
    EXPECT_EQ(allocations_during_replay<PoolArrayBook>(ops), 0u);
}

TEST(ZeroAllocation, PoolArrayBookNeverAllocatesOnMixedOps) {
    const auto ops = mixed_ops(11, 200'000);
    EXPECT_EQ(allocations_during_replay<PoolArrayBook>(ops), 0u);
}
