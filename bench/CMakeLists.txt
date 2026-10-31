add_executable(itch_codec_bench itch_codec_bench.cpp)
target_link_libraries(itch_codec_bench PRIVATE tick_store_bench_support benchmark::benchmark)
tick_store_apply_warnings(itch_codec_bench)

# B2 from the outside: a pin that does not match stops the run before any timing. Any
# small file serves, since the input hash is the first check.
add_test(NAME itch_codec_bench_refuses_a_wrong_pin
    COMMAND itch_codec_bench
        --slice=${CMAKE_CURRENT_LIST_FILE}
        --expect-sha256=0000000000000000000000000000000000000000000000000000000000000000
        --expect-messages=1
        --case=itch_decode
        --report=${CMAKE_BINARY_DIR}/test-scratch/refused-report.json
)
set_tests_properties(itch_codec_bench_refuses_a_wrong_pin PROPERTIES
    PASS_REGULAR_EXPRESSION "does not match the pin"
)
