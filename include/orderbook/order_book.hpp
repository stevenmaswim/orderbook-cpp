#pragma once

#include <cstdint>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "orderbook/order.hpp"
#include "orderbook/trade.hpp"

namespace orderbook {

// Price-time-priority limit order book.
//
// Design mirrors the reference Python engine:
//   * two priority queues hold lightweight handles (price, sequence, id);
//     the authoritative Order state lives in a single hash map by id.
//   * lazy deletion: canceled / filled orders are left in the heaps and
//     skipped when they surface at the top, so cancel is O(1) amortized.
//   * a marketable incoming order is matched against the opposite side at the
//     RESTING order's price; a partially filled resting order is re-pushed.
//
// This Phase-0 port is single-threaded and deterministic. Thread-safety is a
// later phase (see README roadmap) and intentionally left out here so the
// matching semantics can be verified in isolation first.
class OrderBook {
public:
    // Submit a new limit order. Returns the trades it generated (possibly
    // empty). The order id must be unique; resubmitting a live id is rejected
    // (returns empty and leaves the book unchanged).
    std::vector<Trade> add_order(const std::string& id, Side side,
                                 std::int64_t price, std::int64_t quantity);

    // Cancel a live order by id. Returns true if it was live and is now
    // canceled, false if unknown / already filled / already canceled.
    bool cancel_order(const std::string& id);

    // Best prices (in ticks), or nullopt if that side is empty of live orders.
    std::optional<std::int64_t> best_bid();
    std::optional<std::int64_t> best_ask();
    std::optional<std::int64_t> spread();  // best_ask - best_bid

    // Aggregated resting volume at the top `levels` price points per side.
    struct Level {
        std::int64_t price;
        std::int64_t volume;
    };
    struct Depth {
        std::vector<Level> bids;  // descending price
        std::vector<Level> asks;  // ascending price
    };
    Depth depth(std::size_t levels = 10);

    const std::vector<Trade>& trades() const { return trades_; }
    std::size_t live_order_count() const;

private:
    // Heap handle: what we store in the priority queues.
    struct Handle {
        std::int64_t price;
        std::uint64_t sequence;
        std::string id;
    };
    struct BidCmp {  // best bid = highest price, then earliest sequence
        bool operator()(const Handle& a, const Handle& b) const {
            if (a.price != b.price) return a.price < b.price;
            return a.sequence > b.sequence;
        }
    };
    struct AskCmp {  // best ask = lowest price, then earliest sequence
        bool operator()(const Handle& a, const Handle& b) const {
            if (a.price != b.price) return a.price > b.price;
            return a.sequence > b.sequence;
        }
    };

    Order* peek_top_live(Side side);   // discards dead handles, returns top live
    Order* pop_top_live(Side side);    // ^ but removes it from the heap
    void push_handle(const Order& o);
    Trade execute(Order& buy, Order& sell, std::int64_t price);

    std::priority_queue<Handle, std::vector<Handle>, BidCmp> bids_;
    std::priority_queue<Handle, std::vector<Handle>, AskCmp> asks_;
    std::unordered_map<std::string, Order> orders_;
    std::vector<Trade> trades_;
    std::uint64_t next_sequence_ = 0;
};

}  // namespace orderbook
