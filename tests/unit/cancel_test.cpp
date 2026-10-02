// Cancel: removes a resting order in O(1) and leaves everyone else's
// priority untouched.
#include "book_fixture.hpp"

using namespace obtest;

template <class Book>
class CancelTest : public BookTest<Book> {};
TYPED_TEST_SUITE(CancelTest, BookTypes);

TYPED_TEST(CancelTest, CancelsARestingOrder) {
    this->buy(1, 100, 5);
    EXPECT_EQ(this->cancel(1),
              (std::vector<Event>{Event::cancelled(1, Side::Buy, 5, Reason::UserCancel)}));
    EXPECT_EQ(this->book.resting_count(), 0u);
    EXPECT_EQ(this->snap(), BookSnapshot{});
}

TYPED_TEST(CancelTest, ReportsOnlyTheOpenQtyOfAPartialFill) {
    this->sell(1, 100, 10);
    this->buy(2, 100, 4);
    EXPECT_EQ(this->cancel(1),
              (std::vector<Event>{Event::cancelled(1, Side::Sell, 6, Reason::UserCancel)}));
}

TYPED_TEST(CancelTest, UnknownIdIsRejected) {
    EXPECT_EQ(this->cancel(42), (std::vector<Event>{Event::rejected(42, Reason::UnknownOrder)}));
}

TYPED_TEST(CancelTest, SecondCancelIsRejected) {
    this->buy(1, 100, 5);
    this->cancel(1);
    EXPECT_EQ(this->cancel(1), (std::vector<Event>{Event::rejected(1, Reason::UnknownOrder)}));
}

TYPED_TEST(CancelTest, CancelAfterFullFillIsRejected) {
    this->sell(1, 100, 5);
    this->buy(2, 100, 5);
    EXPECT_EQ(this->cancel(1), (std::vector<Event>{Event::rejected(1, Reason::UnknownOrder)}));
}

TYPED_TEST(CancelTest, CancelFromMiddleKeepsOthersInOrder) {
    this->buy(1, 100, 1);
    this->buy(2, 100, 1);
    this->buy(3, 100, 1);
    this->cancel(2);
    EXPECT_EQ(this->ids_at(Side::Buy, 100), (std::vector<OrderId>{1, 3}));
    EXPECT_EQ(this->snap().bids[0].total, 2);
}

TYPED_TEST(CancelTest, CancelFrontPromotesNextOrder) {
    this->sell(1, 100, 3);
    this->sell(2, 100, 3);
    this->cancel(1);
    EXPECT_EQ(this->buy(3, 100, 3), (std::vector<Event>{lim_accepted(3, Side::Buy, 100, 3),
                                                        trade(3, Side::Buy, 2, 100, 3)}));
}

TYPED_TEST(CancelTest, CancellingLastOrderAtBestMovesBestPrice) {
    this->buy(1, 101, 1);
    this->buy(2, 100, 1);
    this->cancel(1);
    EXPECT_EQ(this->book.best_bid(), 100);
    EXPECT_EQ(this->snap().bids.size(), 1u);
}

TYPED_TEST(CancelTest, CancelledIdCanBeReused) {
    this->buy(1, 100, 5);
    this->cancel(1);
    EXPECT_EQ(this->buy(1, 99, 2), (std::vector<Event>{lim_accepted(1, Side::Buy, 99, 2)}));
}
