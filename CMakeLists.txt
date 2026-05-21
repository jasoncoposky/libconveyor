cmake_minimum_required(VERSION 3.15)
project(libconveyor CXX)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  citor
  GIT_REPOSITORY https://github.com/Lallapallooza/citor.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(citor)

FetchContent_Declare(
  concurrentqueue
  GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
  GIT_TAG        master
)
FetchContent_MakeAvailable(concurrentqueue)

# Define the library
add_library(conveyor STATIC
    src/conveyor.cpp
    src/ThreadPool.cpp
)
set_target_properties(conveyor PROPERTIES CMAKE_POSITION_INDEPENDENT_CODE ON)

# Specify include directories
target_include_directories(conveyor PUBLIC
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${concurrentqueue_SOURCE_DIR}>
    $<BUILD_INTERFACE:${citor_SOURCE_DIR}/single_include>
    $<BUILD_INTERFACE:${citor_SOURCE_DIR}/include>
)

# --- Exports ---
# This creates the `conveyor-targets.cmake` file in the build directory
export(TARGETS conveyor
    FILE conveyor-targets.cmake
    NAMESPACE libconveyor::
)

# --- Installation ---
install(TARGETS conveyor
    EXPORT conveyor-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(DIRECTORY include/libconveyor/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# Export package for find_package()
install(EXPORT conveyor-targets
    FILE conveyor-targets.cmake
    NAMESPACE libconveyor::
    DESTINATION lib/cmake/libconveyor
)

include(CMakePackageConfigHelpers)

# ... (rest of the file until the config part)

# Generate a config file for find_package()
configure_package_config_file(conveyorConfig.cmake.in libconveyorConfig.cmake
    INSTALL_DESTINATION lib/cmake/libconveyor
)





# Add the test subdirectory

# Enable testing
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(external/googletest)
    add_subdirectory(test)
    add_subdirectory(benchmark)
endif()