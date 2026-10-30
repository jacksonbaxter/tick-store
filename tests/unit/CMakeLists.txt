include(GoogleTest)

add_executable(tick_store_unit_tests
    big_endian_test.cpp
    frame_cursor_test.cpp
    itch_fixtures.cpp
    itch_golden_test.cpp
)
target_link_libraries(tick_store_unit_tests PRIVATE tick_store_core GTest::gtest_main)
tick_store_apply_warnings(tick_store_unit_tests)

gtest_discover_tests(tick_store_unit_tests)
