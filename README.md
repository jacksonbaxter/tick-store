# tick-store

A C++23 columnar time-series store for Nasdaq TotalView-ITCH 5.0 tick data. A
**record** is one ITCH message of the source file.

Status: Phase 2, harness. `BENCHMARKS.md` holds the targets and `TASKS.md` the plan.

## Build and test

CMake 3.28 or newer and Ninja. The compiler comes from the environment (`CC`,
`CXX`), so nothing here names a toolchain path.

```sh
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

Presets, each building into `build/<preset>`: `dev`, `asan-ubsan`, `tsan`, `fuzz`,
`bench`. Every build preset caps itself at 6 parallel jobs. Fuzzing is described in
`fuzz/README.md`.

## Round-trip check

Gates G-1 and G-2 of `BENCHMARKS.md`. `tick_roundtrip` stores a day, then proves that
every record comes back bit-exact. Fetch and verify the data with `scripts/fetch_data.py`
first, and take the pin from `decompressed_sha256` in `scripts/data_pins.json`.

```sh
cmake --preset bench && cmake --build --preset bench
tools=build/bench/tools
day=data/12302019.NASDAQ_ITCH50
pin=5d81c2e14a0f748b29c674b6a342796932702034b4dd341e39e9a9ec5bac610f

$tools/tick_roundtrip ingest $day build/12302019.stub
$tools/tick_roundtrip verify $day build/12302019.stub --expect-sha256 $pin
```

`verify` walks the source's frames and the stored records in lock step, comparing every
byte, and hashes both whole streams on worker threads. It prints a JSON summary, with
records, bytes, both hashes, the verdict and the wall time. It exits 0 on a pass, 1 on a
mismatch and 2 on a usage or I/O error. A mismatch names the first difference: record
index, source byte offset, bit, expected and actual byte, and message type.

To see it catch corruption, flip one bit of the store, where bit 0 is the least
significant. Flipping the same bit again restores the file.

```sh
$tools/tick_roundtrip flip-bit build/12302019.stub 1390407067 1
$tools/tick_roundtrip verify $day build/12302019.stub --expect-sha256 $pin   # exits 1
$tools/tick_roundtrip flip-bit build/12302019.stub 1390407067 1
```

The store here is a stub with no checksum, on purpose, so the oracle is what catches
the fault. Each step needs about 30 seconds and the store takes as much disk as the
day. Results for both pinned days are in `results/`.

`tick_census <source> <output.json>` writes the aggregate statistics in `results/`.
`tick_slice <source> <output> [--from HH:MM:SS] [--to HH:MM:SS]` writes a small valid
stream for the comparison harness: every `S` and `R` of the day plus one window.
