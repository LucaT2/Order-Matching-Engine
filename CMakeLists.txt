cmake_minimum_required(VERSION 3.20)
project(HFT_Project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HFT_NATIVE_ARCH "Enable -march=native (non-portable, faster on this machine only)" OFF)
option(HFT_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(HFT_TSAN "Build with ThreadSanitizer (Linux/WSL only; mutually exclusive with HFT_SANITIZE)" OFF)

# Needed by anything using std::thread (SPSC queue tests, future producer/consumer demo)
find_package(Threads REQUIRED)

if (HFT_SANITIZE AND HFT_TSAN)
    message(FATAL_ERROR "HFT_SANITIZE (ASan+UBSan) and HFT_TSAN cannot be combined in one build")
endif()

if (HFT_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address,undefined)
endif()

if (HFT_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=thread)
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

# --- Benchmark: throughput + latency percentiles, comparable against
# unoptimized-map-implementation's benchmarks/benchmark_engine.cpp ---
add_executable(benchmark benchmarks/benchmark_engine.cpp)
target_link_libraries(benchmark PRIVATE matching_engine_core)
target_compile_options(benchmark PRIVATE -Wall -Wextra -O3)

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

# --- SPSC queue tests: separate target since it's an independent component;
# needs Threads::Threads for std::thread-based producer/consumer tests ---
add_executable(spsc_tests tests/SPSCQueueTests.cpp tests/SPSCIntegrationTests.cpp)
target_link_libraries(spsc_tests PRIVATE matching_engine_core GTest::gtest_main Threads::Threads)
target_compile_options(spsc_tests PRIVATE -Wall -Wextra)

gtest_discover_tests(spsc_tests)
