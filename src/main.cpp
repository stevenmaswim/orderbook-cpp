#include <cstdio>
#include "orderbook/order_book.hpp"

int main() {
  ob::OrderBook book;
  book.add({1, ob::Side::Buy, 10000, 5});
  std::printf("orders in book: %zu\n", book.size());
  return 0;
}
