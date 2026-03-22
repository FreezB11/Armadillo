cmake_minimum_required(VERSION 3.16)
project(MotionTracker CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release build: full optimisation
# Debug build:   no optimisation, symbols for gdb
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g")

add_executable(tracker
    main.cpp
    capture.cpp
)

# No external libraries needed — V4L2 is part of the Linux kernel headers.
# The only thing we link is the C++ standard library (implicit).

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Binary will be: ${CMAKE_BINARY_DIR}/tracker")