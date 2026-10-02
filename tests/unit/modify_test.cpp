// Modify: shrinking in place keeps priority; anything else is a
// cancel-replace that goes to the back of the queue.
#include "book_fixture.hpp"

using namespace obtest;

template <class Book>
class ModifyTest : public BookTest<Book> {};
TYPED_TEST_SUITE(ModifyTest, BookTypes);

namespace {
Event kept(OrderId id, Side s, Price px, Qty q) {
    return Event::modified(id, s, px, q, Reason::KeptPriority);
}
Event lost(OrderId id, Side s, Price px, Qty q) {
    return Event::modified(id, s, px, q, Reason::LostPriority);
}
}  // namespace

TYPED_TEST(ModifyTest, ShrinkKeepsQueuePosition) {
    this->buy(1, 100, 10);
    this->buy(2, 100, 10);
    EXPECT_EQ(this->modify(1, 100, 4), (std::vector<Event>{kept(1, Side::Buy, 100, 4)}));
    EXPECT_EQ(this->ids_at(Side::Buy, 100), (std::vector<OrderId>{1, 2}));
    EXPECT_EQ(this->snap().bids[0].total, 14);
    EXPECT_EQ(this->snap().bids[0].orders[0].open, 4);
}

TYPED_TEST(ModifyTest, SameQtySamePriceIsANoOpThatKeepsPriority) {
    this->sell(1, 100, 5);
    this->sell(2, 100, 5);
    EXPECT_EQ(this->modify(1, 100, 5), (std::vector<Event>{kept(1, Side::Sell, 100, 5)}));
    EXPECT_EQ(this->ids_at(Side::Sell, 100), (std::vector<OrderId>{1, 2}));
}

TYPED_TEST(ModifyTest, IncreaseLosesQueuePosition) {
    this->buy(1, 100, 5);
    this->buy(2, 100, 5);
    EXPECT_EQ(this->modify(1, 100, 8), (std::vector<Event>{lost(1, Side::Buy, 100, 8)}));
    EXPECT_EQ(this->ids_at(Side::Buy, 100), (std::vector<OrderId>{2, 1}));
    EXPECT_EQ(this->snap().bids[0].total, 13);
}

TYPED_TEST(ModifyTest, PriceChangeMovesToBackOfNewLevel) {
    this->sell(1, 101, 5);
    this->sell(2, 100, 5);
    EXPECT_EQ(this->modify(1, 100, 5), (std::vector<Event>{lost(1, Side::Sell, 100, 5)}));
    EXPECT_EQ(this->ids_at(Side::Sell, 100), (std::vector<OrderId>{2, 1}));
    EXPECT_EQ(this->snap().asks.size(), 1u);  // the 101 level emptied and is gone
}

TYPED_TEST(ModifyTest, PriceChangeWithSmallerQtyStillLosesPriority) {
    this->buy(1, 100, 5);
    this->buy(2, 99, 5);
    EXPECT_EQ(this->modify(1, 99, 2), (std::vector<Event>{lost(1, Side::Buy, 99, 2)}));
    EXPECT_EQ(this->ids_at(Side::Buy, 99), (std::vector<OrderId>{2, 1}));
}

TYPED_TEST(ModifyTest, RepriceIntoCrossTradesAtMakerPriceThenRests) {
    this->sell(1, 102, 3);
    this->buy(2, 100, 5);
    EXPECT_EQ(this->modify(2, 103, 5), (std::vector<Event>{lost(2, Side::Buy, 103, 5),
                                                            trade(2, Side::Buy, 1, 102, 3)}));
    EXPECT_EQ(this->book.best_bid(), 103);
    EXPECT_EQ(this->snap().bids[0].orders, (std::vector<OrderView>{{2, 2, 0}}));
    EXPECT_FALSE(this->book.best_ask().has_value());
}

TYPED_TEST(ModifyTest, ShrinkOfAPartiallyFilledOrderUsesOpenQty) {
    this->sell(1, 100, 10);
    this->buy(2, 100, 6);  // open is now 4
    EXPECT_EQ(this->modify(1, 100, 3), (std::vector<Event>{kept(1, Side::Sell, 100, 3)}));
    // 5 > open of 4, so this is an increase and loses priority.
    EXPECT_EQ(this->modify(1, 100, 5), (std::vector<Event>{lost(1, Side::Sell, 100, 5)}));
}

TYPED_TEST(ModifyTest, CancelAfterModifyReportsTheNewOpenQty) {
    this->buy(1, 100, 10);
    this->modify(1, 100, 4);
    EXPECT_EQ(this->cancel(1),
              (std::vector<Event>{Event::cancelled(1, Side::Buy, 4, Reason::UserCancel)}));
}

TYPED_TEST(ModifyTest, UnknownIdIsRejected) {
    EXPECT_EQ(this->modify(9, 100, 1), (std::vector<Event>{Event::rejected(9, Reason::UnknownOrder)}));
}

TYPED_TEST(ModifyTest, UnknownIdWinsOverBadValues) {
    EXPECT_EQ(this->modify(9, 100, 0), (std::vector<Event>{Event::rejected(9, Reason::UnknownOrder)}));
}

TYPED_TEST(ModifyTest, ZeroQtyIsRejectedNotTreatedAsCancel) {
    this->buy(1, 100, 5);
    EXPECT_EQ(this->modify(1, 100, 0), (std::vector<Event>{Event::rejected(1, Reason::InvalidQty)}));
    EXPECT_EQ(this->book.resting_count(), 1u);
}

TYPED_TEST(ModifyTest, OutOfBandPriceIsRejectedAndOrderStays) {
    this->buy(1, 100, 5);
    BookSnapshot before = this->snap();
    EXPECT_EQ(this->modify(1, kTestConfig.max_price + 1, 5),
              (std::vector<Event>{Event::rejected(1, Reason::InvalidPrice)}));
    EXPECT_EQ(this->snap(), before);
}
