# Reduces a compile command to the flags that can go in a public results file. A
# path, an include directory or a sysroot names the machine and the user, and says
# nothing about the code, so all of it goes. What stays is a plain flag such as -O3.
#
# The rule is an allowlist of shape rather than a list of known bad flags: a token
# with a path separator, a quote or a home marker is dropped whatever flag carries
# it, so a flag nobody thought of cannot leak one.
function(tick_store_sanitise_flags out flags)
    separate_arguments(tokens UNIX_COMMAND "${flags}")
    set(kept "")
    set(skip_next FALSE)
    foreach(token IN LISTS tokens)
        if(skip_next)
            set(skip_next FALSE)
        elseif(token MATCHES "^-(I|L|F|B|isystem|iquote|idirafter|isysroot|iprefix|include|imacros|arch|target|-sysroot|-gcc-toolchain)$")
            # The value is the next token. It may not look like a path, so it is
            # dropped by position.
            set(skip_next TRUE)
        elseif(token MATCHES "^-(I|L|F|B|isystem|iquote|idirafter|isysroot)")
            # Joined form, -Ipath.
        elseif(token MATCHES "[/\\\\~\"']")
            # Any path, whatever flag it is joined to.
        else()
            list(APPEND kept "${token}")
        endif()
    endforeach()
    list(JOIN kept " " result)
    set(${out} "${result}" PARENT_SCOPE)
endfunction()
