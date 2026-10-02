// A guided tour of the engine: each order type in turn, printing the events
// it produces and the book afterwards. Then a short load run.
//
//   cmake --preset release && cmake --build --preset release -j
//   ./build/release/examples/ob_demo
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "ob/books.hpp"
#include "ob/op.hpp"
#include "workloads.hpp"

using namespace ob;

namespace {

using Book = PoolArrayBook;  // the fastest variant: flat ladder + node pool

// Prices are integer ticks; show them as dollars with 1 tick = $0.01.
std::string px(Price p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld.%02lld", static_cast<long long>(p / 100),
                  static_cast<long long>(p % 100));
    return buf;
}

// Prints every event as it happens, in plain words.
struct PrintSink {
    void operator()(const Event& e) const {
        switch (e.type) {
            case EventType::Accepted:
                if (e.order_type == OrderType::Market) {
                    std::printf("    accepted  #%llu %s %s qty %lld\n", ull(e.id), to_string(e.side),
                                to_string(e.order_type), ll(e.qty));
                } else {
                    std::printf("    accepted  #%llu %s %s %lld @ %s\n", ull(e.id), to_string(e.side),
                                to_string(e.order_type), ll(e.qty), px(e.price).c_str());
                }
                break;
            case EventType::Trade:
                std::printf("    TRADE     taker #%llu hit maker #%llu: %lld @ %s\n", ull(e.id), ull(e.other_id),
                            ll(e.qty), px(e.price).c_str());
                break;
            case EventType::Cancelled:
                std::printf("    cancelled #%llu: %lld removed (%s)\n", ull(e.id), ll(e.qty), to_string(e.reason));
                break;
            case EventType::Modified:
                std::printf("    modified  #%llu -> %lld @ %s (%s)\n", ull(e.id), ll(e.qty), px(e.price).c_str(),
                            to_string(e.reason));
                break;
            case EventType::Rejected:
                std::printf("    REJECTED  #%llu (%s)\n", ull(e.id), to_string(e.reason));
                break;
        }
    }
    static unsigned long long ull(std::uint64_t v) { return v; }
    static long long ll(std::int64_t v) { return v; }
};

void print_book(const Book& book) {
    const BookSnapshot s = book.snapshot();
    std::printf("\n      %-10s %8s   orders (oldest first)\n", "price", "qty");
    for (auto it = s.asks.rbegin(); it != s.asks.rend(); ++it) {
        std::printf("  ASK %-10s %8lld   ", px(it->price).c_str(), static_cast<long long>(it->total));
        for (const OrderView& o : it->orders) std::printf("#%llu:%lld ", static_cast<unsigned long long>(o.id), static_cast<long long>(o.open));
        std::printf("\n");
    }
    std::printf("  ------------------------------\n");
    for (const LevelView& l : s.bids) {
        std::printf("  BID %-10s %8lld   ", px(l.price).c_str(), static_cast<long long>(l.total));
        for (const OrderView& o : l.orders) std::printf("#%llu:%lld ", static_cast<unsigned long long>(o.id), static_cast<long long>(o.open));
        std::printf("\n");
    }
    std::printf("\n");
}

void step(const char* title, Book& book, const Op& op) {
    std::printf("== %s\n", title);
    PrintSink sink;
    apply(book, op, sink);
    print_book(book);
}

NewOrder order(OrderId id, Side s, OrderType t, Price p, Qty q) { return {id, s, t, p, q}; }

}  // namespace

int main() {
    Book book(BookConfig{1, 20'000, 1 << 18});
    const Side B = Side::Buy, S = Side::Sell;
    const OrderType L = OrderType::Limit;

    std::printf("\nPrice-time priority limit order book: guided tour\n\n");
    step("Two sellers rest at 101.00, one at 102.00", book, submit_op(order(1, S, L, 10100, 50)));
    step("(second seller at 101.00 queues behind #1)", book, submit_op(order(2, S, L, 10100, 30)));
    step("(seller at 102.00)", book, submit_op(order(3, S, L, 10200, 40)));
    step("Buyers rest below the ask", book, submit_op(order(4, B, L, 9900, 60)));
    step("Limit buy 70 @ 101.50 crosses: fills #1 first (time priority), at the MAKER's price",
         book, submit_op(order(5, B, L, 10150, 70)));
    step("Market sell 100: fills 60 at the best bid; the other 40 is cancelled, never rests",
         book, submit_op(order(6, S, OrderType::Market, kNoPrice, 100)));
    step("IOC buy 100 @ 102.00: fills what is there, cancels the rest", book,
         submit_op(order(7, B, OrderType::IOC, 10200, 100)));
    step("Rebuild some asks", book, submit_op(order(8, S, L, 10300, 25)));
    step("(another ask)", book, submit_op(order(9, S, L, 10300, 25)));
    step("FOK buy 60 @ 103.00: only 50 available, so all-or-nothing kills it, book untouched",
         book, submit_op(order(10, B, OrderType::FOK, 10300, 60)));
    step("Modify #8 down to 10: shrinking keeps its place in the queue", book, modify_op(8, 10300, 10));
    step("Modify #8 up to 40: growing loses priority (moves behind #9)", book, modify_op(8, 10300, 40));
    step("Cancel #9", book, cancel_op(9));
    step("Duplicate id: #8 is still live, so a new #8 is rejected", book,
         submit_op(order(8, B, L, 9000, 1)));

    // A short load run on the benchmark workload shape. A demo number only:
    // the measured, reproducible figures are in bench/RESULTS.md.
    std::printf("== Load run: 1,000,000 orders (Python benchmark shape) through a fresh book\n");
    const std::vector<Op> ops = obbench::generate_shape({1, 1'000'000, 9950, 10050});
    Book big(BookConfig{1, 20'000, 1 << 18});
    CountingSink counts;
    const auto t0 = std::chrono::steady_clock::now();
    for (const Op& op : ops) apply(big, op, counts);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("    %zu ops (submits + cancels), %llu trades, %zu orders left resting\n", ops.size(),
                static_cast<unsigned long long>(counts.trades), big.resting_count());
    std::printf("    took %.3f s on this run (demo only; see bench/RESULTS.md for real numbers)\n\n", secs);
    return 0;
}
