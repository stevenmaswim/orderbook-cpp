// SPSC ring (upgrade E). The multi-threaded tests are the ones the `tsan`
// preset exists for: ThreadSanitizer flags any data race the memory orders
// fail to prevent.
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "ob/books.hpp"
#include "ob/op.hpp"
#include "ob/rng.hpp"
#include "ob/spsc_ring.hpp"

using namespace ob;

TEST(SpscRing, FifoAndCapacityOnOneThread) {
    SpscRing<int, 4> ring;
    int out = 0;
    EXPECT_FALSE(ring.try_pop(out));  // starts empty
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(ring.try_push(i));
    EXPECT_FALSE(ring.try_push(99));  // full at Capacity
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_FALSE(ring.try_pop(out));
}

TEST(SpscRing, WrapsAroundManyTimes) {
    SpscRing<std::uint64_t, 8> ring;
    std::uint64_t out = 0;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        ASSERT_TRUE(ring.try_push(i));
        ASSERT_TRUE(ring.try_pop(out));
        ASSERT_EQ(out, i);
    }
}

TEST(SpscRing, TwoThreadsDeliverEveryValueInOrder) {
    // A small ring forces constant full/empty transitions, which is where a
    // wrong memory order would show up.
    constexpr std::uint64_t kN = 1'000'000;
    auto ring = std::make_unique<SpscRing<std::uint64_t, 64>>();
    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= kN; ++i) {
            while (!ring->try_push(i)) cpu_relax();
        }
    });
    std::uint64_t expected = 1, got = 0, bad = 0;
    while (expected <= kN) {
        if (!ring->try_pop(got)) {
            cpu_relax();
            continue;
        }
        bad += got != expected;
        ++expected;
    }
    producer.join();
    EXPECT_EQ(bad, 0u);
}

TEST(SpscRing, PipelineThroughTheRingMatchesDirectCalls) {
    // Gateway thread -> ring -> matcher thread must produce exactly the events
    // that calling the book directly on one thread produces: the queue changes
    // where the work happens, never what the result is.
    Rng rng(5);
    std::vector<Op> ops;
    for (OrderId id = 1; id <= 50'000; ++id) {
        if (id > 10 && rng.percent(20)) {
            ops.push_back(cancel_op(id - 1 - rng.below(10)));
            continue;
        }
        const auto type = static_cast<OrderType>(rng.below(4));
        ops.push_back(submit_op({id, rng.percent(50) ? Side::Buy : Side::Sell, type,
                                 type == OrderType::Market ? kNoPrice : rng.uniform(95, 105),
                                 rng.uniform(1, 20)}));
    }
    const BookConfig cfg{1, 1000, 1 << 16};

    PoolArrayBook direct_book(cfg);
    VectorSink direct;
    for (const Op& op : ops) apply(direct_book, op, direct);

    auto ring = std::make_unique<SpscRing<Op, 256>>();
    PoolArrayBook piped_book(cfg);
    VectorSink piped;
    std::thread matcher([&] {
        Op op;
        for (std::size_t done = 0; done < ops.size();) {
            if (!ring->try_pop(op)) {
                cpu_relax();
                continue;
            }
            apply(piped_book, op, piped);
            ++done;
        }
    });
    for (const Op& op : ops) {
        while (!ring->try_push(op)) cpu_relax();
    }
    matcher.join();  // join makes the matcher's writes visible here
    EXPECT_EQ(piped.events, direct.events);
    EXPECT_EQ(piped_book.snapshot(), direct_book.snapshot());
}
