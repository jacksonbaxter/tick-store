include(GoogleTest)

add_executable(tick_store_unit_tests
    big_endian_test.cpp
    frame_cursor_test.cpp
    itch_boundary_test.cpp
    itch_fixtures.cpp
    itch_golden_test.cpp
    itch_raw_path_test.cpp
    itch_real_day_test.cpp
    itch_truncation_test.cpp
)
target_link_libraries(tick_store_unit_tests PRIVATE tick_store_core GTest::gtest_main)
tick_store_apply_warnings(tick_store_unit_tests)
set_source_files_properties(itch_real_day_test.cpp PROPERTIES
    COMPILE_DEFINITIONS "TICK_STORE_DATA_DIR=\"${TICK_STORE_DATA_DIR}\""
)

gtest_discover_tests(tick_store_unit_tests)
