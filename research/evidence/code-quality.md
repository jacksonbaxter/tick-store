# Code-quality bar: evidence from open-source codec and storage implementations

Phase 1 research. Status: complete. The review checklist is section 16, gaps are section 15.

Every file:line below was confirmed in the clone in this task. The transcript digest of the
killed agent was used as a lead only. CACHE is the research cache, kept outside this repo; `CACHE/...` paths are labels into it.

Clones (shallow, CACHE/downloads/<name>), with the commit SHA that all links pin to:

| Project | Repo | SHA |
|---|---|---|
| beringei | facebookarchive/beringei | 75c3002b179d99c8709323d605e7d4b53484035c |
| prometheus | prometheus/prometheus | 9e28ec50ba751345a1f01a4fdc0d6887f533a37b |
| ClickHouse | ClickHouse/ClickHouse | 2a49505f2c31cc1c1b0be339fbf786fc823e6599 |
| duckdb | duckdb/duckdb | 06ec9d1248940afbf987e0ef51c1abda2cca4b44 |
| cwida-ALP | cwida/ALP | 31ca0ed11c93c99d3f5b5c30e01a3e1c3832d3ce |
| simdcomp | lemire/simdcomp | d5301778fe5045ca8099252de5a6ed8016a287df |
| FastPFor | lemire/FastPFor | ae4e62db4de1ce6904e278f94afb56a87a36b32c |
| streamvbyte | lemire/streamvbyte | edc6105b24e150cd109ece75e6e37f54291635b0 |
| arrow | apache/arrow | 5585e501e2593b1c12ca688a7fdb160df0f07161 |
| highway | google/highway | 62868412cf235b2064000a339010e200e93ac754 |
| ArcticDB | man-group/ArcticDB | 96bc048942f6041909db3d39dacd1c55cbad1ff7 |
| questdb | questdb/questdb | 8ea2a41fe8a1ccbdef29c475cb014327942ce7b1 |

Citation form: each section gives a pinned base URL. `path:N` means `<base>/path#LN`.
"Observation" means something read in the code. Nothing here was built or run, so a defect
called out below is a code-reading result and not a reproduced crash unless it says so.

## 1. Beringei (Facebook, archived; the reference Gorilla implementation)

Base: https://github.com/facebookarchive/beringei/blob/75c3002b179d99c8709323d605e7d4b53484035c
(paths below are under `beringei/lib/`).

Decoding untrusted input
- Single checked primitive: every read goes through `readValueFromBitString`, which throws
  `std::runtime_error` when `bitPos + bitsToRead > data.size() * 8` (BitUtil.cpp:54-60). It then
  loops one bit at a time (BitUtil.cpp:62-67), so safety costs one compare per field plus a
  per-bit loop. This is the simplest correct shape and the slowest.
- Error model: `readValues` catches `std::runtime_error`, logs, and returns the partial `count`
  (TimeSeriesStream-inl.h:65, 116-119). A truncated stream is therefore indistinguishable from a
  short one unless the caller compares `count` with `n`.
- Stream has no header. Point count `n` is caller-supplied (TimeSeriesStream-inl.h:55-60) and
  drives `reserve(out.size() + n)` before any data is validated (TimeSeriesStream-inl.h:66).
- Observation (real weakness): `readNextValue` reads 5 bits of leading zeros and 6 bits of block
  size plus 1 (TimeSeriesStream.cpp:304-308), then computes `previousTrailingZeros = 64 -
  blockSize - leadingZeros` in `uint64_t` with no check (TimeSeriesStream.cpp:309). With
  leadingZeros 31 and blockSize 64 this wraps, and `xorValue <<= previousTrailingZeros`
  (TimeSeriesStream.cpp:311) shifts by 64 or more, which is undefined behaviour in C++. The same
  value feeds the reuse path (TimeSeriesStream.cpp:300-302).
- Observation: `prevDelta += decodedValue` and `prevValue += prevDelta` are signed int64 additions
  driven by stream content with no overflow handling (TimeSeriesStream.cpp:275, 278).
- Observation: doubles are produced by pointer cast, `double* p = (double*)&value`
  (TimeSeriesStream.cpp:318-319), which breaks strict aliasing. Bit-cast is the safe form.

On-disk format
- Block files carry `'D','A','T','A'` magic plus an int32 version (DataBlockPosition.cpp:26-29,
  42-62), parsed with `memcpy` through `fromUnaligned` (DataBlockPosition.cpp:72-85).
- Good forward-compat device: a `static_assert` proves the new magic can never be misread as the
  legacy headerless zlib format (DataBlockPosition.cpp:31-40). Unknown values are kept, not
  coerced: "Defer conversion until retrieval to preserve unknown values"
  (DataBlockPosition.cpp:60). An invalid header falls through to the legacy zlib path
  (DataBlockPosition.cpp:161-173).
- Structural check: exact file length is recomputed from `count` and `activePages`, and any
  mismatch rejects the file (DataBlockPosition.cpp:192-200) before `resize`/`memcpy`.
- Weakness: integers are written in host byte order (DataBlockPosition.cpp:255-258), there is no
  checksum of its own in `lib/` (rg for crc/checksum found none), and `V_0_UNCOMPRESSED` blocks
  (DataBlockPosition.cpp:106-110) get no zlib integrity either.

Durability
- rg of `lib/` finds no fsync, fdatasync, msync or mmap. Only `fflush` in PersistentKeyList.cpp:199.
- Commit marker: data file is fwritten and closed, then an empty "complete" marker file is created
  (DataBlockPosition.cpp:291-317). No fsync of either file or the directory, so the marker can
  reach disk before the data.
- `DataLogWriter::flushBuffer` handles short writes, then discards the buffer anyway
  (`bufferSize_ = 0`, DataLog.cpp:95-125); its bool result is ignored at DataLog.cpp:52 and :88.
- `readLog` treats out-of-range ids as corruption and stops (DataLogUtil.cpp:162-165). The
  `max_allowed_timeseries_id` flag exists to stop a corrupt id driving a huge allocation
  (DataLog.cpp:31-36), which is the right idea: bound attacker-sized allocations.

Tests and CI
- Round trip with fixed seed, named FuzzyTest, is 100000 points in one distribution
  (tests/TimeSeriesStreamTest.cpp:179-204). It is not a fuzzer: no mutated input reaches the
  decoder. Malformed input coverage is one test, an empty string (tests/BitUtilTest.cpp:138-143).
- `DataLogTest.DISABLED_Fuzz` is disabled (tests/DataLogTest.cpp:128). Its comment says 200M points
  is needed to exceed 2^32 bits and "excercise overflow bugs" (tests/DataLogTest.cpp:140-141); the
  writer's `numBits_` and `addValueToBitString` argument are `uint32_t`
  (TimeSeriesStream.h:118, BitUtil.cpp:21).
- CI is one docker `cmake; make; ctest` (`circle.yml`). rg for fuzz/sanitize/asan in the build files
  finds nothing.

## 2. Prometheus tsdb/chunkenc (Go)

Base: https://github.com/prometheus/prometheus/blob/9e28ec50ba751345a1f01a4fdc0d6887f533a37b

Decoding untrusted input
- Layered gate. Outer layer (`tsdb/chunks/chunks.go:689-727`): segment index range check, bounds
  check before reading the length field, uvarint failure check, end-of-segment check, then CRC,
  and only then is a decoder constructed. So the bit decoder sees only CRC-clean bytes in normal
  reads.
- Inner layer: `bstreamReader` returns `io.EOF` when it runs out (`tsdb/chunkenc/bstream.go:176-184,
  202-228`). The iterator is sticky: `it.err` set once ends iteration, `Next` returns `ValNone`, and
  callers fetch it through `Err()` (`tsdb/chunkenc/xor.go:286-288, 305-308`).
- Fast path falls back to slow path, never skips a check: `readBitFast` returns EOF if the buffer
  is empty and the caller retries `readBit` (`bstream.go:186-198`, `xor.go:364-371`). Pattern worth
  copying: a fast path that can only fail over to the checked path.
- Sign and range: `xorRead` does `newTrailing = 64 - newLeading - mbits` with no range check
  (`xor.go:557`). Go defines an oversize shift as 0, so it is memory safe in Go, but the same line
  in C++ is undefined behaviour (compare Beringei above). A C++ port must add the check.
- Observation (asymmetry): head chunk reader has explicit guards, `chkDataLen > uint64(math.MaxInt)`
  and an overflow-safe end check `chkDataLenStart > math.MaxInt-n-chkDataLenInt`
  (`tsdb/chunks/head_chunks.go:779-794`). Block reader computes `int(chkDataLen)` directly
  (`tsdb/chunks/chunks.go:710`). The uvarint comes from a 5-byte slice (`MaxVarintLen32`,
  chunks.go:47, :703), which caps it near 2^35, so on 64-bit this is not an exploitable overflow; it
  is a guard style difference only.
- Concurrency-aware read: the reader copies the last stream byte at construction, because a
  concurrent appender can still be writing it (`bstream.go:159-174`), and the 8-byte fast load
  refuses to touch that last byte (`bstream.go:411-420`).
- Stream header is 2 bytes big-endian sample count (`xor.go:55, 87, 137`). Writer panics at
  `math.MaxUint16` (`xor.go:180-181`), with a test (`xor_test.go:44`).

On-disk format
- Segment: 4-byte magic, 1-byte version, 3 bytes padding, big-endian (`chunks.go:36-41, 463-465`).
  `newReader` checks length, then magic, then
  version, per segment, at open (`chunks.go:622-640`). Same in head chunks with a different magic
  (`head_chunks.go:42, 355-368`).
- Each chunk: uvarint length, encoding byte, data, CRC32 Castagnoli covering encoding plus data
  (`chunks.go:264-285, 718-722`). Head chunks extend the CRC over the series ref and mint/maxt
  (`head_chunks.go:806-808`). Encoding byte selects a codec, unknown value is an error
  (`tsdb/chunkenc/chunk.go:395-411`).
- New codec is a new encoding number (XOR2 next to XOR, `tsdb/chunkenc/xor2.go`), so old data stays
  decodable. CI has a version upgrade test (`.github/workflows/ci.yml:104`).

mmap and durability
- Read mapping: `unix.Mmap(..., PROT_READ, MAP_SHARED)` (`tsdb/fileutil/mmap_unix.go:24-26`). Size
  comes from `Stat` or caller (`tsdb/fileutil/mmap.go:40-46`). Head file is mapped at
  `MaxHeadChunkFileSize` after preallocation (`head_chunks.go:636`; `chunks.go:453-457`).
- No SIGBUS handling: rg for SIGBUS and SetPanicOnFault under `tsdb/` and `cmd/` finds nothing.
  Length is validated at open but a file truncated later would fault. Stated as a gap in the code,
  not a reproduced crash.
- Copy out of the mapping: `bytes.Clone(chkData)` because the mapped file may be closed while the
  chunk is still in use (`head_chunks.go:819-822`). The comment names the failure as a panic. The
  block reader (`chunks.go:723-725`) returns a slice into the map, so it depends on reader lifetime
  tracking (`pendingReaders`, `tsdb/block.go:332`).
- Crash ordering: preallocate, fsync the directory, then write header (`chunks.go:453-470`). The
  doc comment states the contract: content is not fsynced at creation and "Callers must account for
  this" (`chunks.go:431-433`). Tail: flush, `Sync`, truncate zero padding, close
  (`chunks.go:367-391`). Observation: the truncate comes after `Sync` with no second sync, so the
  size change is not itself durable; the padding is zeros, which the reader tolerates.
- Recovery: last head file with a short or zero magic is removed, not treated as fatal; a wrong
  magic is deliberately passed up (`head_chunks.go:448-458`). Gaps in file sequence are errors
  (`head_chunks.go:342-353`).
