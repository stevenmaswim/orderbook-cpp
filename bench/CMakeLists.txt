add_executable(ob_bench bench_main.cpp)
target_include_directories(ob_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ob_bench PRIVATE ob ob_warnings)
