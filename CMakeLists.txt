cmake_minimum_required(VERSION 3.16)
project(orderbook_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Core library
add_library(orderbook STATIC src/order_book.cpp)
target_include_directories(orderbook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Demo driver
add_executable(orderbook_demo src/main.cpp)
target_link_libraries(orderbook_demo PRIVATE orderbook)

# Tests
enable_testing()
add_executable(orderbook_tests tests/test_order_book.cpp)
target_link_libraries(orderbook_tests PRIVATE orderbook)
add_test(NAME orderbook_tests COMMAND orderbook_tests)
