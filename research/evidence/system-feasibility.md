# System feasibility evidence (Phase 1)

Status: complete, smoke tests only, no benchmarks.
Host: Apple M4 Max, macOS 27.2, arm64. All paths are relative to the repo root unless absolute.
`output/` means `research/feasibility-scripts/output/`. Files named `*-recheck*` or `*-current*`, in `output/` were produced by this agent; all others are raw records from the previous agent's run (12:15-12:26 local, 2026-10-30).
CACHE means the research cache, kept outside this repo; `CACHE/...` paths are labels into it. The digest there is used only where marked, for events that no raw output records.

## Summary

| System | Verdict | Version | Python / JDK | Key limit |
|---|---|---|---|---|
| pyarrow | Runs, native arm64 | 26.0.0 | Python 3.13.18 | Page index and bloom filters are written but not used on read |
| ArcticDB | Runs, native arm64, officially supported (not beta since 2025-11-26) | 6.28.2 | Python 3.13.18 | Needs macOS 15+ and `pandas<3`; no `cp315` wheel |
| QuestDB | Runs, native arm64, no Java fallback | 10.1.0 | Temurin 25.0.5, client 6.0.1 | Vector kernels are `Vanilla` scalar dispatch; ILP auto-create can silently lose nanoseconds |

All required smoke checks passed on all three systems. The checks that were not actually done are listed in each section and under "Gaps and items not verified". After the last stop, `ps` and `lsof -i` show nothing left running (`output/final-state-recheck.txt`).

## pyarrow (Parquet)

Result: RUNS. `pyarrow 26.0.0`, native arm64 wheel, `FAILS: 0` in `output/smoke-pyarrow.txt`.

- Package: `pyarrow-26.0.0-cp313-cp313-macosx_12_0_arm64.whl`, 36336700 bytes (`output/wheel-urls.txt`).
- URL: https://files.pythonhosted.org/packages/4d/35/ca95493712af97c46a312945c8e9d16b21c5fe2f148be5466168d0290505/pyarrow-26.0.0-cp313-cp313-macosx_12_0_arm64.whl
- sha256: `a6ca849f90cf73fe361f08a5762c783ead9671e4548c1f558cc637b54c9103f2` (`output/wheel-urls.txt`, `output/archive-sha256.txt`).
- Build: AppleClang 21.0.0, `-march=armv8-a`, release, git `446167169a0dc547e00e1bc29f417a40c0ca267a`, `git_description='apache-arrow-26.0.0-rc0'`, `package_kind='python-wheel-macos'` (`output/smoke-pyarrow.txt`).
- Allocator: mimalloc. `cpu_count` 16, `io_thread_count` 8.
- Python 3.13.18, numpy 2.5.4, pandas 2.3.3 (`output/uv-venv.txt`, `output/venv-freeze.txt`).
- Command: `.venv/bin/python -I research/feasibility-scripts/smoke_pyarrow.py vendor/tmp/pyarrow-smoke`.

Codec and feature matrix, all from `output/smoke-pyarrow.txt`:

| Feature | Result | How it was checked |
|---|---|---|
| zstd | Works, levels 1, 3, 9, 19, 22 | Footer says ZSTD, full read-back equal |
| snappy, gzip, brotli, lz4 | Work | Footer codec and read-back equal |
| `lz4_raw` | Accepted; footer reports `LZ4`, same 182150 bytes as `lz4` | Footer label cannot separate the two |
| BYTE_STREAM_SPLIT on ts, price, size | Works | Encoding asserted from the footer, read-back equal |
| DELTA_BINARY_PACKED on ts, price, size | Works | Encoding asserted from the footer, read-back equal |
| DELTA_LENGTH_BYTE_ARRAY, DELTA_BYTE_ARRAY on symbol | Work | Footer and read-back |
| BYTE_STREAM_SPLIT on a string column | Rejected: `OSError: BYTE_STREAM_SPLIT only supports FLOAT, DOUBLE, INT32, INT64 and FIXED_LEN_BYTE_ARRAY` | Expected, not a build gap |
| DELTA_BINARY_PACKED on a string column | Rejected: `OSError: DELTA_BINARY_PACKED encoder only supports INT32 and INT64` | Expected, not a build gap |
| Page index, write | Works: `has_column_index`, `has_offset_index` true, negative control false | Footer only |
| Page index, read pruning | NOT CHECKED. Range filter returned correct rows, but nothing shows the index was used | `ParquetFile` exposes no page-index API |
| Bloom filter, write | Works for symbol and ts, absent on price as a negative control | Footer offsets and lengths |
| Bloom filter, read use | NOT CHECKED, no read API found | `ParquetFile` has no bloom attribute; `ParquetFragmentScanOptions` shows only `page_checksum_verification` |

