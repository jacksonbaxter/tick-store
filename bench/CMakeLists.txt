add_executable(itch_codec_bench itch_codec_bench.cpp)
target_link_libraries(itch_codec_bench PRIVATE tick_store_bench_support benchmark::benchmark)
tick_store_apply_warnings(itch_codec_bench)

# The decode kernels, one kernel and one set per case, on seeded L1-resident input.
add_executable(simd_kernel_bench simd_kernel_bench.cpp)
target_link_libraries(simd_kernel_bench PRIVATE tick_store_bench_support benchmark::benchmark)
tick_store_apply_warnings(simd_kernel_bench)

# The real-data column decodes: the slice's add-order prices and timestamps.
add_executable(column_decode_bench column_decode_bench.cpp)
target_link_libraries(column_decode_bench PRIVATE tick_store_bench_support benchmark::benchmark)
tick_store_apply_warnings(column_decode_bench)

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

# The same for the column cases, a wrong pin stopping the run before any column is built.
add_test(NAME column_decode_bench_refuses_a_wrong_pin
    COMMAND column_decode_bench
        --slice=${CMAKE_CURRENT_LIST_FILE}
        --expect-sha256=0000000000000000000000000000000000000000000000000000000000000000
        --expect-messages=1
        --verify-all
)
set_tests_properties(column_decode_bench_refuses_a_wrong_pin PROPERTIES
    PASS_REGULAR_EXPRESSION "does not match the pin"
)

# B2 on the real input: the pinned slice verifies, and every column case gives back its
# column exactly. The slice is data, not in the repository, so the test exists only where
# it does (scripts/fetch_data.py and tick_slice make it), and configure says so otherwise.
file(READ "${PROJECT_SOURCE_DIR}/scripts/data_pins.json" TICK_STORE_PINS)
string(JSON TICK_STORE_SLICE_PATH GET "${TICK_STORE_PINS}" slices 12302019-open path)
string(JSON TICK_STORE_SLICE_SHA GET "${TICK_STORE_PINS}" slices 12302019-open sha256)
string(JSON TICK_STORE_SLICE_MESSAGES GET "${TICK_STORE_PINS}" slices 12302019-open message_count)
if(EXISTS "${PROJECT_SOURCE_DIR}/${TICK_STORE_SLICE_PATH}")
    add_test(NAME column_decode_bench_every_case_verifies
        COMMAND column_decode_bench
            --slice=${PROJECT_SOURCE_DIR}/${TICK_STORE_SLICE_PATH}
            --expect-sha256=${TICK_STORE_SLICE_SHA}
            --expect-messages=${TICK_STORE_SLICE_MESSAGES}
            --verify-all
    )
else()
    message(STATUS "No ${TICK_STORE_SLICE_PATH}, so column_decode_bench_every_case_verifies is not registered")
endif()
