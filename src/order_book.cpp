#include "orderbook/order_book.hpp"

namespace ob {
Qty OrderBook::add(const Order&) {
  ++count_;
  return 0;   // TODO: matching logic
}
}  // namespace ob