Smoke coverage (`research/feasibility-scripts/smoke_pyarrow.py`, `smoke_common.py`):

- Nanosecond timestamp column: checked. Type asserted as `timestamp[ns, tz=UTC]`, and the data has non-zero sub-microsecond digits.
- Symbol column: checked, as a plain string column (dictionary encoded by default on write).
- Integer price `int64`, integer size `int32`: checked, by schema equality.
- Read-back equality: checked, exact on all four columns over 10,000 rows.
- Time-range filter: checked, half-open `[lo, hi)`, 5000 of 5000 rows exact, through `read_table(filters=)` and the dataset scanner.
- There is no negative control for the comparison in this script. `same()` itself is negative-tested only in `smoke_questdb.py`.

## ArcticDB

Result: RUNS. `arcticdb 6.28.2`, LMDB backend, native arm64 wheel, `FAILS: 0` in `output/smoke-arcticdb.txt`. Cross-process persistence also passed (`output/persist-arcticdb.txt`: written by pid 60024, read by pid 60028, `rows 10000 mismatches []`).

- Package: `arcticdb-6.28.2-cp313-cp313-macosx_15_0_arm64.whl`, 17087474 bytes, uploaded 2026-10-26 (`output/wheel-urls.txt`, `output/pypi-tags.txt`). It is the latest release on PyPI at the time of the run.
- URL: https://files.pythonhosted.org/packages/0c/c6/3dcc5c5bc6a30e8eb922bbb3e877da53924679a5b9c22e53e2c3bae7902a/arcticdb-6.28.2-cp313-cp313-macosx_15_0_arm64.whl
- sha256: `1d717af923c38eb9ac254f10d9cb00c024426a8218b6d57a03881da5931e6843` (`output/wheel-urls.txt`, `output/archive-sha256.txt`).
- Python 3.13.18, numpy 2.5.4, pandas 2.3.3. pandas stays below 3 because the wheel declares `Requires-Dist: pandas<3` (`.venv/lib/python3.13/site-packages/arcticdb-6.28.2.dist-info/METADATA`).
- Command: `.venv/bin/python -I research/feasibility-scripts/smoke_arcticdb.py vendor/arcticdb-data/smoke`.
- Library options: all defaults, `rows_per_segment=100000`, `columns_per_segment=127`, `encoding_version=0`, `dedup=False` (`output/smoke-arcticdb.txt`).

Support status on macOS:

- Official and supported, not beta, for this build. The README bundled in the wheel's METADATA and the v6.28.2 README (`CACHE/docs-recheck/arcticdb-README-v6.28.2.txt`) mark `MacOS arm64` as supported on PyPI and conda-forge, and LMDB, S3 and Azure as supported on Mac.
- macOS was beta until recently. README commit `fce2d6f83c0b` of 2025-11-26 changed `| MacOS | Beta | Beta |` to supported (`CACHE/docs-recheck/arcticdb-commit-docs-for-mac-release.txt`). The release notes for v6.5.0 of 2025-12-12 announce macOS support (`CACHE/docs-recheck/arcticdb-release-v6.5.0.txt`). Mac support is therefore under a year old.
- Limitations stated by docs or packaging:
  - The wheel is tagged `macosx_15_0_arm64`, so it needs macOS 15 or later. uv refused to resolve it for its default `macosx_13_0_arm64` target when the previous agent passed `--python-platform`, which that agent then dropped (recorded only in the digest, CACHE/transcript-digests/system-feasibility.md). This host, macOS 27.2, satisfies the tag.
  - PyPI has no macOS x86_64 wheel (README table). This does not affect this host.
  - v6.21.0 fixed a macOS 15 `ImportError: Symbol not found: __hash_memory` on back-deployed wheels (GitHub release notes, read live, not saved). It does not affect 6.28.2 on macOS 27.2.
  - The docs mention macOS nowhere: `CACHE/docs-recheck/arcticdb-docs-index-v6.28.2.txt`, `arcticdb-docs-faq-v6.28.2.txt` and `arcticdb-docs-lmdb-v6.28.2.txt` have no hits for mac, apple, beta or experimental. The docs index still says Python 3.8 - 3.13 while the README says 3.9 - 3.14. The LMDB map-size guidance covers only Windows and Linux, so nothing documents macOS behaviour.
  - LMDB limits apply on every OS: no remote filesystems, and only one `Arctic` instance per LMDB database in a process (`arcticdb-docs-index-v6.28.2.txt`).
