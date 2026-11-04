# Third-party code is pinned here by exact URL and hash, and never vendored.
#
# Each preset would otherwise fetch its own copy, so downloads and sources are
# shared under FETCHCONTENT_BASE_DIR (the presets set it to build/_deps). The
# binary directory stays per preset: FetchContent's default puts it in the shared
# directory too, where builds with different sanitizers would overwrite each
# other's objects. Every dependency therefore goes through this helper.
include(FetchContent)

function(tick_store_declare_dependency name)
    FetchContent_Declare(${name}
        ${ARGN}
        BINARY_DIR ${CMAKE_BINARY_DIR}/_deps/${name}-build
        SYSTEM
        EXCLUDE_FROM_ALL
    )
endfunction()

if(TICK_STORE_BUILD_TESTS)
    tick_store_declare_dependency(googletest
        URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
        URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    )
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# The core-only build is the histogram engine alone: no zlib, no threads, no logging.
# It backs tick_store_bench_support, so the tests need it as well as the benchmarks.
if(TICK_STORE_BUILD_TESTS OR TICK_STORE_BUILD_BENCH)
    tick_store_declare_dependency(hdrhistogram
        # Tag 0.12.1.
        URL https://github.com/HdrHistogram/HdrHistogram_c/archive/30e9ce6ef5dcdc60f9270e1ca03c0966eaaf71d0.tar.gz
        URL_HASH SHA256=7f882d20defe473d3e42e8424805db6b37cbec1d044082f76ca1ab32726dbc8d
    )
    set(HDR_HISTOGRAM_CORE_ONLY ON CACHE BOOL "" FORCE)
    set(HDR_LOG_REQUIRED OFF CACHE BOOL "" FORCE)
    set(HDR_HISTOGRAM_DISABLE_AVX2 ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(hdrhistogram)
endif()

# zstd at level 1 wraps the store's columns where it pays (codec/zstd_frame.hpp), so the core
# library needs it in every preset, and it is the generic baseline tick_codec_eval measures the
# column codecs against. Only the static library is built, single-threaded, without legacy
# formats or the command-line programs.
tick_store_declare_dependency(zstd
    # Tag v1.5.7.
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    SOURCE_SUBDIR build/cmake
)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zstd)

if(TICK_STORE_BUILD_BENCH)
    tick_store_declare_dependency(benchmark
        # Tag v1.9.5.
        URL https://github.com/google/benchmark/archive/192ef10025eb2c4cdd392bc502f0c852196baa48.tar.gz
        URL_HASH SHA256=f82705a2726d8f6cdcda274b841f6314dbfc6f731cdda06c946f310ec1cc3ad9
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_INSTALL_TOOLS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)
endif()
