#pragma once
#include "orderbook/order.hpp"

namespace ob {
class OrderBook {
 public:
  // Returns quantity filled immediately.
  Qty add(const Order& o);
  std::size_t size() const noexcept { return count_; }
 private:
  std::size_t count_{0};
};
}  // namespace ob
