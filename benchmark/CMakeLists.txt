cmake_minimum_required(VERSION 3.15)

add_executable(conveyor_benchmark conveyor_benchmark.cpp)

target_link_libraries(conveyor_benchmark PRIVATE
    conveyor
)

target_include_directories(conveyor_benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
