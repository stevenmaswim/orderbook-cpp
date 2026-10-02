#pragma once

// Invariant checker, ported from the Python engine's assert_invariants
// (Order_book_project/tests/conftest.py) and extended.
//
// It does not trust the book's internals. It keeps its own ledger built only
// from the event stream, then compares that ledger with the book's snapshot.
// So a book whose events and state disagree is caught even when both look
// plausible on their own.
//
// Python invariants carried over:
//   1. per order: filled + open + cancelled == total, nothing negative
//   2. every trade: qty > 0, maker and taker on opposite sides, price equals
//      the maker's resting price
//   3. the book is never crossed at rest: best bid < best ask
//   4. each level's incremental total equals a recount of its orders
// Added here:
//   5. no empty levels; levels strictly sorted best first; seq strictly
//      increasing within a FIFO (time priority)
//   6. best_bid/best_ask/resting_count agree with the snapshot
//   7. the set of resting orders (id, side, price, open) in the book equals
//      the set the ledger derived from events
//   8. a trade never violates the taker's limit; Market/IOC/FOK never rest
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "ob/event.hpp"
#include "ob/snapshot.hpp"
#include "ob/types.hpp"

namespace obtest {

using namespace ob;

class InvariantChecker {
public:
    // Feed the events produced by one operation. Returns "" or a description
    // of the first violation found.
    std::string on_events(const std::vector<Event>& events) {
        for (const Event& e : events) {
            std::string err = apply(e);
            if (!err.empty()) return err;
        }
        // At the end of an operation, only Limit orders (or modified orders,
        // which are always limit) may still have open qty. Those are resting.
        for (OrderId id : touched_) {
            auto it = ledger_.find(id);
            if (it == ledger_.end()) continue;  // listed twice and already dropped
            Entry& en = it->second;
            if (!conserved(en)) return fail("conservation broken for id ", id);
            if (en.open > 0 && en.type != OrderType::Limit) {
                return fail("non-limit order left open qty, id ", id);
            }
            en.resting = en.open > 0;
            if (!en.resting) ledger_.erase(it);  // finished; its id may be reused
        }
        touched_.clear();
        return "";
    }

    // Compare the book's state with the ledger and check structural rules.
    template <class Book>
    std::string check_book(const Book& book) const {
        const BookSnapshot s = book.snapshot();
        std::size_t orders_in_snapshot = 0;
        for (int side_i = 0; side_i < 2; ++side_i) {
            const Side side = side_i == 0 ? Side::Buy : Side::Sell;
            const std::vector<LevelView>& levels = side == Side::Buy ? s.bids : s.asks;
            for (std::size_t i = 0; i < levels.size(); ++i) {
                const LevelView& l = levels[i];
                if (l.orders.empty()) return fail("empty level at price ", l.price);
                if (i > 0) {
                    const Price prev = levels[i - 1].price;
                    const bool sorted = side == Side::Buy ? prev > l.price : prev < l.price;
                    if (!sorted) return fail("levels not sorted best first at price ", l.price);
                }
                Qty recount = 0;
                for (std::size_t k = 0; k < l.orders.size(); ++k) {
                    const OrderView& o = l.orders[k];
                    if (o.open <= 0) return fail("non-positive open qty for id ", o.id);
                    if (k > 0 && l.orders[k - 1].seq >= o.seq) {
                        return fail("FIFO seq not increasing at id ", o.id);
                    }
                    recount += o.open;
                    auto it = ledger_.find(o.id);
                    if (it == ledger_.end() || !it->second.resting) {
                        return fail("book has an order the events never left resting, id ", o.id);
                    }
                    const Entry& en = it->second;
                    if (en.side != side || en.price != l.price || en.open != o.open) {
                        return fail("book and ledger disagree on id ", o.id);
                    }
                    ++orders_in_snapshot;
                }
                if (recount != l.total) return fail("level total != recount at price ", l.price);
            }
        }
        std::size_t ledger_resting = 0;
        for (const auto& [id, en] : ledger_) ledger_resting += en.resting ? 1 : 0;
        if (ledger_resting != orders_in_snapshot) {
            return fail("ledger expects resting orders: ", static_cast<Qty>(ledger_resting));
        }
        if (book.resting_count() != orders_in_snapshot) return "resting_count disagrees with snapshot";
        if (!s.bids.empty() && !s.asks.empty() && s.bids[0].price >= s.asks[0].price) {
            return fail("crossed book, best bid ", s.bids[0].price);
        }
        const auto front = [](const std::vector<LevelView>& v) {
            return v.empty() ? std::optional<Price>{} : std::optional<Price>{v[0].price};
        };
        if (book.best_bid() != front(s.bids)) return "best_bid disagrees with snapshot";
        if (book.best_ask() != front(s.asks)) return "best_ask disagrees with snapshot";
        return "";
    }

private:
    struct Entry {
        Side side = Side::Buy;
        OrderType type = OrderType::Limit;
        Price price = 0;  // limit (or kNoPrice for market)
        Qty total = 0;
        Qty filled = 0;
        Qty cancelled = 0;
        Qty open = 0;
        bool resting = false;
    };

