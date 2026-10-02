#pragma once

// Every book variant the tests and benchmarks know about, in one place.
#include "ob/array_ladder.hpp"
#include "ob/baseline_book.hpp"
#include "ob/heap_storage.hpp"
#include "ob/map_ladder.hpp"
#include "ob/order_book.hpp"
#include "ob/pool_storage.hpp"

namespace ob {

// A: intrusive FIFO levels, std::map ladder, new/delete + unordered_map.
using IntrusiveMapBook = OrderBook<MapLadder, HeapStorage>;

// C: same, with a flat tick-indexed array ladder instead of the map.
using IntrusiveArrayBook = OrderBook<ArrayLadder, HeapStorage>;

// B: preallocated node pool + flat id table. With the array ladder nothing on
// the hot path allocates (tests/alloc_test.cpp checks this). With the map
// ladder, new price levels still allocate map nodes.
using PoolArrayBook = OrderBook<ArrayLadder, PoolStorage>;
using PoolMapBook = OrderBook<MapLadder, PoolStorage>;

}  // namespace ob
