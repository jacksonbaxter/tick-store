cmake_minimum_required(VERSION 3.28)

project(tick_store VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(TICK_STORE_BUILD_TESTS "Build the GoogleTest unit tests" ON)
option(TICK_STORE_BUILD_FUZZ "Build the libFuzzer targets (needs upstream clang)" OFF)
set(TICK_STORE_SANITIZE "none" CACHE STRING "Sanitizer set: none, asan-ubsan, tsan or fuzz")
set_property(CACHE TICK_STORE_SANITIZE PROPERTY STRINGS none asan-ubsan tsan fuzz)
set(TICK_STORE_DATA_DIR "${PROJECT_SOURCE_DIR}/data" CACHE PATH
    "Directory holding the decompressed ITCH days. Tests that need one skip visibly if it is absent")

include(cmake/StandardLibrary.cmake)
tick_store_configure_standard_library()

include(cmake/Sanitizers.cmake)
tick_store_enable_sanitizers()

include(cmake/Warnings.cmake)
include(cmake/Dependencies.cmake)

add_library(tick_store_core STATIC)
target_sources(tick_store_core PRIVATE
    src/itch/codec.cpp
    src/itch/frame.cpp
)
target_include_directories(tick_store_core PUBLIC include)
target_compile_features(tick_store_core PUBLIC cxx_std_23)
tick_store_apply_warnings(tick_store_core)

if(TICK_STORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
endif()
