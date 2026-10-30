# A compiler can ship newer C++ standard library headers than the shared library
# the platform links by default, and then ordinary code fails to link: headers
# refer to symbols the older library lacks. This tests for that by building a
# small program, rather than by platform, so a matched toolchain is left alone.
#
# When the test fails, the fix is the compiler's own static libc++, which it
# reports through -print-file-name, so no path is written down here.
#
# Mach-O needs one more step. libSystem loads the platform's libc++ into every
# process, and dyld coalesces exported weak definitions across images, so a
# binary's own libc++ vtables get bound to the platform's copy. Two libc++
# instances then share objects, and the first iostream teardown frees a locale
# the other one allocated. An executable has nothing to export, so exporting
# nothing keeps those symbols local to it.
include(CheckCXXSourceCompiles)

function(tick_store_configure_standard_library)
    check_cxx_source_compiles([[
        #include <format>
        #include <string>
        #include <variant>
        int main(int argc, char**) {
            const std::variant<int, char> value{argc};
            return static_cast<int>(std::format("{}", std::get<int>(value)).size());
        }
    ]] TICK_STORE_STDLIB_LINKS_AS_IS)
    if(TICK_STORE_STDLIB_LINKS_AS_IS)
        return()
    endif()

    set(libraries "")
    foreach(library libc++.a libc++abi.a)
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=${library}
            OUTPUT_VARIABLE archive
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(NOT IS_ABSOLUTE "${archive}" OR NOT EXISTS "${archive}")
            message(FATAL_ERROR
                "This compiler cannot link a small C++ program, and it has no ${library} to fall back on")
        endif()
        string(APPEND libraries " \"${archive}\"")
    endforeach()

    message(STATUS "Linking the compiler's static libc++ instead of the default standard library")
    add_link_options(-nostdlib++)
    set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES}${libraries}" PARENT_SCOPE)

    if(CMAKE_EXECUTABLE_FORMAT STREQUAL "MACHO")
        set(no_exports "${CMAKE_BINARY_DIR}/no-exported-symbols.txt")
        file(WRITE "${no_exports}" "")
        add_link_options("LINKER:-exported_symbols_list,${no_exports}")
    endif()
endfunction()
