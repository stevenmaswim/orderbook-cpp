// Tests for the tester: feed the invariant checker hand-made broken event
// streams and make sure it notices. A checker that never fails proves nothing.
#include <gtest/gtest.h>

#include <vector>

#include "invariants.hpp"
#include "ob/baseline_book.hpp"
#include "ob/op.hpp"

using namespace obtest;

namespace {
Event acc(OrderId id, Side s, Price px, Qty q) { return Event::accepted({id, s, OrderType::Limit, px, q}); }
}  // namespace

TEST(InvariantChecker, AcceptsAValidStream) {
    InvariantChecker c;
    EXPECT_EQ(c.on_events({acc(1, Side::Sell, 100, 5)}), "");
    EXPECT_EQ(c.on_events({acc(2, Side::Buy, 101, 3), Event::trade(2, Side::Buy, 1, 100, 3)}), "");
    EXPECT_EQ(c.on_events({Event::cancelled(1, Side::Sell, 2, Reason::UserCancel)}), "");
}

TEST(InvariantChecker, CatchesTradeAwayFromMakerPrice) {
    InvariantChecker c;
    c.on_events({acc(1, Side::Sell, 100, 5)});
    // Should print at the maker's 100, not the taker's 101.
    EXPECT_NE(c.on_events({acc(2, Side::Buy, 101, 3), Event::trade(2, Side::Buy, 1, 101, 3)}), "");
}

TEST(InvariantChecker, CatchesTradeBeyondTakerLimit) {
    InvariantChecker c;
    c.on_events({acc(1, Side::Sell, 105, 5)});
    EXPECT_NE(c.on_events({acc(2, Side::Buy, 100, 3), Event::trade(2, Side::Buy, 1, 105, 3)}), "");
}

TEST(InvariantChecker, CatchesOverfill) {
    InvariantChecker c;
    c.on_events({acc(1, Side::Sell, 100, 2)});
    EXPECT_NE(c.on_events({acc(2, Side::Buy, 100, 5), Event::trade(2, Side::Buy, 1, 100, 3)}), "");
}

TEST(InvariantChecker, CatchesCancelOfWrongQty) {
    InvariantChecker c;
    c.on_events({acc(1, Side::Buy, 100, 5)});
    EXPECT_NE(c.on_events({Event::cancelled(1, Side::Buy, 4, Reason::UserCancel)}), "");
}

TEST(InvariantChecker, CatchesIocLeftOpen) {
    InvariantChecker c;
    // An IOC that neither filled nor cancelled its remainder.
    EXPECT_NE(c.on_events({Event::accepted({1, Side::Buy, OrderType::IOC, 100, 5})}), "");
}

TEST(InvariantChecker, CatchesBookThatDisagreesWithEvents) {
    InvariantChecker c;
    BaselineBook book{BookConfig{1, 1000}};
    VectorSink sink;
    book.submit({1, Side::Buy, OrderType::Limit, 100, 5}, sink);
    // The checker was never told about order 1, so the book has an order the
    // events never produced.
    EXPECT_NE(c.check_book(book), "");
}

TEST(InvariantChecker, PassesARealBookFedThroughIt) {
    InvariantChecker c;
    BaselineBook book{BookConfig{1, 1000}};
    for (Op op : {submit_op({1, Side::Sell, OrderType::Limit, 101, 5}),
                  submit_op({2, Side::Buy, OrderType::Limit, 101, 2}),
                  modify_op(1, 101, 1), cancel_op(1)}) {
        VectorSink sink;
        apply(book, op, sink);
        ASSERT_EQ(c.on_events(sink.events), "") << op;
        ASSERT_EQ(c.check_book(book), "") << op;
    }
}
