// Minimal dependency-free test harness for the order book core.
// Each CHECK records a pass/fail; main() prints a summary and returns non-zero
// on any failure so CI (and `ctest`) treat it as a build gate.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "orderbook/order_book.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::cerr << "  FAIL: " << what << "\n";
    }
}

using orderbook::OrderBook;
using orderbook::Side;

// A limit buy below the best ask should rest, not trade.
void test_resting_no_cross() {
    OrderBook b;
    auto t1 = b.add_order("a1", Side::Sell, 101, 5);
    auto t2 = b.add_order("b1", Side::Buy, 100, 5);
    check(t1.empty() && t2.empty(), "no cross -> no trades");
    check(b.best_bid() == 100, "best bid rests at 100");
    check(b.best_ask() == 101, "best ask rests at 101");
    check(b.spread() == 1, "spread is 1 tick");
}

// Marketable order fully crosses a single resting order at the resting price.
void test_full_match_at_resting_price() {
    OrderBook b;
    b.add_order("a1", Side::Sell, 100, 5);
    auto t = b.add_order("b1", Side::Buy, 105, 5);  // crosses up into the ask
    check(t.size() == 1, "one trade");
    check(t[0].price == 100, "trade at resting ask price (100)");
    check(t[0].quantity == 5, "full 5 lots");
    check(!b.best_ask().has_value(), "ask side now empty");
    check(!b.best_bid().has_value(), "buy fully filled, nothing rests");
}

// Incoming larger than resting -> partial fill, remainder rests on incoming side.
void test_incoming_partial_rests() {
    OrderBook b;
    b.add_order("a1", Side::Sell, 100, 3);
    auto t = b.add_order("b1", Side::Buy, 100, 8);
    check(t.size() == 1 && t[0].quantity == 3, "crossed 3 lots");
    check(b.best_bid() == 100, "remaining 5 lots rest as bid");
    auto d = b.depth();
    check(d.bids.size() == 1 && d.bids[0].volume == 5, "5 lots resting bid");
}

// Resting larger than incoming -> resting order is re-inserted with remainder.
void test_resting_partial_reinserted() {
    OrderBook b;
    b.add_order("a1", Side::Sell, 100, 10);
    auto t = b.add_order("b1", Side::Buy, 100, 4);
    check(t.size() == 1 && t[0].quantity == 4, "crossed 4 lots");
    check(b.best_ask() == 100, "resting ask remains at 100");
    auto d = b.depth();
    check(d.asks.size() == 1 && d.asks[0].volume == 6, "6 lots remain resting");
}

// Same price -> earlier order (lower sequence) fills first (time priority).
void test_time_priority() {
    OrderBook b;
    b.add_order("a1", Side::Sell, 100, 5);  // earlier
    b.add_order("a2", Side::Sell, 100, 5);  // later
    auto t = b.add_order("b1", Side::Buy, 100, 5);
    check(t.size() == 1, "one trade");
    check(t[0].sell_order_id == "a1", "earliest resting order fills first");
    check(b.best_ask() == 100, "a2 still resting");
}

// Better price wins over time (price priority dominates).
void test_price_priority() {
    OrderBook b;
    b.add_order("a1", Side::Sell, 102, 5);  // worse for a buyer
    b.add_order("a2", Side::Sell, 100, 5);  // better, arrived later
    auto t = b.add_order("b1", Side::Buy, 105, 5);
    check(t.size() == 1 && t[0].sell_order_id == "a2",
          "lowest ask fills first regardless of arrival order");
    check(t[0].price == 100, "executed at 100");
}

// A canceled order must never trade and must vanish from top-of-book.
void test_cancel_lazy_deletion() {
    OrderBook b;
    b.add_order("a1", Side::Sell, 100, 5);
    b.add_order("a2", Side::Sell, 101, 5);
    check(b.cancel_order("a1"), "cancel of live order succeeds");
    check(!b.cancel_order("a1"), "double cancel fails");
    check(b.best_ask() == 101, "best ask skips canceled a1");
    auto t = b.add_order("b1", Side::Buy, 100, 5);
    check(t.empty(), "canceled order does not trade");
}

// Sweep across multiple price levels in one incoming order.
void test_multi_level_sweep() {
    OrderBook b;
    b.add_order("a1", Side::Sell, 100, 2);
    b.add_order("a2", Side::Sell, 101, 2);
    b.add_order("a3", Side::Sell, 102, 2);
    auto t = b.add_order("b1", Side::Buy, 101, 10);  // takes 100 and 101 only
    check(t.size() == 2, "two fills (100 and 101)");
    check(t[0].price == 100 && t[1].price == 101, "ascending fill prices");
    check(b.best_ask() == 102, "102 remains untouched");
    check(b.best_bid() == 101, "unfilled 6 lots rest as bid at 101");
}

// Duplicate live id is rejected.
void test_duplicate_id_rejected() {
    OrderBook b;
    b.add_order("x", Side::Buy, 100, 5);
    auto t = b.add_order("x", Side::Buy, 101, 5);
    check(t.empty(), "duplicate live id makes no trade");
    check(b.live_order_count() == 1, "still one live order");
}

// Throughput smoke test: many orders should process and leave a consistent book.
void test_throughput_smoke() {
    OrderBook b;
    const int N = 100000;
    for (int i = 0; i < N; ++i) {
        Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
        std::int64_t px = 100 + (i % 5);  // clustered so lots of crossing
        b.add_order("o" + std::to_string(i), s, px, 1);
    }
    // Invariant: if both sides are present, the book is not crossed.
    auto bid = b.best_bid();
    auto ask = b.best_ask();
    if (bid && ask) check(*bid < *ask, "book is never crossed after processing");
    check(b.trades().size() > 0, "throughput run produced trades");
    std::cout << "  [smoke] processed " << N << " orders, "
              << b.trades().size() << " trades, "
              << b.live_order_count() << " live orders remain\n";
}

}  // namespace

int main() {
    std::cout << "Running order-book core tests...\n";
    test_resting_no_cross();
    test_full_match_at_resting_price();
    test_incoming_partial_rests();
    test_resting_partial_reinserted();
    test_time_priority();
    test_price_priority();
    test_cancel_lazy_deletion();
    test_multi_level_sweep();
    test_duplicate_id_rejected();
    test_throughput_smoke();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks
              << " checks passed.\n";
    if (g_failures > 0) {
        std::cerr << g_failures << " FAILURE(S).\n";
        return 1;
    }
    std::cout << "All checks passed.\n";
    return 0;
}
