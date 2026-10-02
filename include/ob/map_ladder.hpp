#pragma once

#include <map>

#include "ob/level.hpp"
#include "ob/types.hpp"

// Price ladder for one side, backed by std::map (a red-black tree).
//
// Any ladder must provide:
//   Level* best()                    best level, or nullptr if the side is empty
//   Level* get_or_create(Price)      level at that price, created empty if needed
//   void   erase_empty(Level*)       drop a level whose FIFO just emptied
//   void   for_each_from_best(F)     visit levels best first while F returns true
//
// Map node addresses never move, so a Level* stays valid until that level is
// erased. Cost: O(log L) to find or create a level, O(1) for best() (cached
// begin()), and one heap allocation per new price level.
namespace ob {

class MapLadder {
public:
    MapLadder(Side side, const BookConfig&) : levels_(PriceOrder{side == Side::Buy}) {}

    Level* best() { return levels_.empty() ? nullptr : &levels_.begin()->second; }
    const Level* best() const { return levels_.empty() ? nullptr : &levels_.begin()->second; }

    Level* get_or_create(Price price) {
        auto [it, created] = levels_.try_emplace(price);
        if (created) it->second.price = price;
        return &it->second;
    }

    // O(log L): the Level does not know its own map iterator, so erase by key.
    void erase_empty(Level* level) {
        OB_ASSERT(level->empty());
        levels_.erase(level->price);
    }

    template <class F>
    void for_each_from_best(F&& f) const {
        for (const auto& [price, level] : levels_) {
            if (!f(level)) return;
        }
    }

private:
    // Bids sort high to low, asks low to high, so begin() is always the best.
    struct PriceOrder {
        bool descending;
        bool operator()(Price a, Price b) const { return descending ? a > b : a < b; }
    };
    std::map<Price, Level, PriceOrder> levels_;
};

}  // namespace ob
