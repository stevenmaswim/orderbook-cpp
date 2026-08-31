#pragma once

#include <cstdint>
#include <string>

namespace orderbook {

enum class Side { Buy, Sell };

enum class OrderState { Pending, PartiallyFilled, Filled, Canceled };

// A resting or incoming order. Quantities are integer "lots" to keep the
// matching engine exact (no floating-point drift on partial fills); price is
// an integer number of ticks for the same reason. Convert to/from human units
// at the API boundary, not inside the book.
struct Order {
    std::string id;
    std::int64_t price = 0;      // price in ticks
    std::int64_t quantity = 0;   // total size in lots
    Side side = Side::Buy;
    std::uint64_t sequence = 0;  // monotonic arrival counter -> time priority
    OrderState state = OrderState::Pending;
    std::int64_t filled = 0;     // lots filled so far

    std::int64_t remaining() const { return quantity - filled; }

    // Apply a fill of `qty` lots and advance the state.
    void apply_fill(std::int64_t qty) {
        filled += qty;
        state = (filled >= quantity) ? OrderState::Filled
                                     : OrderState::PartiallyFilled;
    }

    bool is_live() const {
        return state == OrderState::Pending ||
               state == OrderState::PartiallyFilled;
    }
};

}  // namespace orderbook
