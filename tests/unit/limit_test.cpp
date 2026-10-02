// Limit (GTC) orders: resting, crossing, price-time priority, rejects.
#include "book_fixture.hpp"

using namespace obtest;

template <class Book>
class LimitTest : public BookTest<Book> {};
TYPED_TEST_SUITE(LimitTest, BookTypes);

TYPED_TEST(LimitTest, NonCrossingOrdersRestWithoutTrading) {
    EXPECT_EQ(this->sell(1, 101, 5), (std::vector<Event>{lim_accepted(1, Side::Sell, 101, 5)}));
    EXPECT_EQ(this->buy(2, 100, 5), (std::vector<Event>{lim_accepted(2, Side::Buy, 100, 5)}));
    EXPECT_EQ(this->book.best_bid(), 100);
    EXPECT_EQ(this->book.best_ask(), 101);
    EXPECT_EQ(this->book.resting_count(), 2u);
}

TYPED_TEST(LimitTest, EmptyBookHasNoBestPrices) {
    EXPECT_FALSE(this->book.best_bid().has_value());
    EXPECT_FALSE(this->book.best_ask().has_value());
}

TYPED_TEST(LimitTest, FullCrossTradesAtMakerPrice) {
    this->sell(1, 100, 5);
    // The buyer was willing to pay 105 but trades at the resting 100.
    EXPECT_EQ(this->buy(2, 105, 5), (std::vector<Event>{lim_accepted(2, Side::Buy, 105, 5),
                                                        trade(2, Side::Buy, 1, 100, 5)}));
    EXPECT_EQ(this->book.resting_count(), 0u);
    EXPECT_EQ(this->snap(), BookSnapshot{});
}

TYPED_TEST(LimitTest, SellTakerAlsoTradesAtMakerPrice) {
    this->buy(1, 100, 5);
    EXPECT_EQ(this->sell(2, 95, 5), (std::vector<Event>{lim_accepted(2, Side::Sell, 95, 5),
                                                        trade(2, Side::Sell, 1, 100, 5)}));
}

TYPED_TEST(LimitTest, TakerRemainderRestsAtItsLimit) {
    this->sell(1, 100, 3);
    this->buy(2, 100, 8);
    EXPECT_EQ(this->book.best_bid(), 100);
    EXPECT_FALSE(this->book.best_ask().has_value());
    BookSnapshot s = this->snap();
    ASSERT_EQ(s.bids.size(), 1u);
    EXPECT_EQ(s.bids[0].total, 5);
    EXPECT_EQ(s.bids[0].orders, (std::vector<OrderView>{{2, 5, 0}}));
}

TYPED_TEST(LimitTest, PartiallyFilledMakerKeepsItsPlace) {
    this->sell(1, 100, 10);
    this->sell(2, 100, 10);
    this->buy(3, 100, 4);  // takes 4 from order 1, which stays at the front
    EXPECT_EQ(this->ids_at(Side::Sell, 100), (std::vector<OrderId>{1, 2}));
    EXPECT_EQ(this->snap().asks[0].orders[0].open, 6);
    EXPECT_EQ(this->snap().asks[0].total, 16);
}

TYPED_TEST(LimitTest, SamePriceFillsInArrivalOrder) {
    this->sell(1, 100, 2);
    this->sell(2, 100, 2);
    this->sell(3, 100, 2);
    EXPECT_EQ(this->buy(4, 100, 5),
              (std::vector<Event>{lim_accepted(4, Side::Buy, 100, 5), trade(4, Side::Buy, 1, 100, 2),
                                  trade(4, Side::Buy, 2, 100, 2), trade(4, Side::Buy, 3, 100, 1)}));
    EXPECT_EQ(this->ids_at(Side::Sell, 100), (std::vector<OrderId>{3}));
}

TYPED_TEST(LimitTest, BetterPriceBeatsEarlierArrival) {
    this->sell(1, 102, 5);  // arrived first, worse price
    this->sell(2, 101, 5);
    EXPECT_EQ(this->buy(3, 102, 5), (std::vector<Event>{lim_accepted(3, Side::Buy, 102, 5),
                                                        trade(3, Side::Buy, 2, 101, 5)}));
}

TYPED_TEST(LimitTest, WalksSeveralLevelsBestFirst) {
    this->sell(1, 101, 2);
    this->sell(2, 103, 2);
    this->sell(3, 102, 2);
    EXPECT_EQ(this->buy(4, 103, 6),
              (std::vector<Event>{lim_accepted(4, Side::Buy, 103, 6), trade(4, Side::Buy, 1, 101, 2),
                                  trade(4, Side::Buy, 3, 102, 2), trade(4, Side::Buy, 2, 103, 2)}));
    EXPECT_EQ(this->snap(), BookSnapshot{});
}

TYPED_TEST(LimitTest, StopsAtItsLimitAndRestsTheRest) {
    this->sell(1, 101, 2);
    this->sell(2, 105, 2);
    EXPECT_EQ(this->buy(3, 103, 5), (std::vector<Event>{lim_accepted(3, Side::Buy, 103, 5),
                                                        trade(3, Side::Buy, 1, 101, 2)}));
    EXPECT_EQ(this->book.best_bid(), 103);
    EXPECT_EQ(this->book.best_ask(), 105);
}

TYPED_TEST(LimitTest, BidsSortHighestFirst) {
    this->buy(1, 99, 1);
    this->buy(2, 101, 1);
    this->buy(3, 100, 1);
    BookSnapshot s = this->snap();
    ASSERT_EQ(s.bids.size(), 3u);
    EXPECT_EQ(s.bids[0].price, 101);
    EXPECT_EQ(s.bids[1].price, 100);
    EXPECT_EQ(s.bids[2].price, 99);
}

TYPED_TEST(LimitTest, EmptiedLevelIsRemoved) {
    this->sell(1, 100, 5);
    this->sell(2, 101, 5);
    this->buy(3, 100, 5);
    EXPECT_EQ(this->book.best_ask(), 101);
    EXPECT_EQ(this->snap().asks.size(), 1u);
}

TYPED_TEST(LimitTest, LiveDuplicateIdIsRejected) {
    this->buy(1, 100, 5);
    EXPECT_EQ(this->sell(1, 200, 5), (std::vector<Event>{Event::rejected(1, Reason::DuplicateId)}));
    EXPECT_EQ(this->book.resting_count(), 1u);
    EXPECT_FALSE(this->book.best_ask().has_value());
}

TYPED_TEST(LimitTest, IdOfAFilledOrderCanBeReused) {
    this->sell(1, 100, 5);
    this->buy(2, 100, 5);  // order 1 is now gone
    EXPECT_EQ(this->sell(1, 100, 3), (std::vector<Event>{lim_accepted(1, Side::Sell, 100, 3)}));
}

TYPED_TEST(LimitTest, InvalidOrderIsRejectedAndChangesNothing) {
    this->sell(1, 100, 5);
    BookSnapshot before = this->snap();
    EXPECT_EQ(this->buy(2, 100, 0), (std::vector<Event>{Event::rejected(2, Reason::InvalidQty)}));
    EXPECT_EQ(this->buy(3, 0, 5), (std::vector<Event>{Event::rejected(3, Reason::InvalidPrice)}));
    EXPECT_EQ(this->snap(), before);
}
