#pragma once

#include <cstdint>
#include <ostream>

#include "ob/event.hpp"
#include "ob/types.hpp"

// One input operation. The fuzz generator, the benchmark traces and the
// replay tools all speak this type, so "apply this stream to that book" is
// the same code everywhere.
namespace ob {

enum class OpKind : std::uint8_t { Submit, Cancel, Modify };

struct Op {
    OpKind kind = OpKind::Submit;
    // Submit uses every field. Cancel uses order.id. Modify uses order.id,
    // order.price (new price) and order.qty (new open qty).
    NewOrder order;
};

inline Op submit_op(const NewOrder& o) { return {OpKind::Submit, o}; }
inline Op cancel_op(OrderId id) { return {OpKind::Cancel, NewOrder{id, Side::Buy, OrderType::Limit, 0, 0}}; }
inline Op modify_op(OrderId id, Price px, Qty q) {
    return {OpKind::Modify, NewOrder{id, Side::Buy, OrderType::Limit, px, q}};
}

// Works for any book type with submit/cancel/modify templates on the sink.
template <class Book, EventSink S>
void apply(Book& book, const Op& op, S& sink) {
    switch (op.kind) {
        case OpKind::Submit: book.submit(op.order, sink); break;
        case OpKind::Cancel: book.cancel(op.order.id, sink); break;
        case OpKind::Modify: book.modify(op.order.id, op.order.price, op.order.qty, sink); break;
    }
}

inline std::ostream& operator<<(std::ostream& os, const Op& op) {
    switch (op.kind) {
        case OpKind::Submit:
            return os << "submit{id=" << op.order.id << ' ' << to_string(op.order.side) << ' '
                      << to_string(op.order.type) << " px=" << op.order.price
                      << " qty=" << op.order.qty << '}';
        case OpKind::Cancel: return os << "cancel{id=" << op.order.id << '}';
        case OpKind::Modify:
            return os << "modify{id=" << op.order.id << " px=" << op.order.price
                      << " qty=" << op.order.qty << '}';
    }
    return os;
}

}  // namespace ob
