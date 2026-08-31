// Tiny demo driver: builds a small book, prints top-of-book and a couple of
// executions. Run `./orderbook_demo` after building.

#include <iostream>

#include "orderbook/order_book.hpp"

using orderbook::OrderBook;
using orderbook::Side;

static void print_top(OrderBook& b, const char* label) {
    auto bid = b.best_bid();
    auto ask = b.best_ask();
    std::cout << label << "  bid="
              << (bid ? std::to_string(*bid) : "-") << "  ask="
              << (ask ? std::to_string(*ask) : "-") << "\n";
}

int main() {
    OrderBook book;
    book.add_order("s1", Side::Sell, 101, 5);
    book.add_order("s2", Side::Sell, 102, 5);
    book.add_order("b1", Side::Buy, 99, 5);
    print_top(book, "after resting orders:");

    std::cout << "sending marketable buy 6 @ 101...\n";
    auto trades = book.add_order("b2", Side::Buy, 101, 6);
    for (const auto& t : trades) {
        std::cout << "  TRADE " << t.quantity << " @ " << t.price
                  << " (" << t.buy_order_id << " x " << t.sell_order_id << ")\n";
    }
    print_top(book, "after cross:");
    return 0;
}
