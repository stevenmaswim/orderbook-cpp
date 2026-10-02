// ITCH 5.0 parsing and replay, on hand-built messages laid out per the spec.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "ob/books.hpp"
#include "ob/itch.hpp"

using namespace ob;
using namespace ob::itch;

namespace {

// Builds one length-prefixed ITCH message, big-endian, field by field.
class Msg {
public:
    explicit Msg(char type, std::uint16_t locate = 7) {
        u8(static_cast<std::uint8_t>(type));
        u16(locate);
        u16(0);              // tracking number
        u48(34'200'000'000'000ULL);  // timestamp: 09:30:00 in ns since midnight
    }
    Msg& u8(std::uint8_t v) { b_.push_back(v); return *this; }
    Msg& u16(std::uint16_t v) { return u8(static_cast<std::uint8_t>(v >> 8)).u8(static_cast<std::uint8_t>(v)); }
    Msg& u32(std::uint32_t v) { return u16(static_cast<std::uint16_t>(v >> 16)).u16(static_cast<std::uint16_t>(v)); }
    Msg& u48(std::uint64_t v) { return u16(static_cast<std::uint16_t>(v >> 32)).u32(static_cast<std::uint32_t>(v)); }
    Msg& u64(std::uint64_t v) { return u32(static_cast<std::uint32_t>(v >> 32)).u32(static_cast<std::uint32_t>(v)); }
    Msg& stock(const char* s) {
        for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>(*s ? *s++ : ' '));
        return *this;
    }
    // Append to a stream with the 2-byte length prefix.
    void into(std::vector<std::uint8_t>& out) const {
        out.push_back(static_cast<std::uint8_t>(b_.size() >> 8));
        out.push_back(static_cast<std::uint8_t>(b_.size()));
        out.insert(out.end(), b_.begin(), b_.end());
    }
    std::size_t size() const { return b_.size(); }

private:
    std::vector<std::uint8_t> b_;
};

Msg add(std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t price) {
    Msg m('A');
    m.u64(ref).u8(static_cast<std::uint8_t>(side)).u32(shares).stock("AAPL").u32(price);
    return m;
}
Msg executed(std::uint64_t ref, std::uint32_t shares) {
    Msg m('E');
    m.u64(ref).u32(shares).u64(1);  // match number
    return m;
}
Msg cancel(std::uint64_t ref, std::uint32_t shares) {
    Msg m('X');
    m.u64(ref).u32(shares);
    return m;
}
Msg del(std::uint64_t ref) {
    Msg m('D');
    m.u64(ref);
    return m;
}
Msg replace(std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t shares, std::uint32_t price) {
    Msg m('U');
    m.u64(old_ref).u64(new_ref).u32(shares).u32(price);
    return m;
}

using Book = IntrusiveMapBook;