- Metadata commit: write to `.tmp`, `Sync`, close, rename, fsync parent dir
  (`tsdb/block.go:291-326`, `tsdb/fileutil/fileutil.go:93-109`).
- WAL: 32 KiB pages, records fragmented as full/first/middle/last, each with CRC32C
  (`tsdb/wlog/wlog.go:42, 50, 624-625, 752`). Torn last record is detected on EOF
  (`tsdb/wlog/reader.go:47-57`); checksum failure becomes an error (`reader.go:135-137`); `Repair`
  drops everything after the corruption and explains why: records may have causality
  (`wlog.go:400-408`).
- Fallocate fallback: ENOTSUP or EINTR falls back to truncate (`tsdb/fileutil/preallocate_linux.go:23-35`).

Tests and fuzzing
- `FuzzXORChunk` (`util/fuzzing/fuzz_test.go:129-183`): fuzzed inputs are seed, count and NaN mask,
  which drive generation of samples. It asserts exact equality by `math.Float64bits`, so NaN
  payloads are compared bitwise, plus "iterator ended early", "extra values" and `Err() == nil`.
- Weakness: this is a round-trip harness. The decoder is never handed mutated bytes, so it cannot
  find a decode-side bounds bug. Parser targets elsewhere in the file feed raw bytes.
- `maxInputSize = 10240` with a comment that large input exhausts memory (`fuzz_test.go:31-36`).
- PR CI runs each fuzz group for `-fuzztime=4m` in a matrix (`.github/workflows/fuzzing.yml:13, 26`)
  and uploads crashers on failure (`fuzzing.yml:29-34`). Seeds are generated by
  `make generate-fuzzing-seed-corpus` (`Makefile:244-247`).
- Corruption table test: wrong size, wrong magic, wrong version, truncation to length field,
  truncation of data, and a flipped data byte, each with the exact expected error string
  (`tsdb/block_test.go:196-277`).
- Boundary values list for varbit (`tsdb/chunkenc/varbit_test.go:23-40`) walks each encoding bucket
  edge, including `math.MinInt64`.
- `-race` in CI (`.github/workflows/ci.yml:46-47`).

Benchmarks
- Fixed RNG seed and pre-generated random arrays "so that adding/removing cases does not change the
  generated samples" (`tsdb/chunkenc/benchmark_test.go:49, 57-63`). Matrix of timestamp and value
  patterns (`benchmark_test.go:82-110`). Benchmark first verifies the decode matches expected
  (`benchmark_test.go:299-321`), then measures with a sink (`:323-335`). Reports `ReportAllocs` and
  `B/chunk` (`:284, 329`). Compression ratio and speed are reported from the same run, and a benchmark
  that decodes garbage fails.

## 3. ClickHouse codecs (Gorilla, DoubleDelta) and compressed block layer

Base: https://github.com/ClickHouse/ClickHouse/blob/2a49505f2c31cc1c1b0be339fbf786fc823e6599

This is the strongest of the Gorilla family on untrusted input, and it is the same algorithm as
Beringei, so the difference between them is purely engineering.

Decoding untrusted input
- Validates the exact condition Beringei misses: after reading the 5-bit and 6-bit fields it
  rejects `data_bits == 0` or `leading + data_bits > sizeof(T)*8` before computing trailing zeros
  (`src/Compression/CompressionCodecGorilla.cpp:321-333`). A second guard rejects an all-zero
  `prev_xored_info`, so a stream starting with reuse prefix `0b10` cannot run with no window
  (`:337-342`).
- Count versus output: `static_cast<UInt64>(items_count) * sizeof(T) > dest_size` in 64-bit, so the
  multiply cannot overflow (`CompressionCodecGorilla.cpp:291-292`). DoubleDelta instead checks
  `dest + sizeof(x) > output_end` before every store (`CompressionCodecDoubleDelta.cpp:403-404, 436-437,
  471-472`).
- Header reads are each bounds checked (`CompressionCodecGorilla.cpp:279, 288`, `:448-462`), width
  byte is whitelisted (`CompressionCodecDoubleDelta.cpp:576-577`), trailing bytes handled
  (`bytes_to_skip`).
- Signed overflow avoided by doing delta arithmetic in unsigned types; the comment says why:
  negating the most negative signed number is undefined (`CompressionCodecDoubleDelta.cpp:370, 461-466`).
- Fast loop stops before the tail: DoubleDelta reads whole words only while `source_end - source >=
  16`, then hands the rest to `BitReader` (`CompressionCodecDoubleDelta.cpp:413-414, 443-449`). This is the
  over-read discipline in its plainest form.
- Final size agreement: the generic wrapper rejects if decoded size differs from the header size
  (`src/Compression/ICompressionCodec.cpp:89-97`), so a short decode becomes an error.
- Observation (weakness, safe but silent): `BitReader::fillBitBuffer` returns 0 at end of input;
  the `throw` below it is unreachable because `bytes_to_read == 0` whenever `available == 0`
  (`src/IO/BitHelpers.h:126-135`). `getBitsFromBitBuffer` then yields zeros, with the comment "if it
  has not enough bits, result is undefined" (`BitHelpers.h:104`), and `bits_count -= bits` in `UInt8`
  can wrap (`BitHelpers.h:45, 82`). Memory safety holds because the loops are bounded by item count
  and output end, but a mid-field truncation does not raise from the bit reader. The checksum layer
  above and the size check are what catch it.

Format discipline
- Block: 16-byte CityHash128 over compressed bytes, then method byte, compressed size, decompressed
  size, little endian (`src/Compression/ICompressionCodec.cpp:63-70`,
  `src/Compression/CompressedReadBufferBase.cpp:46-53, 248-249`). Sizes are capped
  (`DBMS_MAX_COMPRESSED_SIZE`, `CompressedReadBufferBase.cpp:165-171`) and zero sizes refused
  (`ICompressionCodec.cpp:102-117`) before allocation.
- Explicit little-endian helpers on all loads and stores
  (`unalignedLoadLittleEndian`, `CompressionCodecGorilla.cpp:215, 224, 282`).
- Method bytes are a registry and retired values stay reserved: "Removed, don't reuse for another
  codec" (`src/Compression/CompressionInfo.h:43-65`).
- Reserved field kept for compatibility: `dest[1] = bytes_to_skip; /// unused (backward
  compatibility)` (`CompressionCodecGorilla.cpp:419`).
- Size bound is arithmetic, not a guess, and rejects 32-bit overflow
  (`CompressionCodecGorilla.cpp:399-413`). The comment at `:171` records a real past bug class: a
  `static` local caching a width across callers.
- Diagnostics: on checksum mismatch it tries every single-bit flip for blocks under 1 MiB and
  reports the byte and bit (`CompressedReadBufferBase.cpp:86-122`). It also bounds that cost.

Tests
- Golden bytes in both directions: `CodecTestCompatibility.Encoding` asserts encoder output equals a
  stored binary string, `.Decoding` asserts that string decodes to the input
  (`src/Compression/tests/gtest_compressionCodec.cpp:558-596`). Comment on the codec: "Compatibility
  tests solidifying encoded binary output" (`CompressionCodecGorilla.cpp:100-102`).
- Boundary sequences walk each encoding width transition, starting 4 before each corner to let the
  double delta settle (`gtest_compressionCodec.cpp:1233-1262`).
- Parameterised generators: same value, sequential, monotonic, random, overflow, for int and float
  (`gtest_compressionCodec.cpp:854-1190`).
- Reproducers kept as tests with byte-level comments, for example heap overflow in T64
  (`gtest_compressionCodec.cpp:1695-1715`).
- SIMD equals scalar: `T64Test.SimdTransposeMatchesScalar` runs 2048 seeded iterations and `memcmp`s
  outputs of `scalar::` and `simd::` for all four kernels (`gtest_compressionCodec.cpp:1740-1787`).

Fuzzing
- One libFuzzer target per codec plus one for the block layer (`src/Compression/fuzzers/`,
  `CMakeLists.txt:1-29`).
- Target design (`src/Compression/fuzzers/gorilla_decompress_fuzzer.cpp:35-72`): first bytes of
  input choose the output size, modulo 65536; a 1 GiB memory limit is set; and it calls
  `doDecompressData` directly. That is the key choice, because going through the block layer would
  fail at the checksum: `compressed_buffer_fuzzer.cpp:39-43` does go through it, so it mostly
  exercises headers (rg for FUZZING_BUILD_MODE in `src/Compression` finds no checksum bypass).
- Weakness: `catch (...)` with no oracle. It proves no crash under sanitizers, not correct output
  and not a round trip.
- Per-target options: `timeout = 20`, `use_value_profile = 1`, `dict = compression.dict`
  (`tests/fuzz/gorilla_decompress_fuzzer.options`).
- CI: 30 minutes on PRs and 5 hours on master (`ci/jobs/libfuzzer_test_check.py:33-34`), corpus
  merge and upload (`:7-14`), weekly minimisation (`ci/workflows/weekly_fuzzers_corpus.py:6-8, 30`).
  Fuzzers build with ASan (`ci/defs/job_configs.py:703`).
- Sanitizers: asan_ubsan, tsan and msan each run the unit tests (`ci/defs/job_configs.py:1338-1353`,
  `ci/defs/defs.py:405-413`).

mmap position
- A strong written position: server mode refuses mmap, because concurrent modification and device
  errors raise SIGBUS and "recovery from this signal is not feasible even with the usage of
  siglongjmp"; arbitrary page-fault latency is also cited (`src/Storages/StorageFile.cpp:578-588`).
  It stays allowed in clickhouse-local. This is the honest counterweight for any mmap design.

## 4. DuckDB (Chimp, Patas, ALP, block storage)

Base: https://github.com/duckdb/duckdb/blob/06ec9d1248940afbf987e0ef51c1abda2cca4b44
`S` = `src/include/duckdb/storage/compression`.

Decoding untrusted input
- The best single structural idea in this survey: `CompressionSegmentReader`, "A non-owning,
  bounds-checked view over [0, size) ... It validates physical byte ranges, not row or codec
  invariants" (`S/compression_segment_reader.hpp:22-24`). All reads, forward and backward, go through
  checks (`:50-65, 76-88`), it hands out sub-readers (`GetSubReader`, `S/alp/alp_scan.hpp:105`), and it
  checks alignment by integer arithmetic to avoid forming an out-of-buffer pointer (`:157-164`).
- Failure is a typed `DataCorruptionException` carrying the context string, in out-of-line
  `[[noreturn]]` functions, with `DUCKDB_UNLIKELY` on the check, so the hot path stays small
  (`src/storage/compression/compression_segment_reader.cpp:27-61`, `S/compression_segment_reader.hpp:104`).
- Group-at-a-time gate in front of an unchecked fast reader. Comment: "Validate the full group data
  range before using the unchecked bit reader" (`S/chimp/chimp_scan.hpp:278-285`).
  `ValidateAndCalculateDataBitCount` replays flags, sums the exact bit count, and rejects bad field
  combinations before any value is read (`:106-152`), including the Gorilla-family shift bug:
  `significant_bits > BIT_SIZE - leading_zero` throws (`:126-131`), and reuse of an uninitialised
  leading-zero state throws (`:136-140`). The inner `BitReader` then has no per-read checks
  (`S/chimp/algorithm/bit_reader.hpp:62-76`). This keeps the per-bit path branch-free while still
  refusing corrupt input: bounds check per group, not per bit.
- Cross-field consistency: stored leading-zero count must equal the count derived from flags
  (`S/chimp/chimp_scan.hpp:264-268`).
- Same for ALP: exponent, factor, exception count and bit width each range checked
  (`S/alp/alp_scan.hpp:215-231`), offsets checked pairwise (`:147-162`), every exception position
  checked against vector size (`:245-250`). Uncompressed escape via a sentinel exponent (`:204-213`).
