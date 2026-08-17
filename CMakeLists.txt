cmake_minimum_required(VERSION 3.20)
project(HFT_Project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiles main.cpp located at the root of E:/HFT_Project
add_executable(matching_engine apps/main.cpp)

if (MSVC)
    target_compile_options(matching_engine PRIVATE /W4 /O2 /permissive-)
else()
    target_compile_options(matching_engine PRIVATE -Wall -Wextra -O3 -march=native)
endif()