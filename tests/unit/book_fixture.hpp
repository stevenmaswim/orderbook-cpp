#pragma once

// Shared fixture for the order-type unit tests. The tests are gtest *typed*
// tests: every test runs once per book implementation listed in BookTypes, so
// the baseline and each upgraded book must pass the exact same behavioral
// suite.
#include <gtest/gtest.h>

#include <vector>

#include "ob/baseline_book.hpp"
#include "ob/event.hpp"
#include "ob/snapshot.hpp"

namespace obtest {

using namespace ob;

using BookTypes = ::testing::Types<BaselineBook>;

inline const BookConfig kTestConfig{1, 100'000};

template <class Book>
class BookTest : public ::testing::Test {
protected:
    Book book{kTestConfig};

    std::vector<Event> submit(OrderId id, Side s, OrderType t, Price px, Qty q) {
        VectorSink sink;
        book.submit(NewOrder{id, s, t, px, q}, sink);
        return sink.events;
    }
    std::vector<Event> buy(OrderId id, Price px, Qty q) {
        return submit(id, Side::Buy, OrderType::Limit, px, q);
    }
    std::vector<Event> sell(OrderId id, Price px, Qty q) {
        return submit(id, Side::Sell, OrderType::Limit, px, q);
    }

    std::vector<Event> cancel(OrderId id) {
        VectorSink sink;
        book.cancel(id, sink);
        return sink.events;
    }

    BookSnapshot snap() const { return book.snapshot(); }

    // The ids resting at one price, front of the FIFO first.
    std::vector<OrderId> ids_at(Side s, Price px) const {
        BookSnapshot sn = book.snapshot();
        const std::vector<LevelView>& levels = s == Side::Buy ? sn.bids : sn.asks;
        for (const LevelView& l : levels) {
            if (l.price != px) continue;
            std::vector<OrderId> ids;
            for (const OrderView& o : l.orders) ids.push_back(o.id);
            return ids;
        }
        return {};
    }
};

// Expected-event shorthands so a test reads like the contract in DESIGN.md.
inline Event accepted(OrderId id, Side s, OrderType t, Price px, Qty q) {
    return Event::accepted(NewOrder{id, s, t, px, q});
}
inline Event lim_accepted(OrderId id, Side s, Price px, Qty q) {
    return accepted(id, s, OrderType::Limit, px, q);
}
inline Event trade(OrderId taker, Side taker_side, OrderId maker, Price px, Qty q) {
    return Event::trade(taker, taker_side, maker, px, q);
}

}  // namespace obtest
