# Run with cmake -P. Every path, include directory and sysroot goes, in joined and
# separate form, and plain flags stay in order.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/SanitiseFlags.cmake)

set(dirty [[-O3 -DNDEBUG -I/Users/someone/include -I relative/include -isystem /opt/x
-isysroot /Applications/SDK.sdk --sysroot=/home/someone/root -arch arm64
-fdebug-prefix-map=/home/someone=. -fsanitize=address -DPATH="/home/someone" -std=c++23
-include secret.h -L/usr/local/lib -F /Library/Frameworks -B /opt/bin ~/bin/flag]])
string(REPLACE "\n" " " dirty "${dirty}")
tick_store_sanitise_flags(clean "${dirty}")

set(expected "-O3 -DNDEBUG -fsanitize=address -std=c++23")
if(NOT clean STREQUAL expected)
    message(FATAL_ERROR "sanitised flags\n  got      '${clean}'\n  expected '${expected}'")
endif()
