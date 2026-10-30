# Fuzzing

Two libFuzzer targets take raw bytes directly, with nothing checking them first:

| Target | Input | Oracle in `itch_oracles.hpp` |
|---|---|---|
| `fuzz_itch_message` | one payload | `check_message`: both decoders agree, decode then encode gives back the exact input, decoding that gives the same message, and a short buffer fails cleanly |
| `fuzz_itch_frame` | a byte stream | `check_stream`: frames are contiguous, every frame plus the reported tail rebuilds the input exactly, every stop is justified by the bytes, and a shorter input gives the same frames that fit |

## Build

Needs upstream clang: Apple clang has no fuzzer runtime. The build regenerates the
seeds from the real encoder into `build/fuzz/fuzz-seeds/{message,frame}`. They are
never committed.

```sh
cmake --preset fuzz && cmake --build --preset fuzz
mkdir -p build/fuzz/corpus/itch_message build/fuzz/corpus/itch_frame build/fuzz/artifacts
```

## Run

From the repository root. New inputs go to the first directory, so the seeds stay
as generated. Budget a run with `-max_total_time`.

```sh
build/fuzz/fuzz/fuzz_itch_message -max_total_time=60 -max_len=256 \
    -rss_limit_mb=2048 -malloc_limit_mb=256 -timeout=10 -print_final_stats=1 \
    -artifact_prefix=build/fuzz/artifacts/ \
    build/fuzz/corpus/itch_message build/fuzz/fuzz-seeds/message

build/fuzz/fuzz/fuzz_itch_frame -max_total_time=60 -max_len=70000 \
    -rss_limit_mb=2048 -malloc_limit_mb=256 -timeout=10 -print_final_stats=1 \
    -artifact_prefix=build/fuzz/artifacts/ \
    build/fuzz/corpus/itch_frame build/fuzz/fuzz-seeds/frame
```

- `-max_len`: no message is longer than 50 bytes, so 256 leaves room for every
  wrong-length case. A frame can hold 65,535 bytes, so 70,000 reaches the largest
  one and the frames after it.
- `-rss_limit_mb` and `-malloc_limit_mb`: the targets allocate at most a few times
  the input size, so hitting either is a defect. libFuzzer reports it as
  out-of-memory, distinct from a crash.
- `fuzz_itch_message` returns -1 for an input over 65,535 bytes, which no frame can
  declare, so it stays out of the corpus.
- The G-3 gate is 30 minutes per target under the heavy-job lock.

## Reach

Replay the corpus with `-runs=0 -print_coverage=1` and read `COVERED_FUNC` and
`UNCOVERED_FUNC` for `src/itch/` and `include/tick_store/itch/`. Every message
type's `decode_as` and `encode_as`, and each `FrameCursor` stop, should be covered.
The remaining uncovered lines are listed below, so a new one stands out.

## Crashers

A crash writes to `build/fuzz/artifacts/`. Move the input to
`tests/unit/crashers/itch_message/` or `itch_frame/` and fix the bug in the same
commit. `fuzz_regression_test.cpp` replays those files through the same oracles
in every test build.