- Count arithmetic done by division to avoid overflow:
  `vector_count > (metadata_end - HEADER_SIZE) / METADATA_POINTER_SIZE` (`S/alp/alp_scan.hpp:108`),
  `count > position / sizeof(T)` (`S/compression_segment_reader.hpp:104`).
- `static_assert` on trivially copyable reads (`S/compression_segment_reader.hpp:51`).
- Debug-only layer: `D_ASSERT` is used for invariants derived from already validated state
  (`S/chimp/chimp_scan.hpp:110, 124, 144`). The checks on stored fields read above are exceptions, not
  asserts. `array_ptr` carries a `SAFE` template parameter (`src/include/duckdb/common/array_ptr.hpp:50`).

Format discipline
- File: checksummed main header with 'DUCK' magic and a version range check that gives a useful
  message (`src/storage/single_file_block_manager.cpp:29, 199-254`), `VERSION_NUMBER_LOWER/UPPER`
  (`src/include/duckdb/storage/storage_info.hpp:230-232`).
- Two database headers with iteration counter; the higher wins (`single_file_block_manager.cpp:710-728`),
  so commit is an atomic flip.
- Per-block 64-bit checksum checked on read, throwing `DataCorruptionException`
  (`single_file_block_manager.cpp:733-753`). The hash is a fast multiplicative one
  (`src/common/checksum.cpp:7-9, 14-32`), not cryptographic.
- ALP has a storage version test writing under old versions
  (`test/sql/storage/compression/alp/alp_storage_version.test:1-23`).

Durability
- Ordering comment and code: "We need to fsync BEFORE we write the header to ensure that all the
  previous blocks are written as well" (`single_file_block_manager.cpp:1399-1400`).
- fsyncgate: EIO from fsync is fatal, since "`fsync` only reports EIO for once, which makes it
  unretriable and data loss unrecoverable" (`src/common/local_file_system.cpp:845-849`).
- macOS: F_FULLFSYNC with fallback, comment explains that plain fsync is not durable past power
  failure (`local_file_system.cpp:827-833`). fdatasync on Linux path (`:836`).
- Crash injection hook in the commit path: `DEBUG_ABORT_AFTER_FREE_LIST_WRITE`
  (`single_file_block_manager.cpp:1394-1397`). Torn WAL test truncates at computed commit boundaries
  (`test/sql/storage/wal_torn_write.cpp:29-62, 87-89`).

Tests
- Checked-in corrupt database fixtures with exact expected error text. Example:
  `alp_corrupt_exception_position.test:19-22` expects "Corrupted ALP segment: exception position 60000 is
  outside vector size 1024". There are 11 `alp_corrupt_*` tests, 3 for Chimp, 4 for Patas, and 99 fixture
  files under `data/storage/corrupt/`. Each one is a single hand-made corruption of a validated field.
  This is the model for a per-field negative test.
- OSS-Fuzz: harnesses for SQL parse, CSV/JSON and Parquet (`test/ossfuzz/`), regression inputs kept as
  `test/ossfuzz/cases/clusterfuzz-test-*`. The Parquet harness comment is candid: before it was added,
  parse fuzzing "NEVER reaches the Parquet stack" and coverage there was ~0%
  (`test/ossfuzz/parquet_fuzz_test.cpp:5-9`). Lesson: measure what the harness reaches.
- PR fuzzing: CIFuzz matrix of address, undefined and memory, `fuzz-seconds: 3600`
  (`.github/workflows/cifuzz.yml:31, 45`).
- Weakness: no harness targets the Chimp, Patas or ALP codecs directly; they are covered by hand-made
  corrupt files. Whole-database fuzzing would have to pass block checksums first.

SIMD structure
- ALP and bit-packing here are scalar-code autovectorised through vendored `fastpforlib`
  (`src/include/duckdb/common/bitpacking.hpp:29`), in groups of 32. No intrinsics path for these
  codecs. Decoding is a fixed-size block with a stack buffer (`S/alp/algorithm/alp.hpp:401-421`).

## 5. cwida/ALP (research reference implementation)

Base: https://github.com/cwida/ALP/blob/31ca0ed11c93c99d3f5b5c30e01a3e1c3832d3ce

- Scope: kernels, no format. Fixed `VECTOR_SIZE = 1024` (`include/alp/config.hpp:11`). No magic,
  version or length anywhere in `include/alp/`. The kernel API takes raw pointers with implied sizes
  (`include/alp/decoder.hpp:134, 141-144`, `include/alp/rd.hpp:152-158`).
- No validation: `FACT_ARR[factor]` and `FRAC_ARR[exponent]` index with stored bytes
  (`decoder.hpp:129`), `out[exceptions_positions[i]]` writes at a stored position
  (`decoder.hpp:147`), and `left_parts_dict[left_parts[i]]` likewise (`rd.hpp:166`). Compare DuckDB's
  range checks in front of the same algorithm (section 4). The validation belongs in a loader, not
  in the kernel, and here the loader does not exist.
- SIMD: generated per-ISA functions in namespaces (`include/alp/falp.hpp:7-8, 66-67, 76, 85, 94, 104, 113-114,
  123`, `src/falp.cpp` is 42755 lines), selected at build time with `-mavx512dq` or `-march=native`
  (`src/CMakeLists.txt:19-26`). No runtime dispatch.
- Weakness: the only test decodes through `generated::falp::fallback::scalar::falp`
  (`test/test_alp_sample.cpp:169`), so the ISA variants are not checked for identical output by the
  test suite. NaN is compared by `isnan`, not bit pattern (`test_alp_sample.cpp:19-20`). rg finds no
  fuzz or sanitizer use outside vendored publication code.
- Weakness: CI declares `BUILD_TYPE: [Debug, Release]` in the matrix but the cmake lines never use it
  (`.github/workflows/CI.yaml:20, 32, 58`), so those matrix cells build the same configuration.
- Good: asserts compression properties, not only round trip: `ASSERT_EQ(column.exceptions_count, ...)`
  and `ASSERT_EQ(column.bit_width, ...)` per dataset (`test_alp_sample.cpp:178-179`). A silent
  ratio regression fails the test.
- Benchmarks: cycles per value through `benchmark::cycleclock` for compression and decompression
  separately (`benchmarks/benchmark.cpp:34-35, 86-104`), written to CSV. Reproduction guide names
  datasets and microarchitectures (`BENCHMARKING.md`, steps 1-4).

## 6. streamvbyte

Base: https://github.com/lemire/streamvbyte/blob/edc6105b24e150cd109ece75e6e37f54291635b0

- Over-read is a published contract, not an accident: `STREAMVBYTE_PADDING 16`
  (`include/streamvbyte.h:12`), comment "may read (but not use) STREAMVBYTE_PADDING extra bytes"
  (`:44-46, 58-59`, `README.md:82-83`), and `streamvbyte_max_compressedbytes` includes it (`:32, 38`).
  The cause is visible: a 16-byte `_mm_loadu_si128` advanced by a key-derived `len`
  (`src/streamvbyte_x64_decode.c:13, 21-24`).
- API shape: `streamvbyte_decode(in, out, length)` takes no input size at all
  (`include/streamvbyte.h:64`). Decode is unsafe alone. The safety step is separate and O(n):
  `streamvbyte_validate_stream(in, inLength, outLength)` (`:70-75`, `README.md:85-92`). It is a good API
  in that it needs no decode, and the wide accumulator shows care: "Accumulate the key sizes in a
  wider type to avoid overflow" (`src/streamvbyte_decode.c:103-105`), "don't add to outCount in case it
  overflows" (`:94-97`).
- Observation (real weakness, confirmed by trace, not run): when `outCount` is a multiple of 4,
  `streamvbyte_validate_stream` still executes `uint32_t key = *keyPtr++` after the key loop
  (`src/streamvbyte_decode.c:119-123`), though `outCount` is now 0. With `outCount = 4, inCount = 1`,
  `keyLen = 1` passes the check at `:100`, the loop consumes `in[0]`, and line 123 reads `in[1]`, one
  byte past the buffer. The over-read needs an input with keys and no data, which is what a
  validator exists to catch. The ARM path falls through to the same line
  (`src/streamvbyte_arm_decode.c:55-61`). Unit tests only pass valid streams to the validator
  (`tests/unit.c:107, 130, 215, 233`).
- Dispatch: x64 runtime `streamvbyte_sse41()` around the SIMD body, scalar tail for the remainder
  (`src/streamvbyte_decode.c:71-85`). Detection result is cached, in an atomic where available
  (`src/streamvbyte_isadetection.h:200-241`). `#if defined(__SSE4_1__)` short-circuits to true
  (`:245-253`). Functions are compiled with per-function target pragmas (`:284-292`,
  `streamvbyte_x64_decode.c:9, 27`). ARM64 has no dispatch (`streamvbyte_decode.c:78-83`).
- Weakness: no way to force the scalar path. rg for scalar or force in `tests/unit.c` finds only one
  `streamvbyte_sse41()` guard (`:2825`), so a machine with SSE4.1 tests the SIMD body and the scalar tail.
- Fixed fixtures: a compressed byte array is decoded and re-encoded, with the 16-byte padding allocated
  by hand (`tests/unit.c:2750-2772`).
- CI: UBSan builds with `-DSTREAMVBYTE_WERROR=ON` on Ubuntu and macOS
  (`.github/workflows/ubuntu24.yml:44-48`, `macos.yml:40-44`). ASan is an option, off by default
  (`CMakeLists.txt:65, 84-89`). No fuzz target (rg finds none).
- Benchmark is the most complete here: per distribution (log-uniform, full range, small), reports
  ns/op, GB/s, uints/s, iterations, plus instructions, cycles, IPC, instructions per int, cycles per
  int, branches, branch misses, cache misses (`tests/perf.c:35-61, 63-80`). It re-decodes after timing
  and compares compressed size and every value, printing the first mismatch
  (`tests/perf.c:107-115`). Observation: a mismatch only prints; the visible lines do not fail the run.

## 7. simdcomp

Base: https://github.com/lemire/simdcomp/blob/d5301778fe5045ca8099252de5a6ed8016a287df

- Scope statement: "By design it does not define a compressed format. It is up to the (sophisticated)
  user to create a compressed format." (`README.md:26-27`). No magic, version or checksum follows from
  that.
- API: `void simdunpack(const __m128i *in, uint32_t *out, const uint32_t bit)`, fixed 128 values, no
  sizes, no status (`include/simdbitpacking.h:33-34`). `simdunpack_length` returns the advanced input
  pointer (`:49-55`). The caller has to know `bit` and `length`.
- Bad width is silent: the `switch (bit)` in `simdunpack` ends in `default: break;`
  (`src/simdbitpacking.c:13773, 13907-13909`), so `bit > 32` returns with `out` unwritten and no error.
- Over-read handled by layout: `simdpack_compressedbytes` rounds up to whole `sizeof(__m128i)`
  (`src/simdbitpacking.c:14370-14377`), so a 16-byte load (`:14262`) stays inside a buffer of that
  size. The format's size unit equals the load unit.
- Dispatch: compile-time only. AVX2 and AVX-512 functions exist under `#ifdef __AVX2__` and
  `#ifdef __AVX512F__` (`include/avxbitpacking.h:8`, `include/avx512bitpacking.h:8`), and rg finds no
  cpuid use in `src/` or `include/`. The README states the baseline ISA (`README.md:40-41`). No
  scalar fallback in the library; ARM goes through a NEON shim of the SSE intrinsics (`README.md:41`,
  `include/neon128.h`).
- Tests: plain C `main` of round trips. `issue21` walks 30 bit widths and every size to 4095 with an
  exact-size `malloc` and checks byte usage equals `simdpack_compressedbytes` ("bad mem usage")
  (`tests/unit.c:9-44`). This is a useful shape: exact allocation means ASan would see an over-read.
  AVX tests compare against round trip and check `avxmaxbits` equals scalar `maxbits_length`
  (`tests/unit.c:438-444`), and are compiled out without the flag (`:996-1008`).
