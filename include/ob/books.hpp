#pragma once

// Every book variant the tests and benchmarks know about, in one place.
#include "ob/array_ladder.hpp"
#include "ob/baseline_book.hpp"
#include "ob/heap_storage.hpp"
#include "ob/map_ladder.hpp"
#include "ob/order_book.hpp"

namespace ob {

// A: intrusive FIFO levels, std::map ladder, new/delete + unordered_map.
using IntrusiveMapBook = OrderBook<MapLadder, HeapStorage>;

// C: same, with a flat tick-indexed array ladder instead of the map.
using IntrusiveArrayBook = OrderBook<ArrayLadder, HeapStorage>;

}  // namespace ob