- Runtime: the smoke output shows no macOS warning or error. The platform scan (`output/arcticdb-platform-scan.txt`) found no macOS branch outside test fixtures, only `platform.system() != "Linux"` CA-certificate checks in the S3, Azure and GCP adapters, which are irrelevant to LMDB.
- Licence: Business Source License 1.1. Production use needs a paid licence (wheel METADATA). This is a publication and licensing point, not a technical limit.

Smoke coverage (`research/feasibility-scripts/smoke_arcticdb.py`):

- Nanosecond timestamp: checked, but stored as the DataFrame index and not as an ordinary column. Read-back index is `datetime64[ns]` with exact int64 equality.
- Symbol column: checked, as an object string column stored as `UTF_DYNAMIC64`. This is ArcticDB's string column, not a dictionary or categorical type.
- Integer price `int64`, integer size `int32`: checked, dtype asserted after read-back.
- Read-back equality: checked, exact on all four columns.
- Time-range filter: checked with `date_range=` and `QueryBuilder.date_range`, 5000 of 5000 rows exact. `date_range` is inclusive at both ends, so the script subtracts 1 ns to get `[lo, hi)`. The window ends have non-zero nanosecond digits (329 and 699).
- Column filter on price and symbol, and `append`: also checked.
- No negative control in this script, and the data is one small segment, so segment pruning was not exercised.

More ArcticDB detail:

- Native code: `arcticdb_ext.cpython-313-darwin.so` is `Mach-O 64-bit bundle arm64` with `minos 15.0` (`output/native-arch-recheck.txt`).
- LMDB settings: the smoke sets no `map_size` and no extra flags. In the 6.28.2 source the non-Windows default map is 400 GiB and `LMDBStorage.ExtraFlags` defaults to 0, so no `MDB_NOSYNC` or `MDB_WRITEMAP` (CACHE/downloads/arcticdb-6.28.2-src/cpp_arcticdb_storage_lmdb_lmdb_storage.cpp). On this host `data.mdb` was only 376832 bytes after the persistence test, so the map is not preallocated on APFS (`output/persist-arcticdb.txt`).

## QuestDB

Result: RUNS, natively on darwin-aarch64 from `vendor/`, with its root in `vendor/questdb-data/`, telemetry off, and all listeners on 127.0.0.1. `output/smoke-questdb.txt` ends `exit=0`.

- Server: QuestDB 10.1.0, build commit `e11c837f8a169b533540bd6fa57ef3d520b68ad9`, built with JDK 25.0.2 (`output/smoke-questdb.txt`), released 2026-10-22.
- Archive: `questdb-10.1.0-no-jre-bin.tar.gz`, 34276936 bytes (`output/questdb-release.txt`).
- URL: https://github.com/questdb/questdb/releases/download/10.1.0/questdb-10.1.0-no-jre-bin.tar.gz
- sha256: `5d84ca94bb46eacf53fc0d27b30e50d82fa13324069e073a67a60c0cf0de5cff`. This is GitHub's asset digest from the release metadata. QuestDB publishes no checksum file of its own (digest only, not re-checked; `output/questdb-release.txt` lists three assets and none is a checksum file).
- JDK: Temurin 25.0.5+8 LTS, 64-Bit Server VM, `mixed mode, sharing` (`output/java-version.txt`). The QuestDB pom sets `javac.target` to 25 (CACHE/vendor-meta/questdb-pom-10.1.0.xml).
- JDK URL: https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.5%2B8/OpenJDK25U-jdk_aarch64_mac_hotspot_25.0.5_8.tar.gz
- JDK sha256: `36dc14d797aaff6d89aab302cd053fdba6207dfdcf772052fa7b023a94b0e857`, which matches Temurin's published `.sha256.txt` (`output/archive-sha256-recheck.txt`).
- Client: Python `questdb 6.0.1`, wheel `questdb-6.0.1-cp313-cp313-macosx_11_0_arm64.whl`, 9079906 bytes, sha256 `7b495f6a11de4b44afe2fe3aa4bb48e84873a96e373b54bbb77d39023ea9d234`, uploaded 2026-10-29 (`output/pypi-tags.txt`). The hash is pinned in `research/feasibility-scripts/requirements.lock`. The exact file URL is not recorded in any output.
- Commands: `research/feasibility-scripts/qdb.sh start`, then `.venv/bin/python -I research/feasibility-scripts/smoke_questdb.py vendor/questdb-data/import`, then `qdb.sh stop`.

