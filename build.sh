#!/usr/bin/env bash
# Build + run without CMake. Apple clang (installed with Xcode CLT) is all you need.
# Usage: ./build.sh
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build

echo "building tests + demo..."
clang++ -std=c++17 -O2 -Wall -Wextra -I include src/order_book.cpp tests/test_order_book.cpp -o build/orderbook_tests
clang++ -std=c++17 -O2 -I include src/order_book.cpp src/main.cpp -o build/orderbook_demo

echo
echo "=== tests ==="
./build/orderbook_tests
echo
echo "=== demo ==="
./build/orderbook_demo
