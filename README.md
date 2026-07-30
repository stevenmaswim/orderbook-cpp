# orderbook-cpp

A price-time-priority limit order book in C++20, with an event tracer, a
Google Benchmark harness, and a visualization layer.

Companion to the Python engine at
[stevenmaswim/order-book](https://github.com/stevenmaswim/order-book).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Status

Scaffold. Matching logic, tracer, benchmark and visuals in progress.
