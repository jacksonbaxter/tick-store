# Run by the build, in script mode: writes the build-info source with the current
# commit. An exported tree has no .git, and carries the commit in a COMMIT file.
# TICK_STORE_COMMIT_SOURCE says which (review A-8): "git" when git named the commit and
# compared the tracked files with it, "export" when a COMMIT file named it and nothing
# was compared, so tree_dirty is false only because no difference is known, and
# "unknown" when neither could.
include(${VALUES})

set(TICK_STORE_COMMIT "unknown")
set(TICK_STORE_TREE_DIRTY "true")
set(TICK_STORE_COMMIT_SOURCE "unknown")
if(EXISTS ${SOURCE_DIR}/COMMIT)
    file(STRINGS ${SOURCE_DIR}/COMMIT TICK_STORE_COMMIT LIMIT_COUNT 1)
    set(TICK_STORE_TREE_DIRTY "false")
    set(TICK_STORE_COMMIT_SOURCE "export")
else()
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} -C ${SOURCE_DIR} rev-parse HEAD
            OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE commit_failed ERROR_QUIET)
        execute_process(COMMAND ${GIT_EXECUTABLE} -C ${SOURCE_DIR} status --porcelain --untracked-files=no
            OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE status_failed ERROR_QUIET)
        if(NOT commit_failed)
            set(TICK_STORE_COMMIT "${commit}")
            set(TICK_STORE_COMMIT_SOURCE "git")
        endif()
        if(NOT commit_failed AND NOT status_failed AND changes STREQUAL "")
            set(TICK_STORE_TREE_DIRTY "false")
        endif()
    endif()
endif()

configure_file(${TEMPLATE} ${OUTPUT} @ONLY)
