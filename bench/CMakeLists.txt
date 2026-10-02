add_executable(ob_bench bench_main.cpp)
target_include_directories(ob_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ob_bench PRIVATE ob ob_warnings)

find_package(Threads REQUIRED)
add_executable(ob_pipeline_bench pipeline_main.cpp)
target_include_directories(ob_pipeline_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ob_pipeline_bench PRIVATE ob ob_warnings Threads::Threads)
