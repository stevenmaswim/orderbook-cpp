#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>

#include "ob/assert.hpp"
#include "ob/event.hpp"
#include "ob/snapshot.hpp"
#include "ob/types.hpp"
#include "ob/validate.hpp"

namespace ob {

// The v0 baseline: standard containers only, chosen to be easy to see as
// correct. It allocates on almost every operation on purpose. Upgrades A, B and
// C each remove one source of that cost and are measured against this class,
// which stays in the tree as the comparison point (and as one more book the
// differential fuzz test checks).
//
//   bids_/asks_ : std::map<Price, Level>, sorted best first, so best = begin()
//   Level       : std::list FIFO of resting orders + running total qty
//   index_      : order id -> iterators to its level and its list node
//
// Single-threaded by design (DESIGN.md section 8). Not thread-safe.
class BaselineBook {
public:
    explicit BaselineBook(BookConfig cfg = {})
        : cfg_(cfg), bids_(PriceOrder{true}), asks_(PriceOrder{false}) {}

    // Accept a new order: match what crosses, then rest the remainder.
    // Events: Accepted, Trade*, then at most one Cancelled. Or a lone Rejected.
    template <EventSink S>
    void submit(const NewOrder& o, S& sink) {
        if (Reason r = validate(o, cfg_); r != Reason::None) {
            sink(Event::rejected(o.id, r));
            return;
        }
        // Only a live id is a duplicate. Remembering every id ever used would
        // need unbounded memory (DESIGN.md section 5).
        if (index_.contains(o.id)) {
            sink(Event::rejected(o.id, Reason::DuplicateId));
            return;
        }
        if (o.type != OrderType::Limit) {
            // Market, IOC and FOK arrive in chunk 4.
            sink(Event::rejected(o.id, Reason::InvalidType));
            return;
        }
        sink(Event::accepted(o));
        Qty open = match(o.id, o.side, o.price, o.qty, sink);
        if (open > 0) rest(o.id, o.side, o.price, open);
    }

    std::optional<Price> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        return bids_.begin()->first;
    }
    std::optional<Price> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;
    }

    std::size_t resting_count() const { return index_.size(); }

    BookSnapshot snapshot() const {
        BookSnapshot s;
        copy_side(bids_, s.bids);
        copy_side(asks_, s.asks);
        return s;
    }

private:
    struct RestingOrder {
        OrderId id;
        Price price;
        Qty open;
        Seq seq;
        Side side;
    };
    using Fifo = std::list<RestingOrder>;

    struct Level {
        Fifo fifo;
        Qty total = 0;  // sum of fifo[i].open, kept up to date on every change
    };

    // One comparator type for both sides, with the direction chosen at run
    // time. With std::greater for bids and std::less for asks the two maps
    // would be different types, so one Locator could not hold an iterator into
    // either. The branch is perfectly predictable within a map.
    struct PriceOrder {
        bool descending;
        bool operator()(Price a, Price b) const { return descending ? a > b : a < b; }
    };
    using LevelMap = std::map<Price, Level, PriceOrder>;

    // std::map and std::list iterators stay valid while other elements are
    // inserted or erased, so storing them lets cancel jump straight to the
    // node instead of searching (DESIGN.md section 6).
    struct Locator {
        LevelMap::iterator level;
        Fifo::iterator order;
        Side side;
    };

    LevelMap& side_map(Side s) { return s == Side::Buy ? bids_ : asks_; }

    // Does a taker on `side` with this limit trade against a level at `px`?
    static bool crosses(Side side, Price limit, Price px) {
        return side == Side::Buy ? px <= limit : px >= limit;
    }

    // Fill `open` against the opposite side, best level first and FIFO within
    // a level. Returns what is left. Trades print at the maker's price.
    template <EventSink S>
    Qty match(OrderId taker, Side side, Price limit, Qty open, S& sink) {
        LevelMap& book = side_map(opposite(side));
        while (open > 0 && !book.empty()) {
            auto level_it = book.begin();  // best opposite price
            if (!crosses(side, limit, level_it->first)) break;
            Level& level = level_it->second;
            while (open > 0 && !level.fifo.empty()) {
                RestingOrder& maker = level.fifo.front();  // oldest at this price
                const Qty fill = std::min(open, maker.open);
                sink(Event::trade(taker, side, maker.id, maker.price, fill));
                open -= fill;
                maker.open -= fill;
                level.total -= fill;
                if (maker.open == 0) {
                    index_.erase(maker.id);
                    level.fifo.pop_front();
                }
            }
            // Never leave an empty level behind: best_bid()/best_ask() rely on
            // begin() being a level that really has orders.
            if (level.fifo.empty()) book.erase(level_it);
        }
        return open;
    }

    void rest(OrderId id, Side side, Price price, Qty open) {
        OB_ASSERT(open > 0);
        LevelMap& book = side_map(side);
        auto [level_it, created] = book.try_emplace(price);
        Level& level = level_it->second;
        level.fifo.push_back(RestingOrder{id, price, open, next_seq_++, side});
        level.total += open;
        index_.emplace(id, Locator{level_it, std::prev(level.fifo.end()), side});
    }

    static void copy_side(const LevelMap& book, std::vector<LevelView>& out) {
        for (const auto& [price, level] : book) {
            LevelView v{price, level.total, {}};
            for (const RestingOrder& r : level.fifo) v.orders.push_back({r.id, r.open, r.seq});
            out.push_back(std::move(v));
        }
    }

    BookConfig cfg_;
    LevelMap bids_;
    LevelMap asks_;
    std::unordered_map<OrderId, Locator> index_;
    Seq next_seq_ = 0;
};

}  // namespace ob
