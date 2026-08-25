cmake_minimum_required(VERSION 3.20)
project(HFT_Project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HFT_NATIVE_ARCH "Enable -march=native (non-portable, faster on this machine only)" OFF)
option(HFT_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

if (HFT_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address,undefined)
endif()

# --- Core library: everything under include/ and src/, no main() here ---
add_library(matching_engine_core
    src/OrderBook.cpp
)

target_include_directories(matching_engine_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(matching_engine_core PRIVATE -Wall -Wextra)
if (HFT_NATIVE_ARCH)
    target_compile_options(matching_engine_core PRIVATE -march=native)
endif()

# --- App: links against the core library, only has main.cpp ---
add_executable(matching_engine apps/main.cpp)
target_link_libraries(matching_engine PRIVATE matching_engine_core)

target_compile_options(matching_engine PRIVATE -Wall -Wextra -O3)

# --- Tests: pulls in GoogleTest automatically, no manual install needed ---
enable_testing()

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(unit_tests tests/OrderBookTests.cpp)
target_link_libraries(unit_tests PRIVATE matching_engine_core GTest::gtest_main)
target_compile_options(unit_tests PRIVATE -Wall -Wextra)

include(GoogleTest)
gtest_discover_tests(unit_tests)

# Benchmarks
add_executable(benchmark benchmarks/benchmark_engine.cpp)
target_link_libraries(benchmark PRIVATE matching_engine_core)
target_compile_options(benchmark PRIVATE -Wall -Wextra -O3)

# Same methodology as optimized-array-implementation's apps/benchmark.cpp, ported here for a
# direct, apples-to-apples comparison against BENCHMARK.md
add_executable(benchmark_compare apps/benchmark_compare.cpp)
target_link_libraries(benchmark_compare PRIVATE matching_engine_core)
target_compile_options(benchmark_compare PRIVATE -Wall -Wextra -O3)
