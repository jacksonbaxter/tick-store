include(GoogleTest)

add_executable(tick_store_unit_tests
    bench_support_test.cpp
    big_endian_test.cpp
    bit_stream_test.cpp
    census_test.cpp
    codec_eval_test.cpp
    codec_real_day_test.cpp
    crc32c_test.cpp
    dictionary_test.cpp
    durability_test.cpp
    columns_cli_test.cpp
    columns_flip_test.cpp
    columns_test.cpp
    container_test.cpp
    container_writer_test.cpp
    frame_cursor_test.cpp
    fuzz_regression_test.cpp
    gorilla_timestamps_test.cpp
    gorilla_xor_test.cpp
    hash_worker_test.cpp
    int_primitives_test.cpp
    itch_boundary_test.cpp
    itch_fixtures.cpp
    itch_golden_test.cpp
    itch_query_test.cpp
    itch_raw_path_test.cpp
    itch_real_day_test.cpp
    itch_store_corrupt_test.cpp
    itch_store_test.cpp
    itch_truncation_test.cpp
    json_writer_test.cpp
    oracle_broken_store_test.cpp
    oracle_flip_test.cpp
    oracle_test.cpp
    page_cache_test.cpp
    pfor_test.cpp
    reference_eval_test.cpp
    roundtrip_cli_test.cpp
    run_length_test.cpp
    simd_kernels_test.cpp
    sha256_test.cpp
    slice_test.cpp
    stream_vbyte_test.cpp
    stub_store_test.cpp
)
target_link_libraries(tick_store_unit_tests PRIVATE
    tick_store_bench_support tick_store_harness GTest::gtest_main)
tick_store_apply_warnings(tick_store_unit_tests)
target_include_directories(tick_store_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/fuzz)
set_source_files_properties(fuzz_regression_test.cpp PROPERTIES
    COMPILE_DEFINITIONS "TICK_STORE_CRASHER_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/crashers\""
)
set_source_files_properties(itch_real_day_test.cpp codec_real_day_test.cpp PROPERTIES
    COMPILE_DEFINITIONS "TICK_STORE_DATA_DIR=\"${TICK_STORE_DATA_DIR}\""
)

target_compile_definitions(tick_store_unit_tests PRIVATE
    TICK_STORE_TEST_SCRATCH_DIR="${CMAKE_BINARY_DIR}/test-scratch"
    TICK_STORE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
)

gtest_discover_tests(tick_store_unit_tests DISCOVERY_TIMEOUT 60)

# The same codec, store and query tests again with the kernels capped to the scalar set,
# so the path every processor can fall back to runs everywhere (S1). SimdDispatch fails
# here if the cap did not take (S6). Every codec suite whose decodes reach the kernels is
# here: run-length and the registry decode nested PFOR blocks, patch lists unpack by the
# kernels, and the codec evaluation decodes PFOR and Stream VByte (B-2, docs/REVIEW.md).
set(TICK_STORE_SCALAR_CAPPED_FILTER
    "Simd*:Pfor*:StreamVByte*:Dictionary*:RunLength*:LanePack*:LinearPack*:PatchList*:CodecRegistry*:CodecEval*:ItchStore*:ItchQuery*:FuzzOracles*:FuzzRegression*")
# One ctest entry ran all of it, and on the Linux host it passed check.sh's 900 s limit
# for a single test (R-3), so it runs as these parts, each a gtest filter, the slowest
# tests on their own. simd_capped_to_scalar_partition checks that the parts together
# list exactly the tests the filter above lists, each in one part.
set(TICK_STORE_SCALAR_CAPPED_PARTS
    "codecs=Simd*:Pfor*:StreamVByte*:Dictionary*:RunLength*:LanePack*:LinearPack*:PatchList*:CodecRegistry*:CodecEval*"
    "store=ItchStore*:FuzzRegression*"
    "query=ItchQuery*-ItchQuery.SegmentsWithRawRowsTakeTheExactPath"
    "query_exact_path=ItchQuery.SegmentsWithRawRowsTakeTheExactPath"
    "fuzz_oracles=FuzzOracles*-FuzzOracles.PassEveryCutAndFlipThroughTheQueryOracle"
    "fuzz_query_oracle=FuzzOracles.PassEveryCutAndFlipThroughTheQueryOracle"
)
set(scalar_capped_filters "")
foreach(part IN LISTS TICK_STORE_SCALAR_CAPPED_PARTS)
    if(NOT part MATCHES "^([a-z_]+)=([^,]+)$")
        message(FATAL_ERROR "A scalar-capped part is not name=filter: ${part}")
    endif()
    add_test(NAME simd_capped_to_scalar_${CMAKE_MATCH_1}
        COMMAND tick_store_unit_tests "--gtest_filter=${CMAKE_MATCH_2}")
    set_tests_properties(simd_capped_to_scalar_${CMAKE_MATCH_1} PROPERTIES
        ENVIRONMENT "TICK_STORE_SIMD=scalar")
    list(APPEND scalar_capped_filters "${CMAKE_MATCH_2}")
endforeach()
list(JOIN scalar_capped_filters "," scalar_capped_filters)
add_test(NAME simd_capped_to_scalar_partition
    COMMAND ${CMAKE_COMMAND}
        "-DBINARY=$<TARGET_FILE:tick_store_unit_tests>"
        "-DWHOLE=${TICK_STORE_SCALAR_CAPPED_FILTER}"
        "-DPARTS=${scalar_capped_filters}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_scalar_capped_parts.cmake)
