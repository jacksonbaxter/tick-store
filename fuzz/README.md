# Fuzzing

Four libFuzzer targets take raw bytes directly, with nothing checking them first:

| Target | Input | Oracle in `itch_oracles.hpp` |
|---|---|---|
| `fuzz_columns` | a whole column directory, as length-prefixed sections | `check_columns` in `columns_oracles.hpp`: each file sits in its own heap block, so ASan sees a read past its end. Errors repeat, every frame an accepted directory gives is one whole frame, re-exporting those frames rebuilds the same frames and tail, a cut of any file is refused, and a copy with one bit flipped is refused or gives frames that re-export cleanly |
| `fuzz_itch_message` | one payload | `check_message`: both decoders agree, decode then encode gives back the exact input, decoding that gives the same message, and a short buffer fails cleanly |
| `fuzz_itch_frame` | a byte stream | `check_stream`: frames are contiguous, every frame plus the reported tail rebuilds the input exactly, every stop is justified by the bytes, and a shorter input gives the same frames that fit |
| `fuzz_stub_store` | a whole store file | `check_store` in `store_oracles.hpp`: errors repeat, an accepted file rebuilds the input exactly from its parsed values and holds the records its header counts, a cut of it is refused, and a copy with one bit flipped is refused or rebuilds to the flipped bytes |

## Build

Needs upstream clang: Apple clang has no fuzzer runtime. The build regenerates the
seeds from the real encoder and the real store writer into
`build/fuzz/fuzz-seeds/{message,frame,store,columns}`. They are never committed.

```sh
cmake --preset fuzz && cmake --build --preset fuzz
mkdir -p build/fuzz/corpus/itch_message build/fuzz/corpus/itch_frame \
    build/fuzz/corpus/stub_store build/fuzz/corpus/columns build/fuzz/artifacts
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

build/fuzz/fuzz/fuzz_stub_store -max_total_time=60 -max_len=70000 \
    -rss_limit_mb=2048 -malloc_limit_mb=256 -timeout=10 -print_final_stats=1 \
    -artifact_prefix=build/fuzz/artifacts/ \
    build/fuzz/corpus/stub_store build/fuzz/fuzz-seeds/store

build/fuzz/fuzz/fuzz_columns -max_total_time=60 -max_len=70000 \
    -rss_limit_mb=2048 -malloc_limit_mb=256 -timeout=10 -print_final_stats=1 \
    -artifact_prefix=build/fuzz/artifacts/ \
    build/fuzz/corpus/columns build/fuzz/fuzz-seeds/columns
```

- `-max_len`: no message is longer than 50 bytes, so 256 leaves room for every
  wrong-length case. A frame can hold 65,535 bytes, so 70,000 reaches the largest
  one and the frames after it, and in a store file the header too.
- `-rss_limit_mb` and `-malloc_limit_mb`: the targets allocate at most a few times
  the input size, so hitting either is a defect. libFuzzer reports it as
  out-of-memory, distinct from a crash.
- `fuzz_itch_message` returns -1 for an input over 65,535 bytes, which no frame can
  declare, so it stays out of the corpus.
- The G-3 gate is 30 minutes per target under the heavy-job lock.

## Reach

`-runs=0 -print_coverage=1` lists `COVERED_FUNC` and `UNCOVERED_FUNC`, but at this
optimisation level the per-type templates are inlined into one dispatch function,
so it cannot show that each type was reached. For that, compile `src/itch/` and the
target at `-O0` with `-fprofile-instr-generate -fcoverage-mapping`, replay with
`-runs=0`, and read the instantiations with `llvm-cov show --show-instantiations`.

Measured on the 60-second corpora, with both targets merged:

- `decode_as` is entered for all 23 types, and no line in any of them has a zero
  count, so every field's truncation check and the trailing-bytes check are reached.
- `encode_as` is entered for all 23 types. Its one uncovered branch is the timestamp
  range check, which a value decoded from 6 bytes cannot trip.
- `FrameCursor`, `BigEndianReader`, `BigEndianWriter` and the layout checks are at
  100% of functions, with every `FrameCursor` stop reached.

The fuzzers do not reach the following, all of which the unit tests cover: the
`to_string` formatters and `wire_size_of`, which the oracles do not call, and the
`encode_frame` branches for a short buffer, an oversized raw message or an
out-of-range timestamp, which need a hand-built message or a deliberately small buffer.

## Crashers

A crash writes to `build/fuzz/artifacts/`. Move the input to
`tests/unit/crashers/itch_message/`, `itch_frame/`, `stub_store/` or `columns/` and fix the bug
in the same commit. `fuzz_regression_test.cpp` replays those files through the same
oracles in every test build.
