#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ob/assert.hpp"
#include "ob/types.hpp"

// Order id -> Order* hash table with a fixed number of slots, sized once at
// construction (part of upgrade B). Unlike std::unordered_map, which
// allocates a node on every insert, it never allocates after construction.
//
// Design:
//  - open addressing with linear probing: on a collision, try the next slot.
//    Probes walk adjacent memory, which is cache friendly.
//  - capacity is a power of two at least 2x the max number of entries, so the
//    table is at most half full and probe runs stay short.
//  - Fibonacci hashing (multiply by 2^64 / golden ratio, keep the top bits)
//    spreads sequential ids, which real clients send, across the table.
//  - deletion by backward shift: entries after the hole slide back if that
//    keeps them reachable. No tombstones, so the table never degrades and
//    never needs a rehash.
namespace ob {

template <class V>
class FlatIdMap {
public:
    explicit FlatIdMap(std::size_t max_entries) {
        std::size_t cap = 8;
        while (cap < 2 * max_entries) cap <<= 1;
        slots_.resize(cap);
        mask_ = cap - 1;
        while ((std::size_t{1} << bits_) < cap) ++bits_;
    }

    V* find(OrderId id) const {
        for (std::size_t i = home(id);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.value == nullptr) return nullptr;  // hit a gap: not present
            if (s.key == id) return s.value;
        }
    }

    void insert(OrderId id, V* value) {
        OB_ASSERT(value != nullptr && size_ < slots_.size() / 2);
        std::size_t i = home(id);
        while (slots_[i].value != nullptr) {
            OB_ASSERT(slots_[i].key != id);  // callers never insert a live id twice
            i = (i + 1) & mask_;
        }
        slots_[i] = Slot{id, value};
        ++size_;
    }

    void erase(OrderId id) {
        std::size_t i = home(id);
        for (;; i = (i + 1) & mask_) {
            if (slots_[i].value == nullptr) return;  // hit a gap: not present
            if (slots_[i].key == id) break;
        }
        // Backward shift: walk the run after the hole. An entry at j whose
        // home slot is NOT cyclically in (hole, j] can move into the hole,
        // because its probe path passes through it.
        std::size_t hole = i;
        for (std::size_t j = (hole + 1) & mask_; slots_[j].value != nullptr; j = (j + 1) & mask_) {
            const std::size_t h = home(slots_[j].key);
            const bool stays = hole <= j ? (hole < h && h <= j) : (hole < h || h <= j);
            if (stays) continue;
            slots_[hole] = slots_[j];
            hole = j;
        }
        slots_[hole] = Slot{};
        --size_;
    }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return slots_.size(); }

private:
    struct Slot {
        OrderId key = 0;
        V* value = nullptr;  // nullptr marks an empty slot, so any id is a valid key
    };

    std::size_t home(OrderId id) const {
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ULL) >> (64 - bits_));
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    unsigned bits_ = 0;
    std::size_t size_ = 0;
};

}  // namespace ob
