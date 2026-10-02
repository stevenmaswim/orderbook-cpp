#pragma once

#include <cstdint>

#include "ob/event.hpp"
#include "ob/types.hpp"

// Stateless input checks shared by every book implementation (and the
// reference model), so they all reject exactly the same inputs. Checks that
// need book state (duplicate id, unknown order) live in the books.
namespace ob {

inline bool qty_in_range(Qty q) { return q > 0 && q <= kMaxQty; }

inline bool price_in_band(Price p, const BookConfig& cfg) {
    return p >= cfg.min_price && p <= cfg.max_price;
}

// Returns Reason::None if the order is acceptable.
inline Reason validate(const NewOrder& o, const BookConfig& cfg) {
    // Enums can hold out-of-range values when built from raw bytes (a replay
    // trace, later a network message), so check them explicitly.
    if (static_cast<std::uint8_t>(o.side) > static_cast<std::uint8_t>(Side::Sell) ||
        static_cast<std::uint8_t>(o.type) > static_cast<std::uint8_t>(OrderType::FOK)) {
        return Reason::InvalidType;
    }
    if (!qty_in_range(o.qty)) return Reason::InvalidQty;
    if (o.type == OrderType::Market) {
        // A market order has no limit, so a price on one is a client bug.
        if (o.price != kNoPrice) return Reason::InvalidPrice;
    } else if (!price_in_band(o.price, cfg)) {
        return Reason::InvalidPrice;
    }
    return Reason::None;
}

// Modify targets a resting limit order, so the new price must be in band and
// the new open qty must be positive. Removing an order is Cancel's job.
inline Reason validate_modify(Price new_price, Qty new_qty, const BookConfig& cfg) {
    if (!qty_in_range(new_qty)) return Reason::InvalidQty;
    if (!price_in_band(new_price, cfg)) return Reason::InvalidPrice;
    return Reason::None;
}

}  // namespace ob