Native libraries and fallbacks:

- Both native libraries load. `libquestdb*.dylib` and `libquestdbr*.dylib` were open in the JVM process, extracted to `vendor/questdb-data/tmp/` (`output/questdb-loaded-natives.txt`, confirmed on the current install in `output/questdb-effective-recheck.txt`). Both are `Mach-O 64-bit ... arm64`, `minos 13.0`, ad-hoc signed (`output/questdb-native-inspect.txt`). The jar ships a `darwin-aarch64` directory holding both (`output/questdb-jar-natives.txt`).
- JVM: `java` and `libjvm.dylib` are thin arm64 Mach-O files, so Rosetta does not apply, and `sysctl.proc_translated` is 0 (`output/native-arch-recheck.txt`). The VM is HotSpot server in mixed mode, with C1 and C2 enabled (`TieredCompilation` true), not interpreter-only.
- No Java fallback was found for the native engine. Library load failure is fatal in `Os.java`, with no Java substitute in that path (`CACHE/docs-recheck/questdb-Os-10.1.0.txt`).
- However, the native vector code runs at the lowest level. Every start logs `darwin-aarch64 [Vanilla,0, 64 bits, 16 processors]` (`output/questdb-first-start.log` line 4, `output/questdb-effective-recheck.txt`). `Vanilla` is QuestDB's name for instruction set 0 (`questdb-Vect-10.1.0.txt`). On aarch64 `getSupportedInstructionSet` returns a constant 0 (`questdb-aarch64-vect-10.1.0.txt`), and the build file says `## on ARM64 we use vanilla arithmetic functions for now`, linking `vec_agg_vanilla.cpp`, `ooo_dispatch_vanilla.cpp` and `geohash_dispatch_vanilla.cpp` (`questdb-CMakeLists-10.1.0.txt`). That is scalar-dispatch C++, not Java, and it is not the AVX2 or AVX-512 paths QuestDB uses on x86-64. I found no NEON intrinsics in `aarch64/vect.cpp`; whether the compiler autovectorises the vanilla code was not checked.
- SQL JIT does work here. The log says `SQL JIT compiler mode: on`, `cairo.sql.jit.mode = on`, and `EXPLAIN` shows `Async JIT Filter workers: 16` on `price > 5000000` (`output/smoke-questdb.txt`). `JitUtil.isJitSupported` includes aarch64.
- Startup warnings: one, only on the first start, under `Configuration issues`: `http.min.bind.to: Replaced by http.min.net.bind.to` (`output/questdb-first-start.log` lines 52-54). The rolling log holds exactly one such block across all starts (`output/questdb-log-scan.txt`), because the template was changed to the new key. On the current config there are 0 config issues and 0 E or C level lines in a fresh start and probe (`output/questdb-effective-recheck.txt`).
- All E-level lines in `output/questdb-log-scan.txt` are the smoke scripts' own bad queries (`Invalid column`, or polling a table not yet created). The last section of that file is broken (`head: illegal line count -- 0`), so `output/questdb-effective-recheck.txt` replaces it.
- Telemetry: effective `telemetry.enabled = false`, `telemetry.disable.completely = true`, `posthog.enabled = false`, and no telemetry table exists (`output/questdb-effective-recheck.txt`).
- Listeners: 127.0.0.1 on 9000, 9003, 8812, 9009 only (`output/questdb-listeners-first-start.txt`, `output/questdb-effective-recheck.txt`, and after every restart in `output/questdb-config-matrix.txt`). UDP listeners are disabled.

