cmake_minimum_required(VERSION 3.15)

enable_testing()

# Add a test executable
add_executable(conveyor_basic_test conveyor_test.cpp)

# Link the test executable with libconveyor
target_link_libraries(conveyor_basic_test PRIVATE
    conveyor
)

# Specify include directories for the test
target_include_directories(conveyor_basic_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_test(NAME ConveyorBasicTest COMMAND conveyor_basic_test)