add_executable(ob_demo demo.cpp)
target_include_directories(ob_demo PRIVATE ${CMAKE_SOURCE_DIR}/bench)
target_link_libraries(ob_demo PRIVATE ob ob_warnings)
