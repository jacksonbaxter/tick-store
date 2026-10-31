# Compile-time facts for a results file: compiler, sanitised flags, commit. The
# compiler and flags are fixed at configure time. The commit is read at build time,
# on every build, because configure does not rerun when HEAD moves and a stale commit
# would pin a number to code that did not produce it. The generated file is rewritten
# only when it changes, so an unchanged commit recompiles nothing.
include(${CMAKE_CURRENT_LIST_DIR}/SanitiseFlags.cmake)

function(tick_store_generate_build_info target)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type_upper)
    get_directory_property(directory_options COMPILE_OPTIONS)
    list(JOIN directory_options " " directory_flags)
    tick_store_sanitise_flags(flags
        "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${build_type_upper}} ${CMAKE_CXX23_STANDARD_COMPILE_OPTION} ${directory_flags}")

    set(generated_dir ${CMAKE_BINARY_DIR}/generated)
    set(values ${generated_dir}/build_info_values.cmake)
    set(output ${generated_dir}/build_info.cpp)
    file(CONFIGURE OUTPUT ${values} CONTENT [[
set(TICK_STORE_COMPILER_ID "@CMAKE_CXX_COMPILER_ID@")
set(TICK_STORE_COMPILER_VERSION "@CMAKE_CXX_COMPILER_VERSION@")
set(TICK_STORE_COMPILE_FLAGS "@flags@")
set(TICK_STORE_BUILD_TYPE "@CMAKE_BUILD_TYPE@")
set(TICK_STORE_SANITIZER_SET "@TICK_STORE_SANITIZE@")
]] @ONLY)

    add_custom_target(tick_store_build_info
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
            -DVALUES=${values}
            -DTEMPLATE=${PROJECT_SOURCE_DIR}/src/bench_support/build_info.cpp.in
            -DOUTPUT=${output}
            -P ${PROJECT_SOURCE_DIR}/cmake/GenerateBuildInfo.cmake
        BYPRODUCTS ${output}
        VERBATIM
    )
    target_sources(${target} PRIVATE ${output})
    add_dependencies(${target} tick_store_build_info)
endfunction()