    static bool conserved(const Entry& e) {
        return e.filled >= 0 && e.open >= 0 && e.cancelled >= 0 &&
               e.filled + e.open + e.cancelled == e.total;
    }

    template <class T>
    static std::string fail(const char* what, T value) {
        std::ostringstream os;
        os << what << value;
        return os.str();
    }

    std::string apply(const Event& e) {
        switch (e.type) {
            case EventType::Rejected:
                return "";
            case EventType::Accepted: {
                auto it = ledger_.find(e.id);
                if (it != ledger_.end() && it->second.resting) {
                    return fail("accepted an id that is still resting: ", e.id);
                }
                ledger_[e.id] = Entry{e.side, e.order_type, e.price, e.qty, 0, 0, e.qty, false};
                touched_.push_back(e.id);
                return "";
            }
            case EventType::Trade: {
                if (e.qty <= 0) return fail("trade with qty <= 0, taker ", e.id);
                auto mk = ledger_.find(e.other_id);
                auto tk = ledger_.find(e.id);
                if (mk == ledger_.end() || !mk->second.resting) {
                    return fail("trade against a maker that is not resting: ", e.other_id);
                }
                if (tk == ledger_.end()) return fail("trade by an unknown taker: ", e.id);
                Entry& maker = mk->second;
                Entry& taker = tk->second;
                if (maker.side == taker.side || e.side != taker.side) {
                    return fail("trade sides do not oppose, taker ", e.id);
                }
                if (e.price != maker.price) return fail("trade not at maker price, maker ", e.other_id);
                if (taker.type != OrderType::Market) {
                    const bool within = taker.side == Side::Buy ? e.price <= taker.price
                                                                : e.price >= taker.price;
                    if (!within) return fail("trade outside the taker's limit, taker ", e.id);
                }
                maker.open -= e.qty;
                maker.filled += e.qty;
                taker.open -= e.qty;
                taker.filled += e.qty;
                if (maker.open < 0 || taker.open < 0) return fail("overfill on trade, taker ", e.id);
                if (maker.open == 0) {
                    maker.resting = false;
                    touched_.push_back(e.other_id);  // gets conservation-checked and dropped
                }
                return "";
            }
            case EventType::Cancelled: {
                auto it = ledger_.find(e.id);
                if (it == ledger_.end()) return fail("cancel of an unknown id: ", e.id);
                Entry& en = it->second;
                // Every cancel removes exactly what was open, no more, no less.
                if (e.qty != en.open) return fail("cancel qty != open qty, id ", e.id);
                en.cancelled += e.qty;
                en.open = 0;
                en.resting = false;
                touched_.push_back(e.id);
                return "";
            }
            case EventType::Modified: {
                auto it = ledger_.find(e.id);
                if (it == ledger_.end() || !it->second.resting) {
                    return fail("modify of an order that is not resting: ", e.id);
                }
                Entry& en = it->second;
                if (e.reason == Reason::KeptPriority) {
                    if (e.price != en.price || e.qty > en.open) return fail("bad in-place modify, id ", e.id);
                    en.cancelled += en.open - e.qty;
                    en.open = e.qty;
                } else {
                    // Cancel-replace: the old open qty is cancelled, the new
                    // qty is freshly accepted under the same id.
                    en.cancelled += en.open;
                    en.total += e.qty;
                    en.open = e.qty;
                    en.price = e.price;
                }
                en.type = OrderType::Limit;
                touched_.push_back(e.id);
                return "";
            }
        }
        return "unknown event type";
    }

    std::unordered_map<OrderId, Entry> ledger_;
    std::vector<OrderId> touched_;
};

}  // namespace obtest