Ingest paths, each with exact read-back and exact window of 5000 of 5000 rows on a pre-created `TIMESTAMP_NS, SYMBOL, LONG, INT` table (`output/smoke-questdb.txt`):

| Path | Result | Note |
|---|---|---|
| ILP/HTTP Python client | PASS | WAL table |
| ILP/HTTP raw `POST /write?precision=n` | PASS | HTTP 204 |
| ILP/TCP Python client | PASS | |
| ILP/TCP raw socket | PASS | |
| REST `/imp` CSV, new table with schema | PASS | Needs an explicit nanosecond pattern |
| REST `/imp` CSV, into pre-created WAL table | PASS | |
| SQL `COPY`, non-WAL table | PASS | Finished all import phases |
| SQL `COPY`, WAL table | PASS | `wal_tables` shows `writerTxn` 0 and `sequencerTxn` 0, so this data did not go through the WAL sequencer |

Auto-created tables (no DDL) behave differently, and this is the main data-correctness trap:

| How the table was created | Designated column | ts exact to ns | Source |
|---|---|---|---|
| ILP/HTTP Python client, default conf | `TIMESTAMP_NS` | 10000 of 10000 | `output/smoke-questdb.txt`, `output/questdb-effective-recheck.txt` |
| ILP/TCP Python client, default conf | `TIMESTAMP` (microseconds) | 0; all 10000 silently truncated | `output/questdb-effective-recheck.txt` |
| ILP/TCP Python client, `protocol_version=2` | `TIMESTAMP_NS` | 10000 of 10000 | `output/questdb-effective-recheck.txt` |
| Raw ILP over HTTP or TCP | `TIMESTAMP` (microseconds) | 0; all 10000 silently truncated | `output/smoke-questdb-rawauto.txt` |
| Raw ILP with `line.timestamp.default.column.type=TIMESTAMP_NS` | `TIMESTAMP` (microseconds) | 0; the setting did not change this | `output/smoke-questdb-rawauto.txt` |

- In every auto-created table `size` became `LONG`, not `INT`.
- The setting in the last row was shown as effective in the matrix run (`output/questdb-config-matrix.txt`). The raw-auto run that used the same line did not print it back, so that one run relies on the conf file written just before it (digest).
- `output/smoke-questdb-tsns-default.txt` shows the client path is unchanged by the setting. What the setting does affect was not established.

Commit modes (`output/questdb-config-matrix.txt`):

| `cairo.commit.mode` | Server started | Echoed by `SHOW PARAMETERS` | ILP round trip | Effective mode |
|---|---|---|---|---|
| `nosync` | yes | `nosync` | exact | nosync |
| `async` | yes | `async` | exact | async |
| `sync` | yes | `sync` | exact | sync |
| `not_a_mode` | yes, no warning | `not_a_mode` | exact | Silently nosync |

- `SHOW PARAMETERS` echoes the raw string, so it does not prove a mode was applied. `getCommitMode` falls through to `CommitMode.NOSYNC` for unknown values, and its default when unset is `nosync` (CACHE/downloads/questdb-10.1.0-src/core_src_main_java_io_questdb_PropServerConfiguration.java lines 3078-3097).
- The mode is read by both `TableWriter` (CACHE source) and `WalWriter` (`CACHE/docs-recheck/questdb-WalWriter-10.1.0.txt`).
- The matrix label `default` means the template with `nosync` written explicitly, not an unset key.
- NOT CHECKED: whether `sync` actually flushes more than `nosync`. The smoke proves only that the key parses and rows round-trip. No syscall or latency evidence exists, and system-call tracing would need sudo, which is not allowed here.
- Data survived a clean restart in every run: `restart persistence: s_ilp_http_client exact`. A crash was not tested.

Smoke coverage (`research/feasibility-scripts/smoke_questdb.py`):

