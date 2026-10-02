cmake_minimum_required(VERSION 3.24)
project(orderbook_v2 LANGUAGES CXX)

# C++20 for concepts (EventSink) and std::span. Extensions off so the code
# builds the same under gcc and clang on Linux CI.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# Sanitizers apply to every target, gtest included, because ASan needs the
# whole process instrumented to catch bad accesses that cross library lines.
# Values: "" (off), "address,undefined", "thread".
set(OB_SANITIZE "" CACHE STRING "Sanitizer list passed to -fsanitize=")
if(OB_SANITIZE)
  # no-sanitize-recover makes UBSan abort, so any UB fails the test run
  # instead of printing a warning that scrolls past.
  add_compile_options(-fsanitize=${OB_SANITIZE} -fno-omit-frame-pointer
                      -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${OB_SANITIZE})
endif()

# Strict warnings for our code only (not gtest), carried by an interface
# target. -Wconversion catches silent int64 -> int truncation of a price or qty.
add_library(ob_warnings INTERFACE)
target_compile_options(ob_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror)

# The engine is header-only: the book's methods are templates on the event
# sink, so their bodies must be visible to every caller.
add_library(ob INTERFACE)
target_include_directories(ob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

option(OB_BUILD_BENCH "Build the benchmark driver" ON)
if(OB_BUILD_BENCH)
  add_subdirectory(bench)
endif()

add_subdirectory(examples)

option(OB_BUILD_TESTS "Build the test suite" ON)
if(OB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
