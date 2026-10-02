// Stateless validation and the Event type itself.
#include <gtest/gtest.h>

#include <sstream>
#include <type_traits>

#include "ob/event.hpp"
#include "ob/validate.hpp"

using namespace ob;

namespace {
const BookConfig kCfg{100, 200};

NewOrder limit(Qty qty, Price px) { return {1, Side::Buy, OrderType::Limit, px, qty}; }
}  // namespace

TEST(Validate, AcceptsLimitInsideBand) {
    EXPECT_EQ(validate(limit(10, 100), kCfg), Reason::None);
    EXPECT_EQ(validate(limit(10, 200), kCfg), Reason::None);  // both ends inclusive
}

TEST(Validate, RejectsZeroAndNegativeQty) {
    EXPECT_EQ(validate(limit(0, 150), kCfg), Reason::InvalidQty);
    EXPECT_EQ(validate(limit(-5, 150), kCfg), Reason::InvalidQty);
}

TEST(Validate, RejectsQtyAboveMax) {
    EXPECT_EQ(validate(limit(kMaxQty, 150), kCfg), Reason::None);
    EXPECT_EQ(validate(limit(kMaxQty + 1, 150), kCfg), Reason::InvalidQty);
}

TEST(Validate, RejectsPriceOutsideBand) {
    EXPECT_EQ(validate(limit(1, 99), kCfg), Reason::InvalidPrice);
    EXPECT_EQ(validate(limit(1, 201), kCfg), Reason::InvalidPrice);
}

TEST(Validate, PricedTypesAllCheckTheBand) {
    for (OrderType t : {OrderType::Limit, OrderType::IOC, OrderType::FOK}) {
        NewOrder o{1, Side::Sell, t, 0, 5};
        EXPECT_EQ(validate(o, kCfg), Reason::InvalidPrice) << to_string(t);
    }
}

TEST(Validate, MarketMustCarryNoPrice) {
    NewOrder m{1, Side::Buy, OrderType::Market, kNoPrice, 5};
    EXPECT_EQ(validate(m, kCfg), Reason::None);
    m.price = 150;  // even an in-band price is a client bug on a market order
    EXPECT_EQ(validate(m, kCfg), Reason::InvalidPrice);
}

TEST(Validate, RejectsOutOfRangeEnums) {
    NewOrder bad_type = limit(1, 150);
    bad_type.type = static_cast<OrderType>(7);
    EXPECT_EQ(validate(bad_type, kCfg), Reason::InvalidType);
    NewOrder bad_side = limit(1, 150);
    bad_side.side = static_cast<Side>(9);
    EXPECT_EQ(validate(bad_side, kCfg), Reason::InvalidType);
}

TEST(Validate, ModifyChecksQtyAndBand) {
    EXPECT_EQ(validate_modify(150, 1, kCfg), Reason::None);
    EXPECT_EQ(validate_modify(150, 0, kCfg), Reason::InvalidQty);
    EXPECT_EQ(validate_modify(250, 1, kCfg), Reason::InvalidPrice);
}

TEST(EventType, IsFlatAndSmall) {
    // The bench copies events into ring buffers later; keep them trivially
    // copyable and small.
    EXPECT_TRUE(std::is_trivially_copyable_v<Event>);
    EXPECT_EQ(sizeof(Event), 40u);
}

TEST(EventType, EqualityIsMemberwise) {
    Event a = Event::trade(2, Side::Buy, 1, 100, 5);
    Event b = a;
    EXPECT_EQ(a, b);
    b.qty = 6;
    EXPECT_NE(a, b);
}

TEST(EventType, PrintsReadably) {
    std::ostringstream os;
    os << Event::cancelled(7, Side::Sell, 3, Reason::IocRemainder);
    EXPECT_NE(os.str().find("Cancelled id=7"), std::string::npos);
    EXPECT_NE(os.str().find("IocRemainder"), std::string::npos);
}

TEST(Sinks, SatisfyTheConcept) {
    static_assert(EventSink<VectorSink>);
    static_assert(EventSink<CountingSink>);
    CountingSink c;
    c(Event::trade(2, Side::Buy, 1, 100, 5));
    c(Event::rejected(3, Reason::InvalidQty));
    EXPECT_EQ(c.events, 2u);
    EXPECT_EQ(c.trades, 1u);
    EXPECT_EQ(c.traded_qty, 5);
}
