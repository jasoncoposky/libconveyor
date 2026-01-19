cmake_minimum_required(VERSION 3.15)
project(libconveyor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Define the library
add_library(conveyor STATIC
    src/conveyor.cpp
)

# Specify include directories
target_include_directories(conveyor PUBLIC
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Install rules (optional, but good practice for libraries)
install(TARGETS conveyor
    EXPORT conveyor_targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/libconveyor/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# Export package for find_package()
install(EXPORT conveyor_targets
    FILE conveyorTargets.cmake
    NAMESPACE conveyor::
    DESTINATION lib/cmake/conveyor
)

# Generate a config file for find_package()
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/conveyorConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/conveyorConfig.cmake
    @ONLY
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/conveyorConfig.cmake
    DESTINATION lib/cmake/conveyor
)





# Add the test subdirectory

# Enable testing
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(external/googletest)
    add_subdirectory(test)
    add_subdirectory(benchmark)
endif()