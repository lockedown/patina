cmake_minimum_required(VERSION 3.14.0)
project(AkaizerCore VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# This top-level CMake project builds and tests the C++ DSP core only
# (Sources/Core). It exists so the core can be developed and validated
# standalone, with no Apple frameworks and no Xcode required -- see the
# project plan, "Build order" stage 1. The macOS app itself (Sources/App,
# Sources/Audio) is a separate Xcode project that links against
# libAkaizerCore.a built here.

enable_testing()

add_subdirectory(Sources/Core)
add_subdirectory(Tests/CoreTests)
