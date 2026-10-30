# Warnings for our own targets only. Dependencies are built without these, and
# their headers come in as SYSTEM, so an upstream warning cannot fail our build.
function(tick_store_apply_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Werror
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
    )
endfunction()
