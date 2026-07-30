#pragma once
#include <cstdint>

namespace ob {
enum class Side { Buy, Sell };
using Price = std::int64_t;   // integer ticks, never floating point
using Qty   = std::uint64_t;
using OrderId = std::uint64_t;

struct Order {
  OrderId id{};
  Side    side{};
  Price   price{};
  Qty     qty{};
};
}  // namespace ob
