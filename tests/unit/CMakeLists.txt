include(GoogleTest)

add_executable(tick_store_unit_tests
    big_endian_test.cpp
    frame_cursor_test.cpp
    fuzz_regression_test.cpp
    hash_worker_test.cpp
    itch_boundary_test.cpp
    itch_fixtures.cpp
    itch_golden_test.cpp
    itch_raw_path_test.cpp
    itch_real_day_test.cpp
    itch_truncation_test.cpp
    sha256_test.cpp
    stub_store_test.cpp
)
target_link_libraries(tick_store_unit_tests PRIVATE tick_store_harness GTest::gtest_main)
tick_store_apply_warnings(tick_store_unit_tests)
target_include_directories(tick_store_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/fuzz)
set_source_files_properties(fuzz_regression_test.cpp PROPERTIES
    COMPILE_DEFINITIONS "TICK_STORE_CRASHER_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/crashers\""
)
set_source_files_properties(itch_real_day_test.cpp PROPERTIES
    COMPILE_DEFINITIONS "TICK_STORE_DATA_DIR=\"${TICK_STORE_DATA_DIR}\""
)

target_compile_definitions(tick_store_unit_tests PRIVATE
    TICK_STORE_TEST_SCRATCH_DIR="${CMAKE_BINARY_DIR}/test-scratch"
)

gtest_discover_tests(tick_store_unit_tests DISCOVERY_TIMEOUT 60)