struct Feed {
    std::vector<std::uint8_t> bytes;
    Feed& operator<<(const Msg& m) {
        m.into(bytes);
        return *this;
    }
    Replayer<Book> run() const {
        Replayer<Book> r{BookConfig{1, 2'000'000'000}};
        for_each_message(bytes.data(), bytes.size(),
                         [&](const std::uint8_t* m, std::size_t len) { r.on_message(m, len); });
        return r;
    }
};

}  // namespace

TEST(Itch, BigEndianReaders) {
    const std::uint8_t b[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    EXPECT_EQ(be16(b), 0x0102u);
    EXPECT_EQ(be32(b), 0x01020304u);
    EXPECT_EQ(be48(b), 0x010203040506ULL);
    EXPECT_EQ(be64(b), 0x0102030405060708ULL);
}

TEST(Itch, MessageBuilderMatchesSpecLengths) {
    EXPECT_EQ(add(1, 'B', 1, 1).size(), kLenAdd);
    EXPECT_EQ(executed(1, 1).size(), kLenExecuted);
    EXPECT_EQ(cancel(1, 1).size(), kLenCancel);
    EXPECT_EQ(del(1).size(), kLenDelete);
    EXPECT_EQ(replace(1, 2, 1, 1).size(), kLenReplace);
}

TEST(Itch, AddRestsWithoutMatchingEvenIfCrossed) {
    // Replay trusts the feed: a crossed add (as can happen around an auction)
    // just rests; matching is the exchange's job, not the replayer's.
    Feed f;
    f << add(1, 'B', 100, 1'500'000) << add(2, 'S', 50, 1'490'000);
    auto r = f.run();
    const Book* b = r.find_book(7);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->best_bid(), 1'500'000);
    EXPECT_EQ(b->best_ask(), 1'490'000);
    EXPECT_EQ(r.resting_orders(), 2u);
    EXPECT_EQ(r.stats().applied, 2u);
}

TEST(Itch, ExecutionsReduceThenRemove) {
    Feed f;
    f << add(1, 'S', 100, 1'000'000) << executed(1, 30);
    auto r = f.run();
    EXPECT_EQ(r.find_book(7)->snapshot().asks[0].total, 70);
    Feed g = f;
    g << executed(1, 70);
    auto r2 = g.run();
    EXPECT_EQ(r2.resting_orders(), 0u);
    EXPECT_FALSE(r2.find_book(7)->best_ask().has_value());
}

TEST(Itch, PartialCancelAndDelete) {
    Feed f;
    f << add(1, 'B', 100, 1'000'000) << add(2, 'B', 40, 1'000'000) << cancel(1, 60) << del(2);
    auto r = f.run();
    const BookSnapshot s = r.find_book(7)->snapshot();
    ASSERT_EQ(s.bids.size(), 1u);
    EXPECT_EQ(s.bids[0].orders, (std::vector<OrderView>{{1, 40, 0}}));
}

TEST(Itch, ReplaceLosesPriorityAndTakesTheNewRef) {
    Feed f;
    f << add(1, 'S', 10, 1'000'000) << add(2, 'S', 10, 1'000'000) << replace(1, 3, 10, 1'000'000);
    auto r = f.run();
    const BookSnapshot s = r.find_book(7)->snapshot();
    ASSERT_EQ(s.asks.size(), 1u);
    EXPECT_EQ(s.asks[0].orders, (std::vector<OrderView>{{2, 10, 0}, {3, 10, 0}}));
}

TEST(Itch, BooksAreSeparatedByStockLocate) {
    Feed f;
    Msg other('A', 9);
    other.u64(1).u8('B').u32(5).stock("MSFT").u32(2'000'000);
    f << add(1, 'B', 5, 1'000'000) << other;  // same order ref, different stock
    auto r = f.run();
    EXPECT_EQ(r.find_book(7)->best_bid(), 1'000'000);
    EXPECT_EQ(r.find_book(9)->best_bid(), 2'000'000);
    EXPECT_EQ(r.active_books(), 2u);
}

TEST(Itch, UnknownRefAndOverExecutionAreCountedNotApplied) {
    Feed f;
    f << add(1, 'B', 10, 1'000'000) << executed(99, 5) << executed(1, 11);
    auto r = f.run();
    EXPECT_EQ(r.stats().rejected_by_book, 2u);
    EXPECT_EQ(r.find_book(7)->snapshot().bids[0].total, 10);  // unchanged
}

TEST(Itch, OtherTypesAreSkippedByLengthAndTradesCounted) {
    Feed f;
    Msg sys('S');
    sys.u8('O');  // system event: start of messages
    Msg trade('P');
    trade.u64(0).u8('B').u32(100).stock("AAPL").u32(1'000'000).u64(42);
    f << sys << trade << add(1, 'B', 1, 1'000'000);
    auto r = f.run();
    EXPECT_EQ(r.stats().by_type['S'], 1u);
    EXPECT_EQ(r.stats().by_type['P'], 1u);
    EXPECT_EQ(r.resting_orders(), 1u);  // the hidden trade did not touch the book
}

TEST(Itch, TruncatedTailIsLeftUnconsumed) {
    Feed f;
    f << add(1, 'B', 1, 1'000'000) << add(2, 'B', 1, 1'000'000);
    const std::size_t cut = f.bytes.size() - 5;  // chop into the last message
    std::size_t seen = 0;
    const std::size_t used = for_each_message(f.bytes.data(), cut, [&](const std::uint8_t*, std::size_t) { ++seen; });
    EXPECT_EQ(seen, 1u);
    EXPECT_EQ(used, 2 + kLenAdd);
}

TEST(Itch, ShortMessageIsMalformedNotRead) {
    Msg tiny('A');  // header only, no body
    Feed f;
    f << tiny;
    auto r = f.run();
    EXPECT_EQ(r.stats().malformed, 1u);
    EXPECT_EQ(r.resting_orders(), 0u);
}