- Weakness: no malformed-input test, no fuzz or sanitizer config (rg of `CMakeLists.txt`, `.travis.yml`,
  `.appveyor.yml`, `cmake/` finds none), no `.github` directory. CI is `cmake`, build, `ctest`
  (`.travis.yml:11`).
- Benchmark timing is `clock()` on POSIX (`benchmarks/benchmark.c:28-30`).

## 8. FastPFor

Base: https://github.com/lemire/FastPFor/blob/ae4e62db4de1ce6904e278f94afb56a87a36b32c

- API: `decodeArray(in, length, out, nvalue)` with `nvalue` in-out (`headers/codecs.h:61-62`). The doc
  defines overrun as a condition to notice afterwards: "if nvalue exceeds the original value, there
  might be a buffer overrun" (`codecs.h:26-31, 51-55`). A length of 0 means unknown input length
  (`codecs.h:57-59`). Errors are C++ exceptions.
- Good: output capacity is checked up front from a stored count, `if (mynvalue > nvalue) throw
  NotEnoughStorage(mynvalue)` (`headers/fastpfor.h:78-80`).
- Anti-pattern: the input bound is checked after the work: `if (initin + length < in) throw
  std::logic_error("Decode run over output buffer. Potential buffer overflow!")` (`fastpfor.h:93-96`).
  By then the out-of-bounds reads have happened.
- Observation: `__decodeArray` follows stored offsets with no checks: `wheremeta`, `bytesize`, `tnvalue`
  (`fastpfor.h:230-243`), and indexes `unpackpointers[maxbits - b]` from two stored bytes
  (`:257, 264-265`), an array of `sizeof(IntType)*8 + 1` (`:247-248`). `maxbits < b` gives a negative
  index. `*(reinterpret_cast<const IntType *>(inexcept))` is a typed load from a computed offset, which
  for 64-bit values is not guaranteed aligned (`:235`). The only end check is `assert`, compiled out in
  release (`:273`). This reads as a codec for trusted data, and nothing in the docs read here says
  otherwise. Its value here is as a contrast.
- Tests: gtest, parameterised over codecs, with width-boundary cases 1 to 64
  (`unittest/test_fastpfor.cpp:152-323`). No malformed-input tests seen.
- CI: an ASan job with `-fno-sanitize-recover=all` (`.github/workflows/ubuntu-ci.yml:33-47`,
  `CMakeLists.txt:75-81`), in `FASTPFOR_SIMD_MODE=portable`. No fuzz target (rg finds none).
- Benchmark timer: CPUID then RDTSC at start and RDTSCP then CPUID at stop, citing the Paoloni Intel
  note, so the timed region cannot reorder across the serialising instructions
  (`headers/cpubenchmark.h:15-30, 33-40`).


## 9. Apache Arrow C++ (bpacking, Parquet reader, MemoryMappedFile)

Base: https://github.com/apache/arrow/blob/5585e501e2593b1c12ca688a7fdb160df0f07161 . Paths are under `cpp/src/` unless they start with `cpp/`, `docs/`, `.github/` or `compose.yaml`.

### Decoding untrusted input

- **Kernel stops short of the end, exact code takes the tail.** `unpack_width` runs a prolog (`unpack_exact` with `kIsProlog=true`) until byte aligned, then the kernel loop with `in_last = in_end - kBytesRead; while (batch_size >= kValuesUnpacked && in <= in_last)`, then an epilog `unpack_exact` (`arrow/util/bpacking_dispatch_internal.h:192-252`; comment at :226-227 "This constant prevent reading past buffer end"). This is the cleanest over-read structure read in this task: scalar and SIMD share one driver, and only the `Unpacker` struct differs (:183-185, :223).
- **Exact tail read.** `unpack_exact` reads a full buffer only if `in + start_byte + kBufferSize < in_end`, otherwise an exact-spread `memcpy` (:131-153). The large-width corner, where a misaligned 63-bit value spreads over 9 bytes, has its own case (:111-114, :160-169). `PackedMaxSpreadBytes` is `constexpr` (:63-85). Little-endian conversion is explicit, `FromLittleEndian` (:155).
- **Caller clamp.** `BitReader::GetBatch` clamps to remaining bits in `int64` arithmetic, passes `.max_read_bytes = max_bytes_ - byte_offset_`, and returns the count actually read (`arrow/util/bit_stream_utils_internal.h:264-295`). Callers compare that count (see the delta decoder below).
- **Delta decoder header validation.** `DeltaBitPackDecoder::InitHeader` rejects zero sizes, values-per-block not a multiple of 128 and values-per-miniblock not a multiple of 32 (`parquet/decoder.cc:1586-1612`). `InitMiniBlock` rejects `bit_width > kMaxDeltaBitWidth` (:1648-1651). Delta accumulation runs in unsigned type `UT` with the comment "Overflow is as expected" (:1704-1707, :1714-1717), so corrupt deltas wrap, not UB. `GetBatch`'s return is compared and raises an EOF exception (:1709-1712). Padding skip is checked (:1727-1732), total is clamped (:1657).
- **Page reader.** Header deserialisation retries with doubling buffer up to `max_page_header_size_`, 16 MiB (`parquet/column_reader.cc:396-424`, `parquet/column_reader.h:41,44`). Negative sizes rejected (:430-432), short reads rejected (:446-452), enums loaded with `LoadEnumSafe` (:454), negative value/row/level counts rejected (:200, :358, :362).
- **File open.** Zero-size and too-small files rejected (`parquet/file_reader.cc:485-489`). `metadata_len > source_size_ - kFooterSize` rejected, with both numbers in the message (:507-515). Failures raise the typed `ParquetInvalidOrCorruptedFileException`.

### On-disk format discipline

- `PAR1` magic at start and end, `PARE` for encrypted (`parquet/file_writer.h:34-35`, `parquet/file_reader.cc:499-505`). Footer is last, so a partial write has no trailing magic and fails the check at open. Footer length is little-endian (:507-515).
- Page CRC is verified before decrypt and decompress when enabled (`parquet/column_reader.cc:456-467`).
- **Weakness (observation).** `page_checksum_verification_ = false` is the default (`parquet/properties.h:166`), so integrity is opt-in as well as dependent on the writer having stored a CRC. Corrupt page bytes reach the decoders in the default configuration.

### API shape

`struct UnpackOptions {int batch_size; int bit_width; int bit_offset=0; int max_read_bytes=-1;}` and `template<Uint> void unpack(const uint8_t* in, Uint* out, const UnpackOptions&)` (`arrow/util/bpacking_internal.h:26-34`). `-1` means "no over-read at all" and any positive value grants a bounded read window. `BitWriter::PutValue` returns `false` when out of space (`arrow/util/bit_stream_utils_internal.h:204-208`). `SafeLoadAs` does unaligned loads through `memcpy` (`arrow/util/ubsan.h:57-61`).

### SIMD structure

- Runtime dispatch by `UnpackDynamicFunction::targets()`, a constexpr array of scalar, NEON, SVE256, SSE4_2, AVX2, AVX512 (`arrow/util/bpacking.cc:29-45`), cached in a function-local `static const DynamicDispatch<...>` (:51). The comment at :37-38 (GH-50503) records why there is no SVE128 entry: code size without gain, and possible ODR violations.
- `DispatchLevel` sorted by preference (`arrow/util/dispatch_internal.h:30-42`), targets concept-checked (:66-69), statically-resolved case detected at compile time (:74-109).
- Runtime override: `ARROW_USER_SIMD_LEVEL` caps SIMD at NONE, SSE4_2, AVX, AVX2 or AVX512, x86 only (`docs/source/cpp/env_vars.rst:151-170`, `arrow/util/cpu_info.cc:385-393`). CMake exposes `ARROW_SIMD_LEVEL` and `ARROW_RUNTIME_SIMD_LEVEL` (`cpp/cmake_modules/DefineOptions.cmake:162,176`). CI carries a simd-level matrix entry (`.github/workflows/cpp.yml:91`).

### Testing

- `TestAll` (`arrow/util/bpacking_test.cc:193-255`) walks `num_values` {64, 128, 2048} × every bit width 0..max × every bit offset 0..7 × every epilog size 0..max. Inputs are known patterns (zeros, ones, alternating, :134-191, :219-221) plus a random round trip through `BitWriter` (:112-132, :224). On mismatch the index is reported (:120-131).
- `.max_read_bytes = -1,  // No over-reading in testing (strict ASAN)` (:215, :240) with vectors sized exactly by `GetNumBytes` (:135-137). Any over-read is therefore an ASan report.
- The same `TestAll` runs per kernel and through the dispatcher: `TYPED_TEST` for UnpackScalar, Sse4_2, Avx2, Avx512, Neon, Sve256 and Unpack (:273-317), with `GTEST_SKIP` if the CPU lacks the feature (:279-280).
- Observation: identical output is proved against shared expected values, not by a direct kernel-vs-kernel diff. A `GTEST_SKIP` also means a CI host without AVX-512 gives a green run with that kernel unexecuted.
- Corrupt-data test: `data_page_v1` with a corrupt checksum (`parquet/reader_test.cc:107-108, 884, 908-927`).

### Fuzzing

- **Encoding fuzz target.** `parquet/arrow/encoding_fuzz.cc:22-26` calls `FuzzEncoding`. Input starts with a packed 64-byte header (`source_encoding_id`, `roundtrip_encoding_id`, `type_id`, `type_length`, `num_values`, reserved bytes, `static_assert` on size) (`parquet/arrow/fuzz_encoding_internal.cc:70-90`). Invalid ids are rejected at parse (:138-147).
- **Oracle.** Decode with the source encoding as reference, re-encode with the round-trip encoding, decode again, compare, with varied chunk sizes and `ValidateFull` (:293-332). "OOM when attempting to roundtrip is not a hard failure, any other error is." `ARROW_CHECK_OK` aborts (:333-338). This is the strongest fuzz oracle read in this task: it finds silent mis-decodes, not only crashes.
- **File fuzz target.** If a read returns OK, `ARROW_CHECK_OK(table->Validate())` aborts, `ValidateFull` is allowed to fail (`parquet/arrow/fuzz_internal.cc:102-116`). Batch sizes nullopt, 13, 300 (:255). Fuzz reader properties do not enable the page CRC (:178-189), so input reaches the decoders.
- **Harness hygiene.** Capped memory pool, `kFuzzingMemoryLimit` 2200*1000*1000 (`arrow/util/fuzz_internal.h:50`, `arrow/util/fuzz_internal.cc:31-35`). `FuzzStatus` with `SkipFuzzInput` returns -1 to keep an input out of the corpus (:51-67).
- **Regressions.** `TEST(TestArrowReadWrite, FuzzReader)` replays through the production harness (`parquet/arrow/arrow_reader_writer_test.cc:6138-6159`). `compose.yaml:685` sets `ARROW_FUZZING: "ON"  # Check fuzz regressions` in the sanitizer job.
- **Corpus.** `cpp/build-support/fuzzing/generate_corpuses.sh:20-79` builds `<FUZZ TARGET>-seed_corpus.zip` (:36-38) from generator binaries, golden files, parquet-testing data and `bad_data` (:69-73). Targets register through `ADD_FUZZ_TARGET` (`cpp/cmake_modules/BuildUtils.cmake:901`).
- **Sanitizers.** `ubuntu-cpp-sanitizer`, ASan plus UBSan (`compose.yaml:648-691`, `.github/workflows/cpp.yml:94-100`), and `ubuntu-cpp-thread-sanitizer` (`compose.yaml:697`).

### Mmap

