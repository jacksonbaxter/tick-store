# Checks that the scalar-capped ctest parts run exactly the tests the whole filter names
# (R-3): every test the filter lists is in exactly one part, and no part lists a test the
# filter does not, or none at all. gtest itself does the matching, through
# --gtest_list_tests, so a filter's negative patterns mean what they mean to gtest.
#
#   cmake -DBINARY=<unit test binary> -DWHOLE=<filter> -DPARTS=<filter>,<filter>,...
#         -P check_scalar_capped_parts.cmake

function(list_tests filter out)
    execute_process(
        COMMAND "${BINARY}" "--gtest_filter=${filter}" --gtest_list_tests
        OUTPUT_VARIABLE listing
        RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "listing the tests of ${filter} failed: ${status}")
    endif()
    string(REPLACE "\n" ";" lines "${listing}")
    set(tests "")
    set(suite "")
    foreach(line IN LISTS lines)
        if(line MATCHES "^([A-Za-z0-9_/]+\\.)")
            set(suite "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^  ([A-Za-z0-9_/]+)")
            list(APPEND tests "${suite}${CMAKE_MATCH_1}")
        endif()
    endforeach()
    set(${out} "${tests}" PARENT_SCOPE)
endfunction()

list_tests("${WHOLE}" whole)
list(LENGTH whole whole_count)
if(whole_count EQUAL 0)
    message(FATAL_ERROR "the whole filter lists no tests")
endif()

string(REPLACE "," ";" parts "${PARTS}")
set(covered "")
foreach(part IN LISTS parts)
    list_tests("${part}" tests)
    list(LENGTH tests count)
    if(count EQUAL 0)
        message(FATAL_ERROR "the part ${part} lists no tests")
    endif()
    foreach(test IN LISTS tests)
        if(NOT test IN_LIST whole)
            message(FATAL_ERROR "the part ${part} runs ${test}, which the whole filter does not")
        endif()
        if(test IN_LIST covered)
            message(FATAL_ERROR "${test} is in two parts, the second ${part}")
        endif()
        list(APPEND covered "${test}")
    endforeach()
endforeach()

set(missing "")
foreach(test IN LISTS whole)
    if(NOT test IN_LIST covered)
        list(APPEND missing "${test}")
    endif()
endforeach()
list(LENGTH missing missing_count)
if(NOT missing_count EQUAL 0)
    list(JOIN missing "\n  " shown)
    message(FATAL_ERROR "${missing_count} of the ${whole_count} scalar-capped tests are in no part:\n  ${shown}")
endif()
message(STATUS "the parts cover all ${whole_count} scalar-capped tests, each once")
