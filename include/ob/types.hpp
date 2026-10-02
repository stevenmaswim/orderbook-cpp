#pragma once

#include <cstdint>

// Core value types. Everything is an integer: prices are ticks, quantities
// are lots. No floating point anywhere in the engine, so a fill can never be
// off by a rounding error.
namespace ob {

// int64 ticks: exact, and comparing two prices is one integer compare.
using Price = std::int64_t;

// Signed on purpose. If an accounting bug subtracts too much, the value goes
// negative and the invariant checker catches it. An unsigned type would wrap
// to about 1.8e19 and look like a huge valid order.
using Qty = std::int64_t;

// Chosen by the client, not the engine, so tests and replay traces control ids.
using OrderId = std::uint64_t;

// Arrival counter stamped when an order rests. Within a price level, a lower
// seq means it arrived earlier, which is what time priority means.
using Seq = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

// Order type and time-in-force folded into one enum. Only these four
// combinations exist in this engine, so two fields would allow meaningless
// pairs (a "market GTC") that we would then have to reject.
enum class OrderType : std::uint8_t { Limit, Market, IOC, FOK };

// Market orders carry this price. They have no limit, so any value other
// than 0 is rejected rather than silently ignored.
inline constexpr Price kNoPrice = 0;

// Upper bound on one order's size. With it, a level's running total is at
// most kMaxQty * (orders on the level), far below the int64 limit (about
// 9.2e18) for any book that fits in memory, so the sum cannot overflow.
inline constexpr Qty kMaxQty = 1'000'000'000;

// Prices outside [min_price, max_price] are rejected at the API. The flat
// price ladder (menu item C) needs this band to size its array; the other
// books enforce the same band so every implementation accepts the same input.
struct BookConfig {
    Price min_price = 1;
    Price max_price = 1'000'000;
};

// A new order as the client sends it.
struct NewOrder {
    OrderId id = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Price price = kNoPrice;  // ticks; must be kNoPrice for Market
    Qty qty = 0;
};

}  // namespace ob