- `Region` owns the mapping; slices keep it alive through buffer ownership; `munmap` result checked (`arrow/io/file.cc:399-420, 434-436`).
- `Resize` refuses read-only maps, partial maps and active readers: "Cannot resize memory map while there are active readers" (:484-495).
- `InitMMap` truncates then maps, rejects a length beyond the file, checks `size_t` fit, handles `MAP_FAILED` (:572-600). `ValidateRange`/`ValidateReadRange` guard reads and the `madvise(WILLNEED)` path (:139, :737-742). Writes hold a lock (:749). `MemoryMapRemap` is `ftruncate` plus `mremap` (`arrow/util/io_util.cc:1435-1484`).
- Gap (observation): `rg` finds no `msync`, `fsync`, `fdatasync` or `SIGBUS` in `arrow/io/file.cc`, `arrow/io/file.h` or `arrow/util/io_util.cc`. There is no durability primitive at this layer and no handling of a file truncated after mapping. Not reproduced, since nothing was run.

### Benchmarks

`BM_Unpack` (`arrow/util/bpacking_benchmark.cc`) uses seeded random bytes (:48-54), controls alignment by skipping to an aligned or unaligned byte (:82-88), calls `benchmark::ClobberMemory()` (:99-102), reports `SetItemsProcessed` (:103), labels each kernel aligned and unaligned, and uses `SkipWithMessage` on unsupported CPUs (:183-200). Comment on size bias at :106-108.

Observation: the benchmark passes `max_read_bytes=-1` (:96), while the driver's own comment says production is "always called with max_read_bytes set" (`arrow/util/bpacking_dispatch_internal.h:134-136`). It therefore measures a different tail path from production.

## 10. Highway (portable SIMD, dynamic dispatch, test harness)

Base: https://github.com/google/highway/blob/62868412cf235b2064000a339010e200e93ac754 .

Highway is the reference here for how SIMD code is structured and tested, not for format handling. It has no on-disk format and no decoder of untrusted bytes in scope.

### SIMD structure

- Dispatch: one source file is re-included per target (`hwy/foreach_target.h:19`), then `HWY_DYNAMIC_DISPATCH` chooses at runtime, `HWY_STATIC_DISPATCH` at compile time (`g3doc/quick_reference.md:32-33, 51-55, 77-83`).
- `DisableTargets` (`hwy/targets.h:52-60`), `SetSupportedTargetsForTest` (:62-66), `SupportedAndGeneratedTargets` (:73-81). `TargetName` covers `EMU128` and `SCALAR` (:88-93), so the fallback is itself a named target and not a side path.
- **Tail handling is spelled out in the API.** `LoadN` loads `HWY_MIN(Lanes(d), max_lanes_to_load)` and "does not fault if all of the elements in `[p, p + max_lanes_to_load)` are accessible, even if `HWY_MEM_OPS_MIGHT_FAULT` is 1" (`g3doc/quick_reference.md:1900-1907`). `LoadNOr` does the same with a fill value (:1909-1912). The FAQ warns the opposite for the obvious approach: "Using `LoadU`, or even `MaskedLoad` with the mask set to `FirstN(d, remaining_lanes)`, may trigger page faults or asan errors" (`g3doc/faq.md:147-155`).
- Contrib bit-packing (`hwy/contrib/bit_pack/bit_pack-inl.h:37-45`): compile-time `kBits`, block-sized `Pack`/`Unpack` of fixed vector counts, no length argument, so any bounds are the caller's job.

### Testing

- **Target-parametrised tests that cannot silently stop testing.** `TestWithParamTarget::SetUp` calls `SetSupportedTargetsForTest(GetParam())`, and `TearDown` fails a test that never used dynamic dispatch: "doesn't use dynamic-dispatch and doesn't need to be parametric" (`hwy/tests/hwy_gtest.h:96-113`; same for the extra-parameter variant :143-153). The list comes from `SupportedAndGeneratedTargets` (:74-80) and `HWY_TARGET_INSTANTIATE_TEST_SUITE_P` (:115-118). Test names carry the target name (:164).
- Round trip: `TestPack` packs random values then unpacks through `Checker` (`hwy/contrib/bit_pack/bit_pack_test.cc:87-175`), run per target via `HWY_EXPORT_AND_TEST_P` (:244-247), with the RNG seeded from lane count (:93). It covers one bit width per template instance (:178-184).
- The test defeats compiler elision: `T zero = static_cast<T>(Unpredictable1() - 1); val = static_cast<T>(val + zero);` (:163-165).
- CI: compilers matrix with `-DHWY_WARNINGS_ARE_ERRORS=ON` (`.github/workflows/build_test.yml:89`), a `-m32 -ftrapv` job (:65-71), per-compiler `HWY_DISABLED_TARGETS` (:188).

### Weaknesses (observations)

- `BUILD:133-136` warns that a target flag change can be "only caught by sanitizers which do not run in CI". `rg` of `build_test.yml`, `run_tests.sh`, `CMakeLists.txt`, `README.md` finds no sanitizer job. This describes this repo's public CI only; the comment does not speak for any internal infrastructure.
- `rg -il fuzz` in `hwy/`, `CMakeLists.txt`, `.github/` matched only a binary test-data file. No fuzz harness found, and no OSS-Fuzz project (see section 13).

### Benchmarks

`hwy/nanobenchmark.h:19-46` is the clearest statement read of what a micro-benchmark gets wrong. Repeating with the same input trains the branch predictor, so "the benchmark is measuring something very different - a best-case result, almost as if the parameter were made a compile-time constant" (:32-35). Its answer: fences around the timer, `Measure(Repeat(VaryInputs(func)))` in place of `VaryInputs(Measure(Repeat(func)))` (:43-44), and the half sample mode as estimator (:44-46). `Params` sets `target_rel_mad = 0.002` and `max_evals = 9` (:97, :101). `Result` reports `ticks` and `variability` (:108-116). `Unpredictable1()` stops constant folding (:57-59). The bit-pack test's optional benchmark prints throughput with the variability figure, `GB/s:%4.1f (+/-%3.1f%%)` (`hwy/contrib/bit_pack/bit_pack_test.cc:154-156`).

## 11. ArcticDB (codec, segment parser, mapped file storage)

Base: https://github.com/man-group/ArcticDB/blob/96bc048942f6041909db3d39dacd1c55cbad1ff7 . Paths are under `cpp/arcticdb/` unless they start with `cpp/` or `.github/`.

### Decoding untrusted input

- **Decoders get a block's sizes but no end of input.** `decode_block` takes `const uint8_t* input` with sizes from the block metadata (`codec/codec-inl.hpp:25-29`), and `decode_ndarray` advances `data_in` by `block_info.out_bytes()` (:128-135). Nothing here bounds `data_in` against the segment end.
- **Whole-field checks after the fact.** `data_in - input == read_bytes` (:148-153) and `data_out - data_begin == data_size` (:155-160). These are good checks for logic errors, but they run after all blocks are decoded.
- **Block decoders are strict on size.** LZ4 uses `LZ4_decompress_safe` and rejects negative and mismatched results with typed `E_DECODE_ERROR` (`codec/lz4.hpp:74-90`). Zstd checks `ZSTD_getFrameContentSize` against the expected size, then the real size (`codec/zstd.hpp:55-68`). Passthrough checks `in_bytes == out_bytes`, then `memcpy` (`codec/passthrough.hpp:152-157`).
- **Ordering weakness (observation).** `Segment::from_bytes` reads `fixed_hdr` from `src` and calls `decode_header_and_fields` before `check_size` against `readable_size` (`codec/segment.cpp:234-241`). So the header is parsed before the buffer has been shown to contain it. On the V2 path `deserialize_from_bytes` does `memcpy` of `fields_bytes_` and `offset_bytes_` taken from the bytes themselves, with no bound in scope (`codec/segment_header.cpp:91-107`). Reachable with a short or corrupt segment by reading the code; not run.
- `decode_protobuf_header` ignores the return of `ParseFromZeroCopyStream` (`codec/segment.cpp:110`), and casts `size_t` to `int` (:106).
- Small: the magic mismatch message passes its arguments swapped, with `fixed_hdr->magic_number` in the "expected" slot (`codec/segment.cpp:161-166`). Argument order matters in a corruption diagnostic.

### On-disk format discipline

- `FixedHeader` is 2-byte `magic_number` 0xFA57, 2-byte `encoding_version`, 4-byte `header_bytes` (`storage/memory_layout.hpp:138-150`). The comment says the header is "invariant between all encodings" and the version permits everything after it to change (:140-143). This is the right structure.
- **Magic words as section delimiters.** `Desc`, `Encd`, `Strp`, `Meta`, `Indx`, `Clmn`, `Fram`, `SDsc` (`codec/magic_words.hpp:14-21`), checked inline on decode (`codec/codec-inl.hpp:140, 143, 173`; `codec/segment.cpp:116, 127, 137`). This is a cheap framing check that turns a mis-parsed offset into an error near the fault.
- `MagicNum` destructor inverts the stored magic (`util/magic_num.hpp:22`), so a use-after-destruction also fails `check()` (:25-35). The field is `volatile` to keep the store (:38).
- Weakness (observation): `check_magic_in_place` does `reinterpret_cast` on a raw position with no bound (`util/magic_num.hpp:58-62`), and the struct is `uint64_t`, so a magic read requires 8 readable bytes at an unaligned position. The structs are read and written as host-endian in-memory layouts (`FixedHeader`, `codec/segment.cpp:152`; `HeaderData`, `codec/segment_header.cpp:92`), with no byte-order conversion on the paths read.
- **Stored hash is not a checksum on read (observation).** Each block stores a hash (`codec/core.hpp:29-32`, `codec/encoded_field.hpp:115-117`). `rg` for `.hash()` outside tests in `cpp/arcticdb` finds only protobuf copies (`codec/protobuf_mappings.cpp:21, 45`) and `hash_field` (`codec/codec.cpp:567-578`), which feeds `get_segment_hash` (:580). Its caller builds the content key at write time (`async/tasks.hpp:79-81`). No decode-path verification was found, so bit rot in a block is not detected by this hash. The stored hash is also of the input (`codec/lz4.hpp:55`), so it could only be checked after decompression.

### API shape

Typed error-code checks at the decoder (`codec::check<ErrorCode::E_DECODE_ERROR>`). `Segment::from_bytes(src, readable_size, copy_data)` has a zero-copy path with `BufferView` (`codec/segment.cpp:251-258`). Ownership by keepalive is tested (`codec/test/test_codec.cpp:387-402`).

### SIMD structure

Out of scope for the read path: the block codecs delegate to LZ4 and Zstd. No in-house SIMD integer decoder was found in `codec/`.

### Testing

- Round-trip unit tests per encoding version: `TYPED_TEST` and `RoundtripTimeseriesDescriptorV1/V2`, `RoundtripStatisticsV1/V2` (`codec/test/test_codec.cpp:80-790`). Hash stability and hash sensitivity to column names (:588-790).
- Sanitizers in CI: ASan+LSan+UBSan job `sanitizer-address-leak-undefined` (`.github/workflows/analysis_workflow.yml:273-284`) with CMake options `ARCTICDB_USING_{ADDRESS,THREAD,UB}_SANITIZER` (`cpp/CMakeLists.txt:28-30`). The TSan job is commented out (`.github/workflows/analysis_workflow.yml:287-295`).
- Not found: no fuzz harness (`rg -il fuzz` in `cpp/arcticdb` sources and the workflows named above). `rg -il 'corrupt|truncat|malform'` in `cpp/arcticdb/codec` matches only `codec/segment.cpp`, at a comment (:388), so the codec tests have no corrupt-input case.

### Mmap and durability

