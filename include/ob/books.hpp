#pragma once

// Every book variant the tests and benchmarks know about, in one place.
#include "ob/baseline_book.hpp"
#include "ob/heap_storage.hpp"
#include "ob/map_ladder.hpp"
#include "ob/order_book.hpp"

namespace ob {

// A: intrusive FIFO levels, std::map ladder, new/delete + unordered_map.
using IntrusiveMapBook = OrderBook<MapLadder, HeapStorage>;

}  // namespace ob
