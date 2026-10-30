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
`bench`.
