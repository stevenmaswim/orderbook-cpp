include(FetchContent)

# Pinned to a release archive (not a branch) so every configure gets the
# same gtest sources.
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(ob_unit_tests
  unit/smoke_test.cpp
  unit/validate_test.cpp
  unit/limit_test.cpp
  unit/cancel_test.cpp
  unit/tif_test.cpp
  unit/modify_test.cpp
  unit/invariants_test.cpp
  unit/pool_test.cpp
  unit/spsc_test.cpp)
target_include_directories(ob_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(ob_unit_tests PRIVATE ob ob_warnings GTest::gtest_main Threads::Threads)
gtest_discover_tests(ob_unit_tests DISCOVERY_MODE PRE_TEST)

# The differential fuzz is its own binary so it can be run alone with a big
# OB_FUZZ_SEEDS sweep without rerunning the unit suite.
add_executable(ob_fuzz_tests fuzz_differential.cpp)
target_include_directories(ob_fuzz_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ob_fuzz_tests PRIVATE ob ob_warnings GTest::gtest_main)
gtest_discover_tests(ob_fuzz_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 600)

# Replaces global operator new to count allocations, so it must be its own
# program. Skipped under ASan, which installs its own operator new.
if(NOT OB_SANITIZE)
  add_executable(ob_alloc_tests alloc_test.cpp)
  target_include_directories(ob_alloc_tests PRIVATE ${CMAKE_SOURCE_DIR}/bench)
  target_link_libraries(ob_alloc_tests PRIVATE ob ob_warnings GTest::gtest_main)
  gtest_discover_tests(ob_alloc_tests DISCOVERY_MODE PRE_TEST)
endif()
