cmake_minimum_required(VERSION 3.15)
project(conveyor_basic_test CXX)

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

# Add a test executable for the toy model
add_executable(toy_model_test toy_model.cpp)

# Specify include directories for the toy model test
target_include_directories(toy_model_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)