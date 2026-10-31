# Sanitizer flags apply to the whole build tree, dependencies included, because
# ASan and TSan instrumentation has to agree across everything that is linked.
# The set is chosen by TICK_STORE_SANITIZE, which the presets fix.
function(tick_store_enable_sanitizers)
    if(TICK_STORE_SANITIZE STREQUAL "none")
        return()
    elseif(TICK_STORE_SANITIZE STREQUAL "coverage")
        # Not a sanitizer, but it has the same shape: one flag set for the whole tree.
        add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
        add_link_options(-fprofile-instr-generate)
        return()
    elseif(TICK_STORE_SANITIZE STREQUAL "asan-ubsan")
        set(compile_flags -fsanitize=address,undefined)
        set(link_flags -fsanitize=address,undefined)
    elseif(TICK_STORE_SANITIZE STREQUAL "tsan")
        set(compile_flags -fsanitize=thread)
        set(link_flags -fsanitize=thread)
    elseif(TICK_STORE_SANITIZE STREQUAL "fuzz")
        # fuzzer-no-link instruments code for coverage without pulling in
        # libFuzzer's main. Each fuzz target adds -fsanitize=fuzzer at link.
        set(compile_flags -fsanitize=fuzzer-no-link,address,undefined)
        set(link_flags -fsanitize=address,undefined)
    else()
        message(FATAL_ERROR "Unknown TICK_STORE_SANITIZE value: ${TICK_STORE_SANITIZE}")
    endif()

    add_compile_options(${compile_flags} -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(${link_flags})
endfunction()
