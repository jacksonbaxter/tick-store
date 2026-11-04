# Fuzz crashers

Each crash input is kept here for good and replayed by `fuzz_regression_test.cpp`
through the same oracle the fuzz target runs (Z6). Put a payload in `itch_message/`,
a stream in `itch_frame/`, a store file in `stub_store/`, a sectioned column directory in
`columns/`, a Gorilla column in `gorilla_timestamps/` or `gorilla_xor/`, and an integer
column in `pfor/`, `stream_vbyte/`, `dictionary/` or `run_length/`, named after the bug.
Fix the bug in the same commit.
