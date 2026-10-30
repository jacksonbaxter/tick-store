# Fuzz crashers

Each crash input is kept here for good and replayed by `fuzz_regression_test.cpp`
through the same oracle the fuzz target runs (Z6). Put a payload in `itch_message/`
and a stream in `itch_frame/`, named after the bug. Fix the bug in the same commit.
