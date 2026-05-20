#----------------------------------------------------------------
# Generated CMake target import file.
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "libconveyor::conveyor" for configuration ""
set_property(TARGET libconveyor::conveyor APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(libconveyor::conveyor PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_NOCONFIG "CXX"
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libconveyor.a"
  )

list(APPEND _cmake_import_check_targets libconveyor::conveyor )
list(APPEND _cmake_import_check_files_for_libconveyor::conveyor "${_IMPORT_PREFIX}/lib/libconveyor.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
