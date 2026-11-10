add_executable(itch_codec_bench itch_codec_bench.cpp)
target_link_libraries(itch_codec_bench PRIVATE tick_store_bench_support benchmark::benchmark)
tick_store_apply_warnings(itch_codec_bench)

# The decode kernels, one kernel and one set per case, on seeded L1-resident input.
add_executable(simd_kernel_bench simd_kernel_bench.cpp)
target_link_libraries(simd_kernel_bench PRIVATE tick_store_bench_support benchmark::benchmark)
tick_store_apply_warnings(simd_kernel_bench)

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

# B2 for the kernel cases: every case of every set this processor runs, at every width,
# matches its reference before anything is timed.
add_test(NAME simd_kernel_bench_every_case_verifies COMMAND simd_kernel_bench --verify-all)