- Nanosecond timestamp column: checked. `SHOW COLUMNS` type plus exact `cast(ts AS LONG)` equality.
- Symbol column: checked, as the `SYMBOL` type.
- Integer price `LONG`, integer size `INT`: checked by type and value, on pre-created tables only.
- Read-back equality: checked, exact on all four columns, and this script has a negative control (a one-unit price change is detected).
- Time-range filter: checked with nanosecond bounds. `EXPLAIN` shows `Interval forward scan` with intervals ending `...015128698Z`, so the bound is not rounded to microseconds.
- Auto-create paths were checked for timestamp only. No window filter ran on them.
- NOT CHECKED: reads over PGWire (port 8812 only listens; all reads use HTTP `/exec`), out-of-order ingest, duplicates, more than one partition, realistic symbol cardinality (5 symbols here).

## Workarounds used by the previous agent

| What | Why | Evidence | Benchmark impact |
|---|---|---|---|
| Python 3.13.18 from python-build-standalone, not the host Python 3.15 | `arcticdb==6.28.2` has no `cp315` wheel; full error below | `output/uv-compile-315-vs-314.txt` | All Python clients share 3.13. 3.14 also resolved, so 3.13 is a choice, not a hard limit |
| pandas 2.3.3 | `arcticdb` requires `pandas<3` | wheel METADATA, `output/uv-install.txt` | Shared by all Python paths |
| No Homebrew QuestDB; `no-jre` tarball plus a repo-local Temurin JDK | Brew is forbidden here, and the release has no macOS runtime bundle: its `rt` assets are linux-x86-64 and windows-x86-64 only | `output/questdb-release.txt` | Runtime JDK is 25.0.5, while QuestDB was built with 25.0.2 |
| `qdb.sh` runs `java` directly and never calls `questdb.sh` | `questdb.sh` defaults its root to `$HOME/.questdb` or `/usr/local/var/questdb` | `research/feasibility-scripts/qdb.sh`, `vendor/questdb/questdb-10.1.0-no-jre-bin/questdb.sh` | JVM flags differ from upstream; see below |
| Added `-XX:-UsePerfData` | Stops the JVM creating `hsperfdata_*` in the user temp dir | `qdb.sh`; digest | Not in upstream flags; expected to be small but not measured |
| Added `-Djava.io.tmpdir=vendor/questdb-data/tmp` | Keeps extracted native libraries inside the repo | `qdb.sh`, `output/questdb-loaded-natives.txt` | None expected |
| `server.conf` template: loopback binds, telemetry off, UDP off, `cairo.commit.mode=nosync` | Hard constraints. `nosync` equals the unset default | `research/feasibility-scripts/questdb-server.conf` | See fairness |
| `http.min.bind.to` replaced by `http.min.net.bind.to` in the template | Startup deprecation notice | `output/questdb-first-start.log` | None |
| Dropped `uv pip compile --python-platform` | It targeted macOS 13 and `arcticdb` needs macOS 15 tags | digest only | None; final lock resolved for the real interpreter |

Everything else in `qdb.sh` matches `questdb.sh` lines 356-384, including `-ea -Dnoebug`, `-XX:+AlwaysPreTouch`, `-XX:+UseParallelGC`, `-XX:-MaxFDLimit`, and raising the soft fd limit to the hard limit. `qdb.sh` passes the label `-DQuestDB-Runtime-66535` and `-Dcontainerized=false` directly.

Script fixes that were bugs in the smoke tests, not product problems (digest only): the first pyarrow run failed `codec none` with `ValueError: Invalid value for compression: 'uncompressed'`; the first QuestDB run failed the auto-create check with `Invalid column: ts` because the auto-created column is named `timestamp`; `from questdb.ingress import Sender` raised a `DeprecationWarning`, so the script now uses `from questdb import Sender`.

Full error for the Python pin (`output/uv-compile-315-vs-314.txt`):

```
error: No solution found when resolving dependencies
  cause: Because arcticdb==6.28.2 has no wheels with a matching Python ABI tag (e.g., `cp315`) and you require arcticdb==6.28.2, we can conclude that your requirements are unsatisfiable.

hint: You require CPython 3.15 (`cp315`), but we only found wheels for `arcticdb` (v6.28.2) with the following Python ABI tags: `cp39`, `cp310`, `cp311`, `cp312`, `cp313`, `cp314`
```

## Benchmark fairness: call-outs

