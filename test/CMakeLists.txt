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

# Add the C++23 modern API test executable
add_executable(modern_test modern_test.cpp)

# Link the C++23 modern API test executable with libconveyor, gtest, gmock
target_link_libraries(modern_test PRIVATE
    conveyor
    gtest
    gmock
    gtest_main
)

# Specify include directories for the C++23 modern API test
target_include_directories(modern_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(NAME ModernApiTest COMMAND modern_test)

# Add the adaptive test executable
add_executable(adaptive_test adaptive_test.cpp)

# Link the adaptive test executable with libconveyor, gtest, gmock
target_link_libraries(adaptive_test PRIVATE
    conveyor
    gtest
    gmock
    gtest_main
)

# Specify include directories for the adaptive test
target_include_directories(adaptive_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(NAME AdaptiveTest COMMAND adaptive_test)



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

add_executable(conveyor_multi_thread_test conveyor_multi_thread_test.cpp)

target_link_libraries(conveyor_multi_thread_test PRIVATE
    conveyor
    gtest
    gmock
    gtest_main
)

target_include_directories(conveyor_multi_thread_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_test(NAME ConveyorMultiThreadTest COMMAND conveyor_multi_thread_test)

add_executable(conveyor_consistency_test conveyor_consistency_test.cpp)

target_link_libraries(conveyor_consistency_test PRIVATE
    conveyor
    gtest
    gmock
    gtest_main
)

target_include_directories(conveyor_consistency_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_test(NAME ConveyorConsistencyTest COMMAND conveyor_consistency_test)

add_executable(conveyor_partial_io_test conveyor_partial_io_test.cpp)

target_link_libraries(conveyor_partial_io_test PRIVATE
    conveyor
    gtest
    gmock
    gtest_main
)

target_include_directories(conveyor_partial_io_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_test(NAME ConveyorPartialIOTest COMMAND conveyor_partial_io_test)

add_executable(conveyor_illegal_op_test conveyor_illegal_op_test.cpp)

target_link_libraries(conveyor_illegal_op_test PRIVATE
    conveyor
    gtest
    gmock
    gtest_main
)

target_include_directories(conveyor_illegal_op_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_test(NAME ConveyorIllegalOpTest COMMAND conveyor_illegal_op_test)

add_executable(conveyor_multi_instance_test conveyor_multi_instance_test.cpp)

target_link_libraries(conveyor_multi_instance_test PRIVATE
    conveyor
    gtest
    gmock
    gtest_main
)

target_include_directories(conveyor_multi_instance_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

add_test(NAME ConveyorMultiInstanceTest COMMAND conveyor_multi_instance_test)