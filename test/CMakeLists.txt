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

add_executable(conveyor_modern_test conveyor_modern_test.cpp)

# # Link the C++23 modern API test executable with libconveyor, gtest, gmock
target_link_libraries(conveyor_modern_test PRIVATE
    conveyor
    gtest
    gmock
    gtest_main
)

# # Specify include directories for the C++23 modern API test
target_include_directories(conveyor_modern_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/expected/include
)

add_test(NAME ConveyorModernTest COMMAND conveyor_modern_test)

# Add the stress test executable
add_executable(conveyor_stress_test conveyor_stress_test.cpp)

# Link the stress test executable with libconveyor and gtest
target_link_libraries(conveyor_stress_test PRIVATE
    conveyor
    gtest
    gmock
    gtest_main
)

# Specify include directories for the stress test
target_include_directories(conveyor_stress_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)