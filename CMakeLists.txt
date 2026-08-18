cmake_minimum_required(VERSION 3.20)
project(HFT_Project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HFT_NATIVE_ARCH "Enable -march=native (non-portable, faster on this machine only)" OFF)

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
