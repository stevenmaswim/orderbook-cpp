// Upgrade B pieces on their own: the flat id table and pool capacity.
#include <gtest/gtest.h>

#include <unordered_map>
#include <vector>

#include "ob/books.hpp"
#include "ob/flat_id_map.hpp"
#include "ob/rng.hpp"

using namespace ob;

TEST(FlatIdMap, FindsWhatWasInsertedAndForgetsWhatWasErased) {
    FlatIdMap<int> m(16);
    int a = 1, b = 2;
    m.insert(10, &a);
    m.insert(20, &b);
    EXPECT_EQ(m.find(10), &a);
    EXPECT_EQ(m.find(20), &b);
    EXPECT_EQ(m.find(30), nullptr);
    m.erase(10);
    EXPECT_EQ(m.find(10), nullptr);
    EXPECT_EQ(m.find(20), &b);
    EXPECT_EQ(m.size(), 1u);
}

TEST(FlatIdMap, IdZeroIsAValidKey) {
    // Emptiness is marked by a null value, not a reserved key.
    FlatIdMap<int> m(4);
    int a = 1;
    m.insert(0, &a);
    EXPECT_EQ(m.find(0), &a);
}

TEST(FlatIdMap, MatchesUnorderedMapUnderRandomChurn) {
    // A small differential test: the same random inserts and erases applied to
    // FlatIdMap and std::unordered_map must agree on every lookup. Tiny key
    // range + near-full table forces long probe runs and many backward shifts.
    constexpr std::size_t kMax = 512;
    FlatIdMap<int> flat(kMax);
    std::unordered_map<OrderId, int*> ref;
    std::vector<int> values(2048);
    Rng rng(7);
    for (int step = 0; step < 200'000; ++step) {
        const OrderId id = rng.below(2048);
        const bool present = ref.contains(id);
        if (!present && ref.size() < kMax && rng.percent(55)) {
            flat.insert(id, &values[id]);
            ref[id] = &values[id];
        } else if (present && rng.percent(50)) {
            flat.erase(id);
            ref.erase(id);
        }
        const OrderId probe = rng.below(2048);
        auto it = ref.find(probe);
        ASSERT_EQ(flat.find(probe), it == ref.end() ? nullptr : it->second) << "step " << step;
        ASSERT_EQ(flat.size(), ref.size());
    }
}

TEST(PoolStorage, FullPoolRejectsNewLimitOrdersUntilSpaceFrees) {
    BookConfig cfg{1, 1000, 2};  // room for two resting orders
    PoolArrayBook book(cfg);
    VectorSink s;
    book.submit({1, Side::Buy, OrderType::Limit, 100, 1}, s);
    book.submit({2, Side::Buy, OrderType::Limit, 100, 1}, s);
    s.events.clear();
    book.submit({3, Side::Buy, OrderType::Limit, 99, 1}, s);
    EXPECT_EQ(s.events, (std::vector<Event>{Event::rejected(3, Reason::CapacityExceeded)}));

    // Orders that never rest do not need a node, so they still work.
    s.events.clear();
    book.submit({4, Side::Sell, OrderType::IOC, 100, 1}, s);
    EXPECT_EQ(s.events.size(), 2u);  // Accepted + Trade
    EXPECT_EQ(s.events[1].type, EventType::Trade);

    // The fill freed a node, so a limit order fits again.
    s.events.clear();
    book.submit({5, Side::Buy, OrderType::Limit, 99, 1}, s);
    EXPECT_EQ(s.events[0].type, EventType::Accepted);
    EXPECT_EQ(book.resting_count(), 2u);
}

TEST(PoolStorage, ModifyReplaceWorksWhenPoolIsFull) {
    // Cancel-replace frees the node before it re-rests, so it cannot fail.
    BookConfig cfg{1, 1000, 1};
    PoolArrayBook book(cfg);
    VectorSink s;
    book.submit({1, Side::Buy, OrderType::Limit, 100, 5}, s);
    s.events.clear();
    book.modify(1, 101, 9, s);
    EXPECT_EQ(s.events, (std::vector<Event>{Event::modified(1, Side::Buy, 101, 9, Reason::LostPriority)}));
}