1. QuestDB's vector kernels run at `Vanilla` level on this machine, as shown above. Results here do not represent its AVX2 or AVX-512 paths on x86-64, and any Apple Silicon result must say so. It is native C++, not a Java fallback.
2. `pyarrow` reports `simd_level='none'`, but that field cannot show NEON: `MakeSimdLevelString` tests only AVX512, AVX2, AVX and SSE4_2 (`CACHE/docs-recheck/arrow-config.cc-26.0.0.txt`). Do not read it as "no SIMD", and do not read it as proof of NEON either. NEON use in this wheel was not established. The wheel is compiled for generic `-march=armv8-a`, so a `tick-store` build with `-mcpu=native` or similar has a compiler-target advantage that must be disclosed.
3. Parquet page indexes and bloom filters are written, but Arrow's own docs say "data read APIs do not currently make any use of them" (CACHE/downloads/arrow-26-src/docs_source_cpp_parquet.rst, footnote at lines 662-663). No benchmark should credit Parquet with page-index or bloom pruning through pyarrow unless that is measured separately.
4. Durability is not equal between systems as configured. QuestDB runs `nosync`. ArcticDB uses LMDB with no extra flags, so LMDB's default syncing applies. Parquet fsync behaviour was not examined here. Ingest comparisons must state the mode, or run `sync` for QuestDB and disclose it. Note that `sync` itself is unverified, and a typo in the key silently gives `nosync`.
5. QuestDB ILP is asynchronous in two ways. WAL apply happens after the client returns, so the smoke polls `count()`, and raw TCP gives no acknowledgement. Ingest time must end when rows are visible, for example when `writerTxn` equals `sequencerTxn`, not when the client returns.
6. Never rely on ILP auto-create. Pre-create tables with `TIMESTAMP_NS` and `INT`. The default TCP client and raw ILP silently lose nanoseconds, and auto-created `size` is `LONG`, which inflates storage against the other systems.
7. `COPY` into a WAL table bypasses the WAL sequence (`writerTxn` 0), so it is a different write path from ILP and must be labelled as such.
8. `symbol` is a different thing in each system: QuestDB `SYMBOL` (dictionary), Parquet string with dictionary encoding by default, ArcticDB `UTF_DYNAMIC64` string. `ts` is the DataFrame index in ArcticDB and a column elsewhere. Compression ratios and filter speeds compare these representations, not one shared layout.
9. QuestDB JVM: `-ea` stays on, as in upstream, so Java assertions run. No `-Xmx` is set, so the ergonomic maximum heap is 17179869184 bytes (16 GiB) with a 1 GiB initial heap and 13 parallel GC threads (`output/native-arch-recheck.txt`). `AlwaysPreTouch` and JIT warm-up cost memory and time. RSS was about 1.43 GB right after the first start (`output/questdb-first-start.log`). Fix heap settings explicitly and report warm and cold runs.
10. Client overhead differs. QuestDB is a server reached over loopback, while pyarrow and ArcticDB are in-process through Python, pandas 2.3.3 and numpy 2.5.4. Times that include Python are not engine times.
11. ArcticDB uses defaults: 100000 rows per segment, 127 columns per segment, LMDB. Results apply to this LMDB-on-APFS setup, and LMDB's documented sizing advice does not cover macOS.
12. Version freshness: `questdb` client 6.0.1 was uploaded one day before the run, and `arcticdb` 6.28.2 four days before. Mac support in ArcticDB is under a year out of beta. Keep the exact pins in any published result.
13. The smoke data is 10,000 rows, 5 symbols, one day, strictly increasing. Passing says nothing about scale, out-of-order data, or high cardinality.

## Disk use and what is left

Current figures are from `output/disk-use-recheck.txt`, after this agent removed its own scratch tree. `output/disk-use.txt` is stale: its `vendor` 1.0G still includes `vendor/dl` and `vendor/meta`, which have moved.

