cmake_minimum_required(VERSION 3.20)
project(orderbook_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ORDERBOOK_ENABLE_TRACE "Compile in the event tracer" ON)
option(ORDERBOOK_BUILD_BENCH  "Build benchmarks"            OFF)

add_library(orderbook src/order_book.cpp)
target_include_directories(orderbook PUBLIC include)
if(ORDERBOOK_ENABLE_TRACE)
  target_compile_definitions(orderbook PUBLIC ORDERBOOK_TRACE=1)
endif()

add_executable(orderbook_cli src/main.cpp)
target_link_libraries(orderbook_cli PRIVATE orderbook)

include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(orderbook_tests tests/test_order_book.cpp)
target_link_libraries(orderbook_tests PRIVATE orderbook GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(orderbook_tests)

if(ORDERBOOK_BUILD_BENCH)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.0)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
  add_executable(orderbook_bench bench/bench_order_book.cpp)
  target_link_libraries(orderbook_bench PRIVATE orderbook benchmark::benchmark)
endif()
