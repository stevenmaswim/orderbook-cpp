#pragma once

#include "ob/assert.hpp"
#include "ob/types.hpp"

// Intrusive FIFO (upgrade A). The order *is* the list node: its prev/next
// pointers live inside it, so there is no separate std::list node to allocate,
// and removing an order needs only the order itself. That gives O(1) cancel
// once the id index hands us the Order*.
namespace ob {

struct Level;

// Layout (upgrade F audit, see bench/experiments/layout/): 64 bytes, no
// padding except 7 bytes after `side`. Every field here is touched when the
// order is matched or cancelled, so there is no cold field worth splitting
// out; dropping the redundant `price` (the level knows it) and aligning to 64
// bytes were both measured and did not give a consistent win.
struct Order {
    Order* prev = nullptr;  // toward the front (older)
    Order* next = nullptr;  // toward the back (newer)
    Level* level = nullptr; // the level this order rests on
    OrderId id = 0;
    Price price = 0;
    Qty open = 0;
    Seq seq = 0;
    Side side = Side::Buy;
};

// One price level: a doubly linked FIFO plus its running total.
//
// 32 bytes, so four levels share one 128-byte cache line on the M5 and a
// line never splits a level. An earlier version also kept an order count
// that nothing read; removing it took Level from 40 to 32 bytes and made the
// array ladder measurably faster, most of all on the sparse workload where
// the best-level scan walks many levels (bench/experiments/layout/).
struct Level {
    Order* head = nullptr;  // oldest order: the next to fill
    Order* tail = nullptr;  // newest order: where arrivals go
    Qty total = 0;          // sum of open qty, kept up to date on every change
    Price price = 0;

    bool empty() const { return head == nullptr; }

    // Arrivals join at the back: that is time priority.
    void push_back(Order* o) {
        o->prev = tail;
        o->next = nullptr;
        o->level = this;
        if (tail) tail->next = o;
        else head = o;
        tail = o;
        total += o->open;
    }

    // Remove from anywhere in the list in O(1) by relinking the neighbours.
    void unlink(Order* o) {
        OB_ASSERT(o->level == this);
        if (o->prev) o->prev->next = o->next;
        else head = o->next;
        if (o->next) o->next->prev = o->prev;
        else tail = o->prev;
        total -= o->open;
        o->prev = o->next = nullptr;
        o->level = nullptr;
    }
};

static_assert(sizeof(Order) == 64, "Order layout changed: re-run the layout experiments");
static_assert(sizeof(Level) == 32, "Level layout changed: re-run the layout experiments");

}  // namespace ob