- `MemoryMappedFile` (`util/memory_mapped_file.hpp`) has a 16-byte header, magic `Arct` plus `uint64` length (:169, :193-196). `open_file` requires `total_size >= header_size` (:185), then checks magic.
- **Weakness (observation).** The stored length is not checked against the real file size at open (:193-196). `do_read_raw` bounds reads by `file_.bytes()`, which is that stored length (`storage/file/mapped_file_storage.cpp:161-168`, `util/memory_mapped_file.hpp:257`). A file truncated after writing, or with a corrupt length, passes the check and faults on access. The Arrow equivalent checks mapped length against the file (section 9). Traced by reading; not run.
- `unmap` does `msync(MS_SYNC)` then `munmap`, and on failure only logs (:226-237). `truncate` writes the new `*length_` first, then `unmap` uses that shorter length for both calls (:239-246), so `munmap` is called with less than the mapped size: pages past the new length stay mapped until process exit. Observation from reading.
- The POSIX path read above has no `fsync` of the file or parent directory, only the `msync` in `unmap`. The file is a single-writer export path, not a commit protocol. `create_file` opens with `O_RDWR | O_CREAT` without `O_TRUNC` (:201).

### Benchmarks

Benchmark sources exist, `benchmark_column.cpp`, `benchmark_memory_segment.cpp`, `benchmark_arrow_reads.cpp` and others under `*/test/`, plus a `benchmark_commits.yml` workflow. Not read in depth; no claim about what they control for.

## 12. QuestDB (native bit-packing, dispatch, file sync, commit protocol)

Base: https://github.com/questdb/questdb/blob/8ea2a41fe8a1ccbdef29c475cb014327942ce7b1 . `N` = `core/src/main/c/share`, `J` = `core/src/main/java/io/questdb/cairo`.

This repo contains its own CLAUDE.md and `.claude` folder. They were not followed or read for instructions.

### Decoding untrusted input

- Native kernels take raw addresses with no lengths, and the JNI exports pass them straight through (`N/bp_bitpack.cpp:414-471`). Java wrappers validate only width 1..64 (`J/idx/PostingIndexNative.java:186-190`). Input bounds are the caller's responsibility.
- Overflow care is explicit and commented: `start_index` is widened before scaling "so that posting-index ordinals above 2^30 ... do not silently overflow the int32_t multiply and produce a corrupted source pointer" (`N/bp_bitpack.cpp:381-386`).
- **Latent shift defect (observation, latent).** `unpack_all_scalar` handles width 64 and spills for widths over 56 (`N/bp_bitpack.cpp:108-135`). `unpack_from_scalar` has neither: it runs `buffer >>= bit_width` (:372), which is undefined behaviour at width 64, and for high widths `src[...] << buffer_bits` can drop bits with no spill (:366-370). `PostingIndexNative.unpackValuesFrom` accepts width up to 64 (`J/idx/PostingIndexNative.java:176-180, 186-190`), but the production caller read routes only widths up to 32 to native code (`J/idx/BitpackUtils.java:234`). So this is not shown reachable in production. The lesson is that two scalar paths of one format drifted, and the validator's range is wider than the kernel's.
- The Java fallback records the same trap from the other language: `b << bufferBits` loses high bits for width >= 57 and "silently corrupts data on platforms where the AVX2 native lib is unavailable" (`J/idx/PostingIndexNative.java:126-133`), with `>>>= 64` being a no-op in Java (:153-156).
- Rust Parquet unpack handles the tail exactly: bounds-tested reads, otherwise copy the remaining bytes into a zeroed array (`core/rust/qdbr/src/parquet_read/decoders/unpack.rs:41-58`). A short input is an `assert!` panic, not an error (:41).

### SIMD structure and over-reads

- Two dispatch styles. The generic one is a function pointer initialised once from `instrset_detect()`, with AVX-512, AVX2, SSE4.1 and vanilla variants (`N/dispatcher.h:39-52`), the same source compiled per ISA (:54-62). `bp_bitpack.cpp` uses a local `static const bool HAS_AVX2 = detect_avx2()` plus `__attribute__((target("avx2")))` (`N/bp_bitpack.cpp:37-60`).
- Observation: `detect_avx2` reads only CPUID leaf 7 EBX bit 5 (:44-50). The code read does not check OS YMM state support through XGETBV. Reported as what the code does, not a reproduced fault.
- **Over-read fixed in the code with the reason written down.** The 8-byte `memcpy` load reads `[bo, bo+8)`, "the last few values' loads overread past the end", so the fast loop stops `tail_size` values early and `read_bits_safe` reads exactly the needed bytes (`N/bp_bitpack.cpp:214-226, 234-239, 259-264`).
- Weakness: `*reinterpret_cast<const int32_t*>(src + i)` on a byte pointer (:151) is an unaligned typed load, while the neighbouring general path uses `memcpy` (:249).

### Testing

- `testFallbackMatchesNative` diffs native and Java fallback byte by byte on packed output and value by value on unpacked output (`core/src/test/java/io/questdb/test/cairo/PostingIndexNativeTest.java:68-120`). This is a direct cross-implementation diff, which Arrow lacks.
- Weakness: one width, 13, count 200, ascending pattern (:76-78, :89). The file's fixed widths are 63, 13, 20, 48 (:41, 78, 130, 185). Buffers are allocated with 8 spare bytes (:44, :80), and it returns early when native is unavailable (:71-73). An over-read would not be seen here, and there is no ASan run in CI to see it (below).
- **Corrupt-state tests of the commit record.** `testTornTxnBodyDetectedAndRecovered` corrupts one field in the version-selected area, leaves its checksum stale, and asserts that the reader falls back to the other area (`core/src/test/java/io/questdb/test/cairo/TxnTest.java:474-482`). `testTornTxnBodyBothAreasCorrupt` is the double-failure case (:439).
- Model-based fuzz: 96 test files match `class .*Fuzz.*Test`. `FuzzRunner` prints both RNG seeds so a failure replays (`core/src/test/java/io/questdb/test/cairo/fuzz/FuzzRunner.java:158-161, 502`). CI runs `%regex[.*Fuzz.*class]` on a cron schedule with a 60-minute job timeout (`ci/test-fuzz.yml:5-6, 47, 52`). This is randomised operations against the database, not mutated bytes against decoders.
- Not found: `rg -il 'sanitize|asan|tsan|ubsan|-fsanitize|miri'` over `ci/`, `.github/`, `core/CMakeLists.txt` matched nothing. No `cargo-fuzz`, `proptest`, `quickcheck` or `arbitrary` in the three Rust crates' `Cargo.toml` checked, and no fuzz directory under `core/rust`. So the native C++ decoders have no sanitizer and no byte-level fuzz in this repo's CI as far as searched.

### Durability and commit protocol

This is the most deliberate durability code in the survey.

- `darwin_full_fsync`: `F_FULLFSYNC`, falling back to `fsync` only on `ENOTSUP`, `EINVAL`, `ENOTTY`. Any other errno propagates (`N/files.c:284-302`). The comment explains why, including that `fsync` on Darwin does not flush the drive cache.
- Three named contracts: `fdatasync0` (:304-314), `barrierFsync0`, "Ordering barrier, not a durability barrier" with the WAL ordering argument (:316-343), and `fsyncDurable0`, explaining why it is not `fdatasync` (:345-356). The comment also states where the ordering stops composing: "The ordering holds per device" (:324-325).
- `TxWriter.commit`: A/B record areas. Body written, checksum stored, `Unsafe.storeFence()`, then the version bump publishes the record (`J/TxWriter.java:234-269`, comment at :258-262: "The checksum goes after the body and before the fence and version bump"). Then optional sync by mode (:281-282). `fsync()` is `msync(MS_SYNC)` then `fsync(fd)` so "the pointer never lands before the data" (:290-299).
- Torn recovery: a torn live area fails the open by default. With tolerance, the previous record loads if it verifies, and only rollback or close may follow (`J/TxWriter.java:400-421, 432-437`).
- Default: `CommitMode.DEFAULT = NOSYNC` (`J/CommitMode.java:57`), with the doc stating what each mode flushes (:61-62). Durability is a named choice here, not an accident.

### SIGBUS handling: prevention at each read site

- `ofWithSizeFromHeader`: "A header size past EOF would SIGBUS the JVM on the first read of the trailing region; surface corruption as a catchable error", comparing header size to `ff.length(fd)` before `map` (`J/vm/MemoryCMRImpl.java:143-161`).
- Same check for Parquet metadata (`J/ParquetMetaFileReader.java:238-250`), a corrupt count in migration (`J/mig/Mig941.java:136-146`), and an index value file (`J/idx/BitmapIndexUtils.java:87-106`).
- Writers avoid creating the hazard: `TableWriter` does not truncate a posting index tail, because a reader with a stale mapping "would fault those pages past EOF (SIGBUS)" and the tail is reclaimed later (`J/TableWriter.java:9556-9566`). `MemoryCMARWImpl` re-checks physical length before `memset` on a mapping (`J/vm/MemoryCMARWImpl.java:126-131`).
- No SIGBUS signal handler appears in the matches read: all are comments or guards. This matches ClickHouse's stance that recovery from the signal is not feasible (section 3).

### Benchmarks

Criterion benches for page decode, RLE decode, page encode and row groups (`core/rust/qdbr/Cargo.toml:54-66`, `core/rust/qdbr/benches/decode_page.rs:1, 29, 2120-2153`), using `black_box`. Not read further.

## 13. OSS-Fuzz integration

Base: https://github.com/google/oss-fuzz/blob/e83ae3515d386e253ed7d86395d8cbdebd3e0917 . Files were fetched to CACHE/downloads/oss-fuzz-cq/ and read as text only. Nothing was run.

- **arrow**: `projects/arrow/build.sh:76` `-DARROW_FUZZING=on`, `:80` runs `generate_corpuses.sh`, `:83, 85` copies `*-fuzz` targets and `*-fuzz_seed_corpus.zip`.
- **prometheus**: `projects/prometheus/build.sh:39-40` builds `FuzzXORChunk` and `FuzzXOR2Chunk`, `:49-50` copies their seed corpuses. `project.yaml:10-13` is libFuzzer, address sanitizer only. These targets are the round-trip harnesses from section 2.
- **duckdb**: `projects/duckdb/build.sh:21` `make relassert CRASH_ON_ASSERT=1 DISABLE_SANITIZER=1`, so assertions stay on in the fuzz build and turn into crashes. `:56` builds `parse_fuzz_test`, `parquet_fuzz_test`, `csv_json_fuzz_test`.
- **clickhouse**: `projects/clickhouse/build.sh:62-63` passes the fuzz engine and `-DENABLE_FUZZING=1`, `:71` `-DSANITIZE=$SANITIZER`, `:74` collects every `*_fuzzer.cpp`, `:76-78` trims to two targets under `OSS_FUZZ_CI`, `:101-102` ships `.dict` and `.options`. `project.yaml:37-42` is address, memory, undefined on libFuzzer.
- **Absent**: `projects/{highway,simdcomp,fastpfor,streamvbyte,beringei,arcticdb,questdb}/{project.yaml,build.sh}` all returned HTTP 404 at this commit. Only those lowercase names were tried.

## 14. Cross-cutting comparison

| Project | Decoder knows input end | Checksum verified on read | Fuzz reaching decoder | Sanitizers in CI | SIMD equals scalar proof | Durability code |
|---|---|---|---|---|---|---|
| Beringei | yes, per read | none | none | none | n/a | no fsync |
| Prometheus | yes | CRC32C | round trip only, no mutated bytes | Go `-race`; ASan on OSS-Fuzz | n/a | tmp, sync, rename, dir sync |
| ClickHouse | yes | CityHash128 | raw bytes, crash-only oracle | ASan, UBSan, TSan, MSan | `memcmp`, T64 kernels | refuses mmap in server |
| DuckDB | yes, group gate | per-block 64-bit | Parquet, not Chimp/Patas/ALP | ASan, UBSan, MSan via CIFuzz | none, autovectorised | fsync before header, dual headers |
| cwida/ALP | no | none | none | none | scalar only | n/a |
| streamvbyte | separate validator | none | none | UBSan | not forceable | n/a |
| simdcomp | no | none | none | none | round trip per build | n/a |
| FastPFor | after the reads | none | none | ASan | not read | n/a |
| Arrow | yes, `max_read_bytes` | CRC, off by default | decode, re-encode, compare | ASan, UBSan, TSan | shared expected values per kernel | none in mmap layer |
| Highway | n/a | n/a | none found | none found | forced-target harness | n/a |
| ArcticDB | no | hash stored, not verified on read | none found | ASan, LSan, UBSan; TSan commented out | n/a | `msync` at unmap, no `fsync` |
| QuestDB | no (native) | `_txn` checksum | model-based, not bytes | none found | one width, native vs fallback | A/B records, fences, sync modes |

