#pragma once

#include <ostream>
#include <vector>

#include "ob/types.hpp"

// A full copy of the resting book, used by tests, the differential fuzz and
// the invariant checker. It allocates, so it is never used on the hot path.
namespace ob {

struct OrderView {
    OrderId id = 0;
    Qty open = 0;
    Seq seq = 0;

    // seq is left out on purpose. Two correct books must agree on FIFO
    // *order* (the position in the vector), but how each one numbers its
    // arrivals is an internal detail.
    bool operator==(const OrderView& o) const { return id == o.id && open == o.open; }
};

struct LevelView {
    Price price = 0;
    Qty total = 0;  // the book's incrementally maintained total, not a recount
    std::vector<OrderView> orders;  // front of the FIFO first
    bool operator==(const LevelView&) const = default;
};

struct BookSnapshot {
    std::vector<LevelView> bids;  // best (highest) price first
    std::vector<LevelView> asks;  // best (lowest) price first
    bool operator==(const BookSnapshot&) const = default;
};

inline std::ostream& operator<<(std::ostream& os, const LevelView& l) {
    os << "  " << l.price << " total=" << l.total << " [";
    for (const OrderView& o : l.orders) os << ' ' << o.id << ':' << o.open;
    return os << " ]\n";
}

inline std::ostream& operator<<(std::ostream& os, const BookSnapshot& s) {
    os << "\nasks (best first):\n";
    for (const LevelView& l : s.asks) os << l;
    os << "bids (best first):\n";
    for (const LevelView& l : s.bids) os << l;
    return os;
}

}  // namespace ob
