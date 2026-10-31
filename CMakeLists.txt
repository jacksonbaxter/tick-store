cmake_minimum_required(VERSION 3.28)

project(tick_store VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(TICK_STORE_BUILD_TESTS "Build the GoogleTest unit tests" ON)
option(TICK_STORE_BUILD_FUZZ "Build the libFuzzer targets (needs upstream clang)" OFF)
option(TICK_STORE_BUILD_TOOLS "Build the correctness oracle and the data tools" ON)
option(TICK_STORE_BUILD_BENCH "Build the benchmarks (pulls in Google Benchmark)" OFF)
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
include(cmake/BuildInfo.cmake)

add_library(tick_store_core STATIC)
target_sources(tick_store_core PRIVATE
    src/itch/codec.cpp
    src/itch/frame.cpp
    src/store/columns.cpp
    src/store/mapped_file.cpp
    src/store/stub_store.cpp
)
target_include_directories(tick_store_core PUBLIC include)
target_compile_features(tick_store_core PUBLIC cxx_std_23)
tick_store_apply_warnings(tick_store_core)

# The correctness harness. It is never linked into the store itself.
if(TICK_STORE_BUILD_TESTS OR TICK_STORE_BUILD_TOOLS OR TICK_STORE_BUILD_BENCH)
    find_package(Threads REQUIRED)
    add_library(tick_store_harness STATIC)
    target_sources(tick_store_harness PRIVATE
        src/harness/census.cpp
        src/harness/columns_reader.cpp
        src/harness/compare_tools_cli.cpp
        src/harness/data_tools_cli.cpp
        src/harness/flip_bit.cpp
        src/harness/hash_worker.cpp
        src/harness/roundtrip.cpp
        src/harness/roundtrip_cli.cpp
        src/harness/sha256.cpp
        src/harness/slice.cpp
        src/harness/stub_reader.cpp
    )
    # Page-cache control does not exist in one form everywhere, so the build picks the
    # file and no caller needs an #ifdef.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_sources(tick_store_harness PRIVATE src/harness/page_cache_linux.cpp)
    else()
        target_sources(tick_store_harness PRIVATE src/harness/page_cache_darwin.cpp)
    endif()
    target_link_libraries(tick_store_harness PUBLIC tick_store_core Threads::Threads)
    tick_store_apply_warnings(tick_store_harness)
endif()

# What the benchmarks stand on: pinning, the timer, the resolution guard. Unit tested
# in every test preset, so it stays free of Google Benchmark.
if(TICK_STORE_BUILD_TESTS OR TICK_STORE_BUILD_BENCH)
    add_library(tick_store_bench_support STATIC)
    target_sources(tick_store_bench_support PRIVATE
        src/bench_support/counter_gate.cpp
        src/bench_support/latency_recorder.cpp
        src/bench_support/resolution_guard.cpp
        src/bench_support/stream_check.cpp
        src/bench_support/timer.cpp
    )
    # Pinning does not exist on every platform, so the build picks the file and no
    # caller needs an #ifdef.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_sources(tick_store_bench_support PRIVATE src/bench_support/affinity_linux.cpp)
    else()
        target_sources(tick_store_bench_support PRIVATE src/bench_support/affinity_unsupported.cpp)
    endif()
    tick_store_generate_build_info(tick_store_bench_support)
    target_link_libraries(tick_store_bench_support
        PUBLIC tick_store_harness
        PRIVATE hdr_histogram_core_static)
    tick_store_apply_warnings(tick_store_bench_support)
endif()

if(TICK_STORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
    add_test(NAME sanitise_flags
        COMMAND ${CMAKE_COMMAND} -P ${PROJECT_SOURCE_DIR}/tests/cmake/sanitise_flags_test.cmake)
endif()

if(TICK_STORE_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(TICK_STORE_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(TICK_STORE_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()