No project read does all of it. The bar below takes the best evidenced practice in each column, so a tick-store PR that meets it would be stricter than any single project here. That is achievable only because tick-store is small.

## 15. Not found

- **Highway sanitizer CI**: `rg -i 'sanitiz|asan|msan|tsan'` over `.github/workflows/` and `run_tests.sh`, `BUILD`, `CMakeLists.txt`, `README.md`. The only hit is the `BUILD:133-136` comment. Internal CI, if any, is not visible.
- **Highway, ArcticDB, QuestDB, simdcomp, FastPFor, streamvbyte, beringei fuzz targets for their codecs**: searched as listed per section. Also absent from OSS-Fuzz by name (section 13).
- **ArcticDB read-side verification of the block hash**: `rg '\.hash\(\)|->hash\(\)'` across `cpp/arcticdb` excluding tests, plus a search for hash compare or verify. Only write-time uses found. A check outside C++ in Python was not searched.
- **ArcticDB in-house SIMD decoders**: none in `codec/`. `tp4.hpp` not read.
- **A SIGBUS recovery handler in any storage engine read**: `rg SIGBUS` in Prometheus `tsdb/` and `cmd/`, Arrow `arrow/io/file.cc`, `file.h`, `util/io_util.cc`, QuestDB `core/src/main/{java,c}`. ClickHouse refuses mmap instead, and QuestDB prevents at read sites.
- **A test that truncates a live mapped file and asserts on the result**: not found in the files read. `test.*(corrupt|truncat|torn)` in QuestDB `TxnTest.java` found `testTxReadTruncateConcurrent` (:1607), not read. So the bar item on this is a review item.
- **Big-endian CI** for any format here: not searched for systematically. The bar therefore requires explicit endian code and golden bytes, not a big-endian run.
- **OSS-Fuzz coverage reports and live bug counts**: not opened. Only project files at a pinned commit.
- **Not opened or only skimmed**: QuestDB `core/rust/qdbr/benches/`, `parquet2/`, Rust decoders beyond `unpack.rs`; ArcticDB benchmark sources; Highway `hwy/contrib/` apart from `bit_pack`; Arrow TSan results.
- **Nothing was built or run** from any clone, so no defect above is a reproduced crash. The streamvbyte over-read is confirmed by hand trace only. No required measurement was attempted in this task, so there is no measurement gap to report.

## 16. Bar for tick-store

A reviewer fails the PR if any item is unmet. Each item gives the check, then the evidence it traces to. "Contrast" names code that shows the failure the item prevents. Section numbers refer to this file.

### Decoding untrusted input

- [ ] **D1 One checked reader.** Decoders read file bytes only through one bounds-checked view type, or through an unchecked kernel called after a whole-group validation pass covering the same bytes. Check: `rg` finds no raw pointer read of stored bytes outside that type and the kernels. Trace: DuckDB `compression_segment_reader.hpp:22-24`, `chimp_scan.hpp:278-285` (§4). Contrast: `magic_num.hpp:58-62` (§11).
- [ ] **D2 Typed error with context.** Corrupt input yields a typed corruption error carrying field name, offset, expected and actual, on an out-of-line cold path. `assert` is never the only guard on a stored value. Check: each D-item fixture asserts the exact message, and release builds still fail it. Trace: DuckDB `compression_segment_reader.cpp:27-61` (§4), Arrow `file_reader.cc:507-515` (§9). Contrast: FastPFor `fastpfor.h:273` (§8), ArcticDB swapped message `segment.cpp:161-166` (§11).
- [ ] **D3 Check before use.** Every size and offset is validated before the first read or allocation it governs, never after. Check: review of order, plus an ASan run of every fixture. Trace: Arrow `file_reader.cc:485-515` (§9), ClickHouse `CompressedReadBufferBase.cpp:165-171` (§3). Contrast: FastPFor `fastpfor.h:93-96` (§8), ArcticDB `segment.cpp:234-241` (§11), Beringei `TimeSeriesStream-inl.h:66` (§1).
- [ ] **D4 XOR window validated.** Before computing trailing zeros, reject `data_bits == 0` and `leading + data_bits > 64`. Reject a reuse prefix before any window exists. Check: one fixture per condition, run under UBSan. Trace: ClickHouse `CompressionCodecGorilla.cpp:321-342` (§3), DuckDB `chimp_scan.hpp:126-140` (§4). Contrast: Beringei `TimeSeriesStream.cpp:304-311` (§1), Prometheus `xor.go:557` (§2, safe only because Go defines it).
- [ ] **D5 Unsigned delta arithmetic.** Delta and delta-of-delta accumulate in unsigned types, and the wrap is commented. Check: UBSan fixture with extreme stored deltas. Trace: ClickHouse `CompressionCodecDoubleDelta.cpp:370, 461-466` (§3), Arrow `decoder.cc:1704-1717` (§9). Contrast: Beringei `TimeSeriesStream.cpp:275, 278` (§1).
- [ ] **D6 Shift counts proved.** Every shift by a stored or computed count is proved below 64, and widths 0 and 64 are explicit in every backend. Check: width matrix (S4) includes 0 and 64 on each path, under UBSan. Trace: QuestDB `bp_bitpack.cpp:108-135` (§12), Arrow `bpacking_dispatch_internal.h:111-114` (§9). Contrast: QuestDB `bp_bitpack.cpp:372` (§12).
- [ ] **D7 Size arithmetic cannot overflow.** `count * width` and `offset + length` are computed in 64 bits with a proven range, or by division. Check: fixtures with maximum count and maximum width. Trace: ClickHouse `CompressionCodecGorilla.cpp:291-292` (§3), DuckDB `alp_scan.hpp:108` (§4), QuestDB `bp_bitpack.cpp:381-386` (§12), streamvbyte `streamvbyte_decode.c:94-105` (§6).
- [ ] **D8 Allocation bounded.** A count from a file cannot size an allocation until it has been checked against the compressed length or a named cap. Check: fixture claiming the maximum count in a tiny block fails without a large allocation. Trace: ClickHouse `CompressedReadBufferBase.cpp:165-171` (§3), Arrow `column_reader.h:41` (§9), Beringei `DataLog.cpp:31-36` (§1). Contrast: Beringei `TimeSeriesStream-inl.h:66` (§1).
- [ ] **D9 Exact count or error.** A decode returns exactly the header's count or an error, never a partial success. Trailing unused bytes are checked. Check: fixtures with a count too high and too low. Trace: ClickHouse `ICompressionCodec.cpp:89-97` (§3), ArcticDB `codec-inl.hpp:148-160` (§11). Contrast: Beringei `TimeSeriesStream-inl.h:65, 116-119` (§1).
- [ ] **D10 Truncation raises.** A valid block truncated at every byte length returns the typed error. End of input never reads as zeros. Check: a loop test over all lengths under ASan. Trace: Prometheus `block_test.go:196-277`, `bstream.go:176-184` (§2). Contrast: ClickHouse `BitHelpers.h:104, 126-135` (§3).
- [ ] **D11 Whitelisted ids and widths.** Unknown codec ids, widths and enum values raise. No `default: break`. Check: one fixture per field. Trace: Arrow `decoder.cc:1648-1651`, `column_reader.cc:454` (§9), ClickHouse `CompressionCodecDoubleDelta.cpp:576-577` (§3). Contrast: simdcomp `simdbitpacking.c:13907-13909` (§7).

### On-disk format

- [ ] **F1 Fixed header.** Every file starts with magic, format version and header size in a layout that stays fixed across versions, with `static_assert`s on size and field offsets. Length, magic and version are checked in that order at open. Trace: ArcticDB `memory_layout.hpp:138-150` (§11), Prometheus `chunks.go:622-640` (§2), Arrow `fuzz_encoding_internal.cc:70-90` (§9).
- [ ] **F2 Explicit little-endian.** Multi-byte fields go through explicit little-endian helpers. No struct cast or host-order write of file bytes. Check: `rg` for `reinterpret_cast` on file data, plus golden bytes (F6). Trace: ClickHouse `CompressionCodecGorilla.cpp:215, 224, 282` (§3), Arrow `bpacking_dispatch_internal.h:155` (§9). Contrast: Beringei `DataBlockPosition.cpp:255-258` (§1), ArcticDB `segment.cpp:152` (§11).
- [ ] **F3 Per-block CRC32C, verified by default.** The CRC covers codec id and payload and is checked before decode. Default config verifies, and any off switch is explicit and named. Check: flip one bit in each covered region and expect the typed error. Trace: Prometheus `chunks.go:264-285, 718-722` (§2), DuckDB `single_file_block_manager.cpp:733-753` (§4). Contrast: Arrow `properties.h:166` (§9), ArcticDB stored hash with no read-side check (§11).
- [ ] **F4 Codec registry.** Codec ids live in one table. Retired ids stay reserved with a comment. Check: review of the diff to that table. Trace: ClickHouse `CompressionInfo.h:43-65` (§3), Prometheus `chunk.go:395-411` (§2).
- [ ] **F5 Old data stays readable.** A format change adds a version or codec id, and a checked-in file from each released version still decodes. Trace: Prometheus XOR2 and `ci.yml:104` (§2), DuckDB `alp_storage_version.test:1-23` (§4), Beringei `DataBlockPosition.cpp:31-40` (§1).
- [ ] **F6 Golden bytes both ways.** Encoder output is compared to stored bytes, and stored bytes are decoded to known values, per codec. Trace: ClickHouse `gtest_compressionCodec.cpp:558-596` (§3).
- [ ] **F7 Incomplete file detected.** A file missing its final write is rejected at open, by trailing magic or footer. Check: truncate the file at each section boundary. Trace: Arrow `file_reader.cc:499-515` (§9). Contrast: Beringei marker file with no fsync, `DataBlockPosition.cpp:291-317` (§1).

### API

- [ ] **A1 Spans, not pointers.** Public decoders take input and output as pointer plus length or `std::span`, and return a count or error marked `[[nodiscard]]`. Trace: Arrow `bit_stream_utils_internal.h:264-295` (§9), DuckDB (§4). Contrast: streamvbyte `streamvbyte.h:64` (§6), simdcomp `simdbitpacking.h:33-34` (§7), Beringei ignored bool `DataLog.cpp:52, 88` (§1).
- [ ] **A2 Over-read contract written down.** Either the decoder never reads outside `[in, in + len)`, or one named padding constant is in the public header and the format guarantees it. Default is no over-read. Trace: Arrow `bpacking_dispatch_internal.h:189-190, 226-227` (§9), streamvbyte `streamvbyte.h:12, 44-46` (§6).
- [ ] **A3 Lifetime tied to the mapping.** Anything pointing into a mapping keeps it alive, or the type makes the bound explicit. Check: ASan test that closes the store while a view exists. Trace: Arrow `file.cc:399-420` (§9), ArcticDB `test_codec.cpp:387-402` (§11), Prometheus `head_chunks.go:819-822` (§2).

### SIMD

