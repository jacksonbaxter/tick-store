# libFuzzer needs upstream clang: Apple clang ships no fuzzer runtime.

function(tick_store_add_fuzzer name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE tick_store_core)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    tick_store_apply_warnings(${name})
endfunction()

tick_store_add_fuzzer(fuzz_columns)
tick_store_add_fuzzer(fuzz_container)
tick_store_add_fuzzer(fuzz_dictionary)
tick_store_add_fuzzer(fuzz_gorilla_timestamps)
tick_store_add_fuzzer(fuzz_gorilla_xor)
tick_store_add_fuzzer(fuzz_itch_frame)
tick_store_add_fuzzer(fuzz_itch_message)
tick_store_add_fuzzer(fuzz_itch_store)
tick_store_add_fuzzer(fuzz_pfor)
tick_store_add_fuzzer(fuzz_run_length)
tick_store_add_fuzzer(fuzz_stream_vbyte)
tick_store_add_fuzzer(fuzz_stub_store)

add_executable(generate_seeds generate_seeds.cpp)
target_link_libraries(generate_seeds PRIVATE tick_store_core)
tick_store_apply_warnings(generate_seeds)

# Seeds are regenerated from the encoder on each build, into the build tree.
add_custom_target(fuzz_seeds ALL
    COMMAND generate_seeds ${CMAKE_BINARY_DIR}/fuzz-seeds
    COMMENT "Generating fuzz seeds from the encoder"
    VERBATIM
)
