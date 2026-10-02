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
  unit/smoke_test.cpp)
target_link_libraries(ob_unit_tests PRIVATE ob ob_warnings GTest::gtest_main)
gtest_discover_tests(ob_unit_tests DISCOVERY_MODE PRE_TEST)