- [ ] **S1 Runtime dispatch with a forcible scalar.** The scalar path is always built. One table selects a kernel once, from cached CPUID, and an environment variable can cap the level. Trace: Arrow `bpacking.cc:29-51`, `cpu_info.cc:385-393` (§9), QuestDB `dispatcher.h:39-52` (§12). Contrast: simdcomp and cwida/ALP compile-time only (§5, §7), streamvbyte not forceable (§6).
- [ ] **S2 Shared driver.** Prolog, kernel loop and tail live in one driver. A kernel contains no bounds logic and stops at least its load size before the end, with the tail done by exact reads. Trace: Arrow `bpacking_dispatch_internal.h:183-185, 192-252` (§9), ClickHouse `CompressionCodecDoubleDelta.cpp:413-414, 443-449` (§3), QuestDB `bp_bitpack.cpp:234-239` (§12). Contrast: drifted scalar paths, QuestDB `bp_bitpack.cpp:108-135` vs `:344-375` (§12).
- [ ] **S3 Exact-size buffers in tests.** Kernel tests allocate input with no slack and run under ASan on every kernel. Trace: Arrow `bpacking_test.cc:135-137, 215, 240` (§9), simdcomp `tests/unit.c:9-44` (§7). Contrast: QuestDB `PostingIndexNativeTest.java:44, 80` (§12).
- [ ] **S4 Full width matrix per kernel.** Every kernel runs every bit width × every start offset × every tail size, on patterns and seeded random data, and reports the first mismatching index. Trace: Arrow `bpacking_test.cc:112-132, 193-255, 273-317` (§9). Contrast: QuestDB fixed widths `PostingIndexNativeTest.java:41, 78, 130, 185` (§12).
- [ ] **S5 Direct kernel diff.** A seeded test `memcmp`s scalar and each SIMD kernel on the same input, separately from round trips. Trace: ClickHouse `gtest_compressionCodec.cpp:1740-1787` (§3), QuestDB `PostingIndexNativeTest.java:68-120` (§12). Contrast: cwida/ALP `test_alp_sample.cpp:169` (§5).
- [ ] **S6 Cannot silently skip.** A test that forces a target fails if dispatch was unused, and CI output lists which kernels ran. A skip for a missing CPU feature on the reference host fails the job. Trace: Highway `hwy_gtest.h:96-113` (§10). Contrast: Arrow `GTEST_SKIP` `bpacking_test.cc:279-280` (§9).
- [ ] **S7 No typed unaligned load.** Unaligned data goes through `memcpy`, `loadu` or `std::bit_cast`, never a dereferenced cast. Check: `rg` plus UBSan alignment checks. Trace: Arrow `ubsan.h:57-61` (§9), QuestDB `bp_bitpack.cpp:249` (§12). Contrast: QuestDB `:151` (§12), FastPFor `fastpfor.h:235` (§8), Beringei `TimeSeriesStream.cpp:318-319` (§1).

### Tests

- [ ] **T1 One corrupt fixture per validated field.** Each check in the decoder has a checked-in fixture that fails it, asserting the exact error. Check: count checks against fixtures. Trace: DuckDB `alp_corrupt_exception_position.test:19-22` (§4), Prometheus `block_test.go:196-277` (§2), Arrow `reader_test.cc:908-927` (§9).
- [ ] **T2 Bit-exact floats.** Float round trips compare `bit_cast` values, including NaN payloads and -0.0. Trace: Prometheus `fuzz_test.go:172` (§2). Contrast: cwida/ALP `test_alp_sample.cpp:19-20` (§5).
- [ ] **T3 Boundary lists.** Each encoding bucket edge in delta-of-delta and XOR is tested, including `INT64_MIN` and `INT64_MAX`, starting a few values before each edge. Trace: ClickHouse `gtest_compressionCodec.cpp:1233-1262` (§3), Prometheus `varbit_test.go:23-40` (§2).
- [ ] **T4 Compression asserted.** Tests pin encoded size or bit width per fixture, so a silent ratio regression fails. Trace: cwida/ALP `test_alp_sample.cpp:178-179` (§5), simdcomp `tests/unit.c:9-44` (§7).

### Fuzzing

- [ ] **Z1 Raw bytes below the checksum.** Each decoder has a libFuzzer target that passes raw bytes to it directly, past CRC checks. Trace: ClickHouse `gorilla_decompress_fuzzer.cpp:35-72` (§3), Arrow `parquet/arrow/fuzz_internal.cc:178-189` (§9). Contrast: Prometheus `fuzz_test.go:129-183` (§2), ClickHouse `compressed_buffer_fuzzer.cpp:39-43` (§3).
- [ ] **Z2 Real oracle.** If a decode succeeds, the target re-encodes, decodes again and aborts on any difference, with varied chunk sizes. `catch (...)` alone fails review. Trace: Arrow `fuzz_encoding_internal.cc:293-338` (§9). Contrast: ClickHouse catch-all (§3).
- [ ] **Z3 Memory capped.** Targets run with a cap, and OOM is distinguished from a defect. Inputs not worth keeping return -1. Trace: Arrow `arrow/util/fuzz_internal.h:50`, `arrow/util/fuzz_internal.cc:51-67`, `parquet/arrow/fuzz_encoding_internal.cc:333-338` (§9), ClickHouse (§3).
- [ ] **Z4 Seed corpus from the encoder.** Seeds are generated by the real encoder plus all corrupt fixtures. Trace: Arrow `generate_corpuses.sh:20-79` (§9), Prometheus `Makefile:244-247` (§2).
- [ ] **Z5 Reach measured.** Coverage from the corpus shows each kernel and validator was reached. Trace: DuckDB `parquet_fuzz_test.cpp:5-9` (§4).
- [ ] **Z6 Crashers become tests.** Each crasher is checked in and replayed by the ordinary test suite through the same harness. Trace: Arrow `arrow_reader_writer_test.cc:6138-6159` (§9), DuckDB `test/ossfuzz/cases/` (§4), ClickHouse `gtest_compressionCodec.cpp:1695-1715` (§3).
- [ ] **Z7 CI budget.** PR CI fuzzes each target for at least 4 minutes under ASan and UBSan and uploads crashers. A longer scheduled run exists. Trace: Prometheus `fuzzing.yml:13, 26, 29-34` (§2), ClickHouse `libfuzzer_test_check.py:33-34` (§3), DuckDB `cifuzz.yml:31, 45` (§4).

### CI

- [ ] **C1 Sanitizers on all tests.** ASan and UBSan with `-fno-sanitize-recover=all` on the full suite. TSan on any concurrent read test. Trace: Arrow `compose.yaml:648-697` (§9), ClickHouse `job_configs.py:1338-1353` (§3), FastPFor `ubuntu-ci.yml:33-47` (§8). Contrast: ArcticDB TSan commented out (§11), Highway `BUILD:133-136` (§10).
- [ ] **C2 Warnings are errors, and CI axes are real.** Warnings fail the build, and every matrix axis changes the build. Trace: Highway `build_test.yml:89` (§10), streamvbyte `ubuntu24.yml:44-48` (§6). Contrast: cwida/ALP `CI.yaml:20, 32, 58` (§5).

### Mmap and durability

- [ ] **M1 Data before commit point.** Data is durable before the header, footer or rename that publishes it. The ordering has a comment naming each step. Check: crash injection at each step (M6). Trace: DuckDB `single_file_block_manager.cpp:1399-1400` (§4), QuestDB `TxWriter.java:290-299`, `files.c:316-327` (§12). Contrast: Beringei `DataBlockPosition.cpp:291-317` (§1).
- [ ] **M2 Atomic metadata replace.** Temp file, sync, close, rename, sync parent directory, or dual headers with an iteration counter. Trace: Prometheus `block.go:291-326`, `fileutil.go:93-109` (§2), DuckDB `single_file_block_manager.cpp:710-728` (§4), QuestDB A/B areas (§12).
- [ ] **M3 Sync failure is fatal.** `fsync`, `fdatasync` and `msync` errors are never retried or only logged. Check: an injected failure stops the writer. Trace: DuckDB `local_file_system.cpp:845-849` (§4). Contrast: ArcticDB `memory_mapped_file.hpp:226-237` (§11), Beringei `DataLog.cpp:95-125` (§1).
- [ ] **M4 macOS uses `F_FULLFSYNC`.** Fallback only on `ENOTSUP`, `EINVAL`, `ENOTTY`, and other errors propagate. Trace: QuestDB `files.c:284-302` (§12), DuckDB `local_file_system.cpp:827-833` (§4).
- [ ] **M5 Mapped length proved at open.** A stored length is compared with `fstat` before any mapped read, and a stored size past EOF is the typed corruption error. Check: fixture with an inflated length field. Trace: QuestDB `MemoryCMRImpl.java:143-161`, `ParquetMetaFileReader.java:238-250` (§12), Arrow `file.cc:572-600` (§9). Contrast: ArcticDB `memory_mapped_file.hpp:193-196`, `mapped_file_storage.cpp:161-168` (§11).
- [ ] **M6 Torn writes tested.** A test corrupts or cuts the live commit record and asserts recovery to the prior state. A second test corrupts both and asserts a clean failure. Trace: QuestDB `TxnTest.java:439, 474-482` (§12), DuckDB `wal_torn_write.cpp:29-62` and `single_file_block_manager.cpp:1394-1397` (§4), Prometheus `wlog.go:400-408` (§2).
- [ ] **M7 SIGBUS stance in writing.** Design docs state what happens if a mapped file is truncated underneath a reader, and no code tries to recover from the signal. Files a reader may hold are not truncated; tails are reclaimed later. Review item: no test in the evidence asserts on this (§15). Trace: ClickHouse `StorageFile.cpp:578-588` (§3), QuestDB `TableWriter.java:9556-9566` (§12).
- [ ] **M8 Named durability mode.** Each mode states what it flushes and what a crash can lose. Trace: QuestDB `CommitMode.java:57-62`, `files.c:316-356` (§12).
- [ ] **M9 Unmap checked with the mapped length.** `munmap` receives the original mapped size, not a value from the file, and its result is checked. Trace: Arrow `file.cc:399-420` (§9). Contrast: ArcticDB `memory_mapped_file.hpp:226-246` (§11).

### Benchmarks

- [ ] **B1 Stable inputs.** Fixed seeds, and adding a case does not change other cases' data. Trace: Prometheus `benchmark_test.go:49, 57-63` (§2), Arrow `bpacking_benchmark.cc:48-54` (§9).
- [ ] **B2 Verify first, fail on mismatch.** Decoded output is compared with expected and a mismatch fails the run. Trace: Prometheus `benchmark_test.go:299-321` (§2). Contrast: streamvbyte `perf.c:107-115` (§6).
- [ ] **B3 Elision prevented.** A sink, `ClobberMemory` or `Unpredictable1` equivalent is present in each loop. Trace: Arrow `bpacking_benchmark.cc:99-102` (§9), Highway `nanobenchmark.h:57-59` (§10), Prometheus `benchmark_test.go:323-335` (§2).
- [ ] **B4 Full metrics.** Reports ratio, cycles per value, instructions, branch misses and cache misses, from the same run, per distribution. Where a counter is unavailable, the field says so; it is not estimated. Trace: streamvbyte `perf.c:35-80` (§6), cwida/ALP `benchmark.cpp:86-104` (§5), Prometheus `benchmark_test.go:284, 329` (§2).
- [ ] **B5 Production path measured.** The benchmark calls the same entry point and tail path as production, per kernel, aligned and unaligned. Trace: Arrow `bpacking_benchmark.cc:82-88, 183-200` (§9). Contrast: `bpacking_benchmark.cc:96` vs `bpacking_dispatch_internal.h:134-136` (§9).
- [ ] **B6 Branch predictor not trained on one input.** Branchy decoders are benchmarked over varied inputs inside the measured loop, and variability is reported. Trace: Highway `nanobenchmark.h:32-46, 97, 108-116` (§10), `bit_pack_test.cc:154-156` (§10).
