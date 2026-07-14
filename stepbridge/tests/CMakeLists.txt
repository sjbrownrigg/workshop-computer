cmake_minimum_required(VERSION 3.13)
project(stepbridge_tests CXX)
set(CMAKE_CXX_STANDARD 17)

# Host-side unit tests — no Pico SDK. sequencer.cpp has zero hardware
# dependencies so it compiles cleanly with the host toolchain.
add_executable(test_runner
    test_sequencer.cpp
    ../sequencer.cpp
)
target_include_directories(test_runner PRIVATE ..)
target_compile_options(test_runner PRIVATE -Wall -Wextra)
