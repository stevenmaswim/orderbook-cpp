#pragma once

#include <cstddef>
#include <unordered_map>

#include "ob/level.hpp"
#include "ob/types.hpp"

// Order storage policy: where Order nodes come from and how an id finds its
// node. Any storage must provide:
//   bool   full() const            no room for another resting order
//   Order* acquire()               a fresh node (never null unless full())
//   void   release(Order*)         give a node back
//   Order* find(OrderId) const     the resting order with this id, or nullptr
//   void   insert(OrderId, Order*)
//   void   erase(OrderId)
//   size_t size() const            number of resting orders
//
// HeapStorage is the simple version: new/delete per order and a
// std::unordered_map index. Both allocate on the hot path; upgrade B replaces
// them with a preallocated pool and a flat hash table.
namespace ob {

class HeapStorage {
public:
    explicit HeapStorage(const BookConfig&) {}
    HeapStorage(const HeapStorage&) = delete;
    HeapStorage& operator=(const HeapStorage&) = delete;
    ~HeapStorage() {
        for (auto& [id, order] : index_) delete order;
    }

    bool full() const { return false; }
    Order* acquire() { return new Order{}; }
    void release(Order* o) { delete o; }

    Order* find(OrderId id) const {
        auto it = index_.find(id);
        return it == index_.end() ? nullptr : it->second;
    }
    void insert(OrderId id, Order* o) { index_.emplace(id, o); }
    void erase(OrderId id) { index_.erase(id); }
    std::size_t size() const { return index_.size(); }

private:
    std::unordered_map<OrderId, Order*> index_;
};

}  // namespace ob
