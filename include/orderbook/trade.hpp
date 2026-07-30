#pragma once

#include <cstdint>
#include <string>

namespace orderbook {

// A single execution produced by the matching engine.
struct Trade {
    std::string buy_order_id;
    std::string sell_order_id;
    std::int64_t price = 0;     // execution price in ticks (resting side's price)
    std::int64_t quantity = 0;  // lots crossed
    std::uint64_t sequence = 0; // engine-assigned execution sequence
};

}  // namespace orderbook
