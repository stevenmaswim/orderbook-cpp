#pragma once

#include <concepts>
#include <cstdint>
#include <ostream>
#include <vector>

#include "ob/types.hpp"

// Everything the engine reports goes out as a stream of one flat Event type.
// Tests compare these streams exactly, so the order in which events are
// emitted is part of the contract (see DESIGN.md section 4).
namespace ob {

enum class EventType : std::uint8_t { Accepted, Rejected, Trade, Cancelled, Modified };

// One enum for every "why". A single field keeps Event small and flat.
enum class Reason : std::uint8_t {
    None,
    // Rejected
    InvalidQty,
    InvalidPrice,
    InvalidType,
    DuplicateId,
    UnknownOrder,
    CapacityExceeded,
    // Cancelled
    UserCancel,
    MarketRemainder,
    IocRemainder,
    FokUnfillable,
    // Modified
    KeptPriority,
    LostPriority,
};

// Trivially copyable, 40 bytes. Field meaning by type:
//   Accepted : id, side, order_type, price (limit or kNoPrice), qty
//   Rejected : id, reason (other fields zero)
//   Trade    : id = taker, other_id = maker, side = taker side,
//              price = maker's resting price, qty = filled amount
//   Cancelled: id, side, reason, qty = amount removed
//   Modified : id, side, reason, price and qty = new price and new open qty
struct Event {
    EventType type = EventType::Accepted;
    Side side = Side::Buy;
    OrderType order_type = OrderType::Limit;
    Reason reason = Reason::None;
    OrderId id = 0;
    OrderId other_id = 0;
    Price price = 0;
    Qty qty = 0;

    // Memberwise equality is exactly what the differential test needs.
    bool operator==(const Event&) const = default;

    static Event accepted(const NewOrder& o) {
        return {EventType::Accepted, o.side, o.type, Reason::None, o.id, 0, o.price, o.qty};
    }
    static Event rejected(OrderId id, Reason why) {
        return {EventType::Rejected, Side::Buy, OrderType::Limit, why, id, 0, 0, 0};
    }
    static Event trade(OrderId taker, Side taker_side, OrderId maker, Price px, Qty q) {
        return {EventType::Trade, taker_side, OrderType::Limit, Reason::None, taker, maker, px, q};
    }
    static Event cancelled(OrderId id, Side s, Qty q, Reason why) {
        return {EventType::Cancelled, s, OrderType::Limit, why, id, 0, 0, q};
    }
    static Event modified(OrderId id, Side s, Price px, Qty q, Reason why) {
        return {EventType::Modified, s, OrderType::Limit, why, id, 0, px, q};
    }
};

// Any callable taking const Event& is a sink. The book's methods are
// templates on the sink type, so the call is resolved at compile time and
// usually inlined: no virtual call, and no std::function (which is an
// indirect call and may heap-allocate).
template <class S>
concept EventSink = requires(S& sink, const Event& e) {
    { sink(e) } -> std::same_as<void>;
};

// Records every event. Used by tests and the differential fuzz.
struct VectorSink {
    std::vector<Event> events;
    void operator()(const Event& e) { events.push_back(e); }
};

// Counts events without storing them. Used by benchmarks so the sink
// itself does not allocate or touch much memory.
struct CountingSink {
    std::uint64_t events = 0;
    std::uint64_t trades = 0;
    Qty traded_qty = 0;
    void operator()(const Event& e) {
        ++events;
        if (e.type == EventType::Trade) {
            ++trades;
            traded_qty += e.qty;
        }
    }
};

// Printing, so gtest shows readable events when a comparison fails.
inline const char* to_string(EventType t) {
    switch (t) {
        case EventType::Accepted: return "Accepted";
        case EventType::Rejected: return "Rejected";
        case EventType::Trade: return "Trade";
        case EventType::Cancelled: return "Cancelled";
        case EventType::Modified: return "Modified";
    }
    return "?";
}

inline const char* to_string(Reason r) {
    switch (r) {
        case Reason::None: return "None";
        case Reason::InvalidQty: return "InvalidQty";
        case Reason::InvalidPrice: return "InvalidPrice";
        case Reason::InvalidType: return "InvalidType";
        case Reason::DuplicateId: return "DuplicateId";
        case Reason::UnknownOrder: return "UnknownOrder";
        case Reason::CapacityExceeded: return "CapacityExceeded";
        case Reason::UserCancel: return "UserCancel";
        case Reason::MarketRemainder: return "MarketRemainder";
        case Reason::IocRemainder: return "IocRemainder";
        case Reason::FokUnfillable: return "FokUnfillable";
        case Reason::KeptPriority: return "KeptPriority";
        case Reason::LostPriority: return "LostPriority";
    }
    return "?";
}

inline const char* to_string(Side s) { return s == Side::Buy ? "Buy" : "Sell"; }

inline const char* to_string(OrderType t) {
    switch (t) {
        case OrderType::Limit: return "Limit";
        case OrderType::Market: return "Market";
        case OrderType::IOC: return "IOC";
        case OrderType::FOK: return "FOK";
    }
    return "?";
}

inline std::ostream& operator<<(std::ostream& os, const Event& e) {
    return os << '{' << to_string(e.type) << " id=" << e.id << ' ' << to_string(e.side) << ' '
              << to_string(e.order_type) << " reason=" << to_string(e.reason)
              << " other=" << e.other_id << " px=" << e.price << " qty=" << e.qty << '}';
}

}  // namespace ob
