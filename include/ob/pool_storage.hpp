#pragma once

#include <cstddef>
#include <vector>

#include "ob/assert.hpp"
#include "ob/flat_id_map.hpp"
#include "ob/level.hpp"
#include "ob/types.hpp"

// Order storage with zero heap allocation after construction (upgrade B).
//
//  - every Order node comes from one slab allocated up front
//  - free nodes form a singly linked free list threaded through their own
//    `next` pointer, so acquire and release are O(1) pointer swaps
//  - ids are found through a FlatIdMap, also sized up front
//
// When the slab is used up, full() is true and the book rejects new limit
// orders with CapacityExceeded. There is deliberately no fallback to new: a
// hidden allocation under load is exactly the latency spike this removes.
namespace ob {

class PoolStorage {
public:
    explicit PoolStorage(const BookConfig& cfg) : slab_(cfg.max_orders), index_(cfg.max_orders) {
        // Thread every node onto the free list, front of the slab first.
        for (std::size_t i = 0; i + 1 < slab_.size(); ++i) slab_[i].next = &slab_[i + 1];
        free_ = slab_.empty() ? nullptr : &slab_[0];
    }
    PoolStorage(const PoolStorage&) = delete;
    PoolStorage& operator=(const PoolStorage&) = delete;

    bool full() const { return free_ == nullptr; }

    Order* acquire() {
        OB_ASSERT(free_ != nullptr);
        Order* o = free_;
        free_ = o->next;
        *o = Order{};  // hand out a clean node, no stale links
        return o;
    }

    void release(Order* o) {
        OB_ASSERT(o >= slab_.data() && o < slab_.data() + slab_.size());
        o->next = free_;
        free_ = o;
    }

    Order* find(OrderId id) const { return index_.find(id); }
    void insert(OrderId id, Order* o) { index_.insert(id, o); }
    void erase(OrderId id) { index_.erase(id); }
    std::size_t size() const { return index_.size(); }

private:
    std::vector<Order> slab_;
    Order* free_ = nullptr;
    FlatIdMap<Order> index_;
};

}  // namespace ob