| Item | Size |
|---|---|
| `vendor/` total | 762M (780388 KiB, 7814 files) |
| `.venv/` total | 341M |
| pyarrow in `.venv` | 123M |
| arcticdb in `.venv` | 90M (`arcticdb` 2.1M plus `arcticdb_ext` 88M) |
| questdb client in `.venv` | 44M |
| pandas, numpy in `.venv` (shared) | 48M, 24M |
| `vendor/questdb` | 34M |
| `vendor/jdk` | 294M |
| `vendor/questdb-data` | 31M, holds the smoke tables and logs |
| `vendor/arcticdb-data` | 424K |
| `vendor/uv-python` | 69M |
| `vendor/cache` (uv cache, shared) | 333M |
| `research/feasibility-scripts` | 820K, of which `output/` 708K |
| Outside repo: CACHE/vendor-dl | 238M |
| Outside repo: CACHE/vendor-meta | 3.8M |

Archive checksums in CACHE/vendor-dl were recomputed and match `output/archive-sha256.txt` for all five archives (`output/archive-sha256-recheck.txt`). The CPython archive also matches the published sum in `output/python-sha256-published.txt`.

Nothing is left running: after the last stop, `pgrep -x java` found nothing, and `lsof` showed no java and nothing on 9000, 9003, 9009 or 8812 (`output/questdb-effective-recheck.txt`). `vendor/tmp` is empty. This agent's only additions to `vendor/` are log lines in `vendor/questdb-data/log/` and fresh extracted libraries in `vendor/questdb-data/tmp/`, removed on stop.

## Repeatable setup

`research/feasibility-scripts/reproduce.zsh` is the setup. It uses `research/feasibility-scripts/env.sh`, and it needs `/opt/homebrew/bin/uv` (0.13.6 was used). In order, it does the following:

```zsh
export REPO=$PWD
F=$REPO/research/feasibility-scripts; V=$REPO/vendor
. $F/env.sh        # all uv, pip and temp paths under vendor/; CACHE defaults to the research cache outside this repo
DL=$CACHE/vendor-dl
/opt/homebrew/bin/uv python install 3.13.18
/opt/homebrew/bin/uv venv --python $V/uv-python/cpython-3.13.18-macos-aarch64-none/bin/python3.13 $REPO/.venv
/opt/homebrew/bin/uv pip install --python $REPO/.venv/bin/python --require-hashes --only-binary :all: -r $F/requirements.lock
curl -fsSL -o $DL/questdb-10.1.0/questdb-10.1.0-no-jre-bin.tar.gz https://github.com/questdb/questdb/releases/download/10.1.0/questdb-10.1.0-no-jre-bin.tar.gz
curl -fsSL -o $DL/temurin-25.0.5/OpenJDK25U-jdk_aarch64_mac_hotspot_25.0.5_8.tar.gz "https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.5%2B8/OpenJDK25U-jdk_aarch64_mac_hotspot_25.0.5_8.tar.gz"
# shasum -a 256 -c both archives, using the two sums in this file
tar xzf $DL/questdb-10.1.0/questdb-10.1.0-no-jre-bin.tar.gz -C $V/questdb
tar xzf $DL/temurin-25.0.5/OpenJDK25U-jdk_aarch64_mac_hotspot_25.0.5_8.tar.gz -C $V/jdk
cp $F/questdb-server.conf $V/questdb-data/conf/server.conf   # must exist before first start
$F/qdb.sh start; $F/qdb.sh stop                              # smoke tests run between these
```

- The script also creates the directories first and runs all three smoke tests.
- `output/reproduce-validation.txt` tested an older script, before the path change. So this agent re-ran the current `reproduce.zsh` on a clean copy, with `REPO` and `CACHE` set to a scratch tree in `vendor/tmp/repro-current` (since removed). Result: `reproduce rc=0`, about 22 seconds of wall time, checksums OK, `FAILS: 0` twice, all QuestDB paths PASS (`output/reproduce-validation-current.txt`).
- Run it only on a clean tree. On the real paths it re-downloads into CACHE, and its behaviour with an existing `.venv` was not tested.
- Setting `REPO` to a scratch tree also needs a copy of the scripts there.

## Did anything land outside the repo?

The audit that answers this is kept outside the repo, with the other private records.

## Gaps and items not verified

- `sync` and `async` commit modes: no evidence of actual flushing beyond parsing and round trips.
- pyarrow read-side use of page index and bloom filters: not exercised; Arrow's docs say unused.
- NEON use inside the pyarrow wheel: not established.
- Autovectorisation of QuestDB's vanilla kernels: not checked.
- No required measurement failed because of the environment, and no system lacked a native route, so nothing here is blocked on the user.
