# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/darkfell/dev/libconveyor/build/_deps/citor-src"
  "/home/darkfell/dev/libconveyor/build/_deps/citor-build"
  "/home/darkfell/dev/libconveyor/build/_deps/citor-subbuild/citor-populate-prefix"
  "/home/darkfell/dev/libconveyor/build/_deps/citor-subbuild/citor-populate-prefix/tmp"
  "/home/darkfell/dev/libconveyor/build/_deps/citor-subbuild/citor-populate-prefix/src/citor-populate-stamp"
  "/home/darkfell/dev/libconveyor/build/_deps/citor-subbuild/citor-populate-prefix/src"
  "/home/darkfell/dev/libconveyor/build/_deps/citor-subbuild/citor-populate-prefix/src/citor-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/darkfell/dev/libconveyor/build/_deps/citor-subbuild/citor-populate-prefix/src/citor-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/darkfell/dev/libconveyor/build/_deps/citor-subbuild/citor-populate-prefix/src/citor-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
