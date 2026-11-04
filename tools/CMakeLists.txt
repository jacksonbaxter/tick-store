function(tick_store_add_tool name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE tick_store_harness)
    tick_store_apply_warnings(${name})
endfunction()

tick_store_add_tool(tick_census)
tick_store_add_tool(tick_coldcache)
tick_store_add_tool(tick_export)
tick_store_add_tool(tick_roundtrip)
tick_store_add_tool(tick_slice)

# The codec measurement names the build that made it, which only bench_support knows.
if(TARGET tick_store_bench_support)
    tick_store_add_tool(tick_codec_eval)
    target_link_libraries(tick_codec_eval PRIVATE tick_store_bench_support)
    tick_store_add_tool(tick_reference_eval)
    target_link_libraries(tick_reference_eval PRIVATE tick_store_bench_support)
endif()
