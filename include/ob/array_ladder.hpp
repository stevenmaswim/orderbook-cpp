#pragma once

#include <cstddef>
#include <vector>

#include "ob/assert.hpp"
#include "ob/level.hpp"
#include "ob/types.hpp"

// Price ladder for one side as a flat array (upgrade C): one Level per tick in
// [min_price, max_price], indexed by price - min_price, plus the index of the
// current best level.
//
// Trade-off against MapLadder:
//   + finding a level is an array index, O(1), no tree walk, no allocation
//   + neighbouring prices are neighbours in memory
//   - memory grows with the price band, not with the number of live levels
//   - when the best level empties, we scan toward worse prices for the next
//     non-empty one: O(gap in ticks). Cheap when the book is dense near the
//     top, expensive when it is sparse (the "wide" benchmark workload)
namespace ob {

class ArrayLadder {
public:
    ArrayLadder(Side side, const BookConfig& cfg)
        : side_(side),
          min_price_(cfg.min_price),
          levels_(static_cast<std::size_t>(cfg.max_price - cfg.min_price + 1)) {
        for (std::size_t i = 0; i < levels_.size(); ++i) {
            levels_[i].price = min_price_ + static_cast<Price>(i);
        }
    }

    Level* best() { return best_ == kNone ? nullptr : &levels_[best_]; }
    const Level* best() const { return best_ == kNone ? nullptr : &levels_[best_]; }

    // The caller pushes an order onto the level right after this, so it is
    // safe to make it the best even though it is still empty at this instant.
    Level* get_or_create(Price price) {
        const std::size_t i = index_of(price);
        if (best_ == kNone || better(i, best_)) best_ = i;
        return &levels_[i];
    }

    void erase_empty(Level* level) {
        OB_ASSERT(level->empty());
        const std::size_t i = index_of(level->price);
        // Only losing the best level changes anything; a deeper level that
        // empties just stays in the array as an empty slot.
        if (i == best_) best_ = next_non_empty(i);
    }

    template <class F>
    void for_each_from_best(F&& f) const {
        for (std::size_t i = best_; i != kNone; i = next_non_empty(i)) {
            if (!f(levels_[i])) return;
        }
    }

private:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    std::size_t index_of(Price price) const {
        OB_ASSERT(price >= min_price_ && static_cast<std::size_t>(price - min_price_) < levels_.size());
        return static_cast<std::size_t>(price - min_price_);
    }

    // Bids: higher index (higher price) is better. Asks: lower index is better.
    bool better(std::size_t a, std::size_t b) const { return side_ == Side::Buy ? a > b : a < b; }

    // The next non-empty level strictly worse than index i, or kNone.
    std::size_t next_non_empty(std::size_t i) const {
        if (side_ == Side::Buy) {
            while (i > 0) {
                --i;
                if (!levels_[i].empty()) return i;
            }
        } else {
            while (++i < levels_.size()) {
                if (!levels_[i].empty()) return i;
            }
        }
        return kNone;
    }

    Side side_;
    Price min_price_;
    std::vector<Level> levels_;
    std::size_t best_ = kNone;
};

}  // namespace ob
