// Market, IOC and FOK: the order types that never rest.
#include "book_fixture.hpp"

using namespace obtest;

template <class Book>
class TifTest : public BookTest<Book> {
protected:
    std::vector<Event> market(OrderId id, Side s, Qty q) {
        return this->submit(id, s, OrderType::Market, kNoPrice, q);
    }
    std::vector<Event> ioc(OrderId id, Side s, Price px, Qty q) {
        return this->submit(id, s, OrderType::IOC, px, q);
    }
    std::vector<Event> fok(OrderId id, Side s, Price px, Qty q) {
        return this->submit(id, s, OrderType::FOK, px, q);
    }
};
TYPED_TEST_SUITE(TifTest, BookTypes);

// ---- Market ----

TYPED_TEST(TifTest, MarketSweepsLevelsRegardlessOfPrice) {
    this->sell(1, 100, 2);
    this->sell(2, 5000, 2);  // far away, still fair game for a market order
    EXPECT_EQ(this->market(3, Side::Buy, 4),
              (std::vector<Event>{accepted(3, Side::Buy, OrderType::Market, kNoPrice, 4),
                                  trade(3, Side::Buy, 1, 100, 2), trade(3, Side::Buy, 2, 5000, 2)}));
    EXPECT_EQ(this->snap(), BookSnapshot{});
}

TYPED_TEST(TifTest, MarketRemainderIsCancelledNotRested) {
    this->buy(1, 100, 3);
    EXPECT_EQ(this->market(2, Side::Sell, 5),
              (std::vector<Event>{accepted(2, Side::Sell, OrderType::Market, kNoPrice, 5),
                                  trade(2, Side::Sell, 1, 100, 3),
                                  Event::cancelled(2, Side::Sell, 2, Reason::MarketRemainder)}));
    EXPECT_FALSE(this->book.best_ask().has_value());
    EXPECT_EQ(this->book.resting_count(), 0u);
}

TYPED_TEST(TifTest, MarketIntoEmptySideIsFullyCancelled) {
    this->buy(1, 100, 3);  // same side, irrelevant to a buy market order
    EXPECT_EQ(this->market(2, Side::Buy, 5),
              (std::vector<Event>{accepted(2, Side::Buy, OrderType::Market, kNoPrice, 5),
                                  Event::cancelled(2, Side::Buy, 5, Reason::MarketRemainder)}));
}

// ---- IOC ----

TYPED_TEST(TifTest, IocFillsWhatCrossesAndCancelsTheRest) {
    this->sell(1, 100, 2);
    this->sell(2, 102, 2);  // beyond the IOC limit
    EXPECT_EQ(this->ioc(3, Side::Buy, 101, 5),
              (std::vector<Event>{accepted(3, Side::Buy, OrderType::IOC, 101, 5),
                                  trade(3, Side::Buy, 1, 100, 2),
                                  Event::cancelled(3, Side::Buy, 3, Reason::IocRemainder)}));
    EXPECT_FALSE(this->book.best_bid().has_value());
    EXPECT_EQ(this->book.best_ask(), 102);
}

TYPED_TEST(TifTest, IocFullyFilledEmitsNoCancel) {
    this->buy(1, 100, 5);
    EXPECT_EQ(this->ioc(2, Side::Sell, 100, 5),
              (std::vector<Event>{accepted(2, Side::Sell, OrderType::IOC, 100, 5),
                                  trade(2, Side::Sell, 1, 100, 5)}));
}

TYPED_TEST(TifTest, NonCrossingIocNeverRests) {
    this->sell(1, 105, 5);
    EXPECT_EQ(this->ioc(2, Side::Buy, 100, 5),
              (std::vector<Event>{accepted(2, Side::Buy, OrderType::IOC, 100, 5),
                                  Event::cancelled(2, Side::Buy, 5, Reason::IocRemainder)}));
    EXPECT_FALSE(this->book.best_bid().has_value());
}

// ---- FOK ----

TYPED_TEST(TifTest, FokExactlyFillableExecutes) {
    this->sell(1, 100, 3);
    this->sell(2, 100, 2);
    EXPECT_EQ(this->fok(3, Side::Buy, 100, 5),
              (std::vector<Event>{accepted(3, Side::Buy, OrderType::FOK, 100, 5),
                                  trade(3, Side::Buy, 1, 100, 3), trade(3, Side::Buy, 2, 100, 2)}));
    EXPECT_EQ(this->snap(), BookSnapshot{});
}

TYPED_TEST(TifTest, FokShortByOneTradesNothing) {
    this->sell(1, 100, 3);
    this->sell(2, 101, 1);
    BookSnapshot before = this->snap();
    EXPECT_EQ(this->fok(3, Side::Buy, 101, 5),
              (std::vector<Event>{accepted(3, Side::Buy, OrderType::FOK, 101, 5),
                                  Event::cancelled(3, Side::Buy, 5, Reason::FokUnfillable)}));
    EXPECT_EQ(this->snap(), before);  // all-or-nothing: the book is untouched
}

TYPED_TEST(TifTest, FokCountsOnlyLevelsInsideItsLimit) {
    this->buy(1, 100, 3);
    this->buy(2, 98, 10);  // enough qty, but below the sell FOK's limit
    EXPECT_EQ(this->fok(3, Side::Sell, 99, 5),
              (std::vector<Event>{accepted(3, Side::Sell, OrderType::FOK, 99, 5),
                                  Event::cancelled(3, Side::Sell, 5, Reason::FokUnfillable)}));
}

TYPED_TEST(TifTest, FokFillableOnlyAcrossLevels) {
    this->buy(1, 101, 2);
    this->buy(2, 100, 2);
    this->buy(3, 99, 2);
    EXPECT_EQ(this->fok(4, Side::Sell, 99, 6),
              (std::vector<Event>{accepted(4, Side::Sell, OrderType::FOK, 99, 6),
                                  trade(4, Side::Sell, 1, 101, 2), trade(4, Side::Sell, 2, 100, 2),
                                  trade(4, Side::Sell, 3, 99, 2)}));
}

TYPED_TEST(TifTest, NonRestingTypesCanReuseTheirIdImmediately) {
    // They never enter the index, so the id is free again right away.
    this->market(1, Side::Buy, 5);
    EXPECT_EQ(this->buy(1, 100, 1), (std::vector<Event>{lim_accepted(1, Side::Buy, 100, 1)}));
}

TYPED_TEST(TifTest, CancelOfNonRestingTypeIsRejected) {
    this->ioc(1, Side::Buy, 100, 5);
    EXPECT_EQ(this->cancel(1), (std::vector<Event>{Event::rejected(1, Reason::UnknownOrder)}));
}
