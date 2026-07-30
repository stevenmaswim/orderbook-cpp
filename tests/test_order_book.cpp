#include <gtest/gtest.h>
#include "orderbook/order_book.hpp"

TEST(OrderBook, AcceptsAnOrder) {
  ob::OrderBook book;
  book.add({1, ob::Side::Buy, 10000, 5});
  EXPECT_EQ(book.size(), 1u);
}
