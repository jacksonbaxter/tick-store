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

# Google Benchmark and HdrHistogram_c go here, through tick_store_declare_dependency
# with an exact URL and URL_HASH, behind their own option, when the benchmarks land.
