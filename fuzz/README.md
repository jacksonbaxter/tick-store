# Fuzzing

Thirteen libFuzzer targets take raw bytes directly, with nothing checking them first:

| Target | Input | Oracle in `itch_oracles.hpp` |
|---|---|---|
| `fuzz_columns` | a whole column directory, as length-prefixed sections | `check_columns` in `columns_oracles.hpp`: each file sits in its own heap block, so ASan sees a read past its end. Errors repeat, every frame an accepted directory gives is one whole frame, re-exporting those frames rebuilds the same frames and tail, a cut of any file is refused, and a copy with one bit flipped is refused or gives frames that re-export cleanly |
| `fuzz_itch_message` | one payload | `check_message`: both decoders agree, decode then encode gives back the exact input, decoding that gives the same message, and a short buffer fails cleanly |
| `fuzz_itch_frame` | a byte stream | `check_stream`: frames are contiguous, every frame plus the reported tail rebuilds the input exactly, and a shorter input gives the same frames that fit. Each of the two stops, the input's and the shorter input's, must sit where the last whole frame ends and pass `check_stop` against the bytes after it: end only with none left, a truncated prefix on 1 byte, a zero-length stop on a `00 00` prefix and a truncated body on a prefix declaring more than is left, each with the `expected` and `actual` `frame.hpp` gives it (0 and 0 for end, 2 and 1 for a truncated prefix, and for the other two the prefix's length, 0 for zero-length, and the bytes after the prefix). The unit suite hands `check_stop` wrong stops of each kind, since the cursor itself cannot be made to give one |
| `fuzz_gorilla_timestamps` | a column of Gorilla delta-of-delta blocks | `check_timestamp_column` in `codec_oracles.hpp`: every block the decoder accepts re-encodes to exactly its bytes (the format has one encoding per sequence), decodes the same from an exact copy, and is refused once cut; refusals repeat and leave the output alone; a whole valid column decodes at its exact count and not one off; the accepted values re-encode and decode at three block sizes; and the input's bytes, read as 64-bit values, go through the encoder and back at a block size the input picks |
| `fuzz_gorilla_xor` | a column of Gorilla XOR blocks | `check_xor_column`: the same checks on the XOR codec, then the price-to-double mappings on every word: an inverse that accepts a double gives an integer that maps back to it, every Price(4) comes back through dollars, and every integer up to 2^53 through a double |
| `fuzz_pfor` | a column of patched frame of reference blocks | `check_pfor_column` in `codec_oracles.hpp`: the format leaves the encoder choices (scale, mode, width, base), so instead of bytes the oracle checks that every accepted block decodes the same from an exact copy and is refused once cut, refusals repeat and leave the output alone, the block's values re-encode with each of the four option sets of its transform to a block that decodes to them, and the same bytes twice; a whole valid column decodes at its exact count and not one off; the values re-encode and decode in all sixteen option sets at three block sizes; and the input's bytes, as 64-bit values, go through all sixteen and back at a block size the input picks, up to 97 or just past one 1,024-value vector |
| `fuzz_stream_vbyte` | a column of Stream VByte blocks | `check_svb_column`: the same checks with the encoder with and without delta, and one more: every field but zigzag has one valid value and the encoder takes zigzag only when strictly smaller, so re-encoding a block's values with its own delta flag gives exactly its bytes when the encoder makes the same zigzag choice, and otherwise a smaller block, or one the same size when the input took zigzag |
| `fuzz_dictionary` | a column of dictionary blocks | `check_dict_column`: the same checks as `fuzz_pfor` with the dictionary encoder, whose choices are the mode and the nested PFOR blocks', and one more: a block's entries are distinct and all used, so its entry count must be its count of distinct values, the encoder must write the same count, and it takes move-to-front only within its 2,048-entry limit |
| `fuzz_run_length` | a column of run-length blocks | `check_rle_column`: the same checks with the run-length encoder, whose choices are the nested PFOR blocks', and one more: runs are maximal, so a block's run count must be its count of maximal runs, and the encoder must write the same count |
| `fuzz_container` | a whole container file | `check_container` in `container_oracles.hpp`: the input goes in three ways, with CRCs verified, with `Checksums::skip`, and with every CRC its structure points at recomputed (`fixed_checksums`), so the bytes reach the structural checks below the CRCs (Z1). Skipping and recomputing must agree on every verdict, and errors repeat. An accepted file is rebuilt from its parsed values alone through `build_container`, the writer's encoding, and must equal the input byte for byte (the format has one encoding per file), or the input with its CRCs recomputed when they were skipped; every block's view must match its directory entry and every section must be found by its name at its own offset. Then a cut of it and a copy with one bit flipped must each be refused or pass the same checks in full. A cut is not required to fail: a file whose section holds a whole container in the right place leaves a valid file when cut there, so the unit suite checks the golden file at every length instead |
| `fuzz_itch_store` | a whole ITCH store file | `check_itch_store` in `itch_store_oracles.hpp`: the input is read with `Checksums::skip` and, with every CRC its container structure points at recomputed (`fixed_checksums`), with CRCs verified, so the bytes reach the store's own checks below the CRCs: its sections, every block's metadata and segment summary, the directory, the column chunks, the zstd wrapper, the import lists and stock exceptions (Z1). Both reads must give the same verdict, the same error or the same records, and a refusal repeats. The encoding leaves the writer choices, so an accepted file is decoded, re-encoded and compared (Z2): its records and tail go back through the real writer at its segment size, that file must read back to the same records and tail, and writing them again must give the same bytes. Every segment is also read on its own, last to first, and must give what the sequential read gave. A file declaring more than 64 records a segment is opened and its directory checked but not decoded, since a segment's raw bytes reach 65,535 a record held as 64-bit values |
| `fuzz_itch_query` | a whole ITCH store file | `check_itch_query` in `itch_query_oracles.hpp`: the input is read with every CRC its container structure points at recomputed, so the bytes reach the query engine's index decoders below the CRCs: the seek index, the locate runs and their directory, the time bases, the plain flag, and each piece and codec block the indexed path decodes on its own. Every rows query, minute aggregate, locate count and share sum must answer or refuse, and give the same result asked twice. The indexed path checks only what it reads, so it may answer a file the whole decode refuses; but when `SegmentReader` accepts every segment, every indexed answer must equal the exact path's, which maps each record as `tick_export` does, and the exact path's must equal what the oracle computes itself from the column form of the whole decode, no segment skipped. Both paths choose segments with the same summary test and seed raw rows the same way, so only that third answer can see a fault in either (review C-5). The column form shares the rest of the mapping with both paths, `row_values()` with `hot_fields()` and `fits_columns()`, reading only each record's type byte from its payload (review RD-6): a fault in which form a typed record takes, or in a hot or rare row's fields, that shows only outside plain segments passes this oracle. The column export's tests see a fault in a hot row's fields: a share count made one more failed `Columns.EveryFrameAndTheTailComeBack` and `Columns.GoldenAddOrderColumns`. Requests come from the file: windows over its summaries' times, a one-nanosecond window at a segment's first time and a window and locate steered by the input's last bytes, for every locate, locate 0 and two named ones. Files declaring more than 20,487 records a segment, a 16,384-value piece and a seek group and a few records, are opened but not queried |
| `fuzz_stub_store` | a whole store file | `check_store` in `store_oracles.hpp`: errors repeat, an accepted file rebuilds the input exactly from its parsed values and holds the records its header counts, a cut of it is refused, and a copy with one bit flipped is refused or rebuilds to the flipped bytes |

## Build

Needs upstream clang: Apple clang has no fuzzer runtime. On macOS the targets link with
that clang's own `ld64.lld` (`-fuse-ld=lld`, in `cmake/Sanitizers.cmake`), because the
runner's Xcode 16.4 linker refuses the coverage sections clang 23 writes. The build regenerates the
seeds from the real encoder and the real store writer into
`build/fuzz/fuzz-seeds/{message,frame,store,container,itch_store,itch_query,columns,gorilla_timestamps,gorilla_xor,pfor,stream_vbyte,dictionary,run_length}`.
They are never committed. The ITCH store seeds are files from the real `ItchStoreWriter`: the 23
types with raw records and a tail at 1, 2, 5, 8, 25 and 64 records a segment, empty stores, and a
book of orders created, named, replaced and deleted across segments at 7, 32 and 64, with a stock
exception and references under other locates; one over the 64-record cap; then the 8-record store
with each store section field, a symbols count and order, each metadata field of a type block and
of a column block (its piece index among them), a block's codec, a dropped, repeated or swapped block, a column wrapped in zstd
by hand with each zstd header field changed, and a byte of each of the first twelve payloads
changed, and a store of an add and its execution with the execution's reference distance one
past its locate's list, each rebuilt through `build_container` so its CRCs hold; and cuts. The query seeds are
stores of 9,000 records of adds, executions, cancels, deletes, replaces, trades and system events
at 4,500, 9,000 and 20,487 records a segment, one of 18,384 records in one segment so that its
columns are in two pieces, with its type column's second piece renumbered and its first a value
short, one with records the column form keeps raw, the 4,500-record store with a byte of each
index column (the seek index, the run directory, the locate column, the time bases and the
residual times) changed at four offsets and the plain flag flipped both ways, each rebuilt with
valid CRCs, and every ITCH store seed. The container seeds are files from the real `ContainerWriter`: empty,
one empty block, one section, empty blocks and sections sharing offsets, forty blocks over every
codec with sections between them, and a typical file with a real timestamp block; then that file
with each header, trailer and footer field corrupted (footer fields with every CRC recomputed, so
the structural check is what refuses them), a repeated section name, a stale block and section CRC,
and cuts at every boundary. The Gorilla seeds are columns from the real encoders at block sizes
1, 3 and 4,096, cuts of them, each header field corrupted, and a hand-built block for each
refusal the decoders make. The integer seeds are columns from every PFOR option set and from
Stream VByte with and without delta, at the same block sizes, of ITCH-like timestamps,
whole-cent prices with stub quotes, a regular clock, multiples of 7 with values off it, values
near 2^64 and each width edge over a full vector and a tail; cuts of them; each header field
corrupted; and a small block from each encoder with each of its first 48 payload bytes set to
0, to 0xFF and to one more. Stream VByte also gets a hand-built block for each refusal its
decoder makes, and one with zigzag set where plain is no larger. The dictionary and
run-length seeds are the same integer columns from their encoders, plus columns with few
distinct values (share-like sizes that take Huffman codes, a locate-like column that takes
move-to-front, runs of a few values, one value, two alternating), each at the three block
sizes and cut, and the first 64 values of each as one block with each header field
corrupted and each of its first 48 payload bytes changed as above. Each also gets a
hand-built block for each of its decoder's own refusals: a dictionary entry repeated, out of
frequency order, tied but not ascending, unused, and a sorted dictionary not ascending; runs
not maximal, and lengths one short of and one past the count. The dictionary seeds also hold
a valid block of 738 bytes whose 32,768 entries share one slot of the decoder's repeat check,
the input that made that check quadratic (B-1 in `docs/REVIEW.md`).

```sh
cmake --preset fuzz && cmake --build --preset fuzz
mkdir -p build/fuzz/corpus/itch_message build/fuzz/corpus/itch_frame \
    build/fuzz/corpus/stub_store build/fuzz/corpus/container build/fuzz/corpus/itch_store \
    build/fuzz/corpus/itch_query \
    build/fuzz/corpus/columns \
    build/fuzz/corpus/gorilla_timestamps build/fuzz/corpus/gorilla_xor \
    build/fuzz/corpus/pfor build/fuzz/corpus/stream_vbyte \
    build/fuzz/corpus/dictionary build/fuzz/corpus/run_length build/fuzz/artifacts
```

## Run

From the repository root. New inputs go to the first directory, so the seeds stay
as generated. Budget a run with `-max_total_time`. The flags, folders and `-max_len` below
are `fuzz/launch.txt`'s, which also holds the ASan options; `scripts/check.sh`'s fuzz steps
and the G-3 launcher read that file.

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

build/fuzz/fuzz/fuzz_container -max_total_time=60 -max_len=70000 \
    -rss_limit_mb=2048 -malloc_limit_mb=256 -timeout=10 -print_final_stats=1 \
    -artifact_prefix=build/fuzz/artifacts/ \
    build/fuzz/corpus/container build/fuzz/fuzz-seeds/container

build/fuzz/fuzz/fuzz_itch_store -max_total_time=60 -max_len=70000 \
    -rss_limit_mb=2048 -malloc_limit_mb=256 -timeout=10 -print_final_stats=1 \
    -artifact_prefix=build/fuzz/artifacts/ \
    build/fuzz/corpus/itch_store build/fuzz/fuzz-seeds/itch_store

build/fuzz/fuzz/fuzz_itch_query -max_total_time=60 -max_len=131072 \
    -rss_limit_mb=2048 -malloc_limit_mb=256 -timeout=10 -print_final_stats=1 \
    -artifact_prefix=build/fuzz/artifacts/ \
    build/fuzz/corpus/itch_query build/fuzz/fuzz-seeds/itch_query

build/fuzz/fuzz/fuzz_columns -max_total_time=60 -max_len=70000 \
    -rss_limit_mb=2048 -malloc_limit_mb=256 -timeout=10 -print_final_stats=1 \
    -artifact_prefix=build/fuzz/artifacts/ \
    build/fuzz/corpus/columns build/fuzz/fuzz-seeds/columns

build/fuzz/fuzz/fuzz_gorilla_timestamps -max_total_time=60 -max_len=70000 \
    -rss_limit_mb=2048 -malloc_limit_mb=256 -timeout=10 -print_final_stats=1 \
    -artifact_prefix=build/fuzz/artifacts/ \
    build/fuzz/corpus/gorilla_timestamps build/fuzz/fuzz-seeds/gorilla_timestamps

build/fuzz/fuzz/fuzz_gorilla_xor -max_total_time=60 -max_len=70000 \
    -rss_limit_mb=2048 -malloc_limit_mb=256 -timeout=10 -print_final_stats=1 \
    -artifact_prefix=build/fuzz/artifacts/ \
    build/fuzz/corpus/gorilla_xor build/fuzz/fuzz-seeds/gorilla_xor

build/fuzz/fuzz/fuzz_pfor -max_total_time=60 -max_len=16384 \
    -rss_limit_mb=2048 -malloc_limit_mb=256 -timeout=10 -print_final_stats=1 \
    -artifact_prefix=build/fuzz/artifacts/ \
    build/fuzz/corpus/pfor build/fuzz/fuzz-seeds/pfor

build/fuzz/fuzz/fuzz_stream_vbyte -max_total_time=60 -max_len=70000 \
    -rss_limit_mb=2048 -malloc_limit_mb=256 -timeout=10 -print_final_stats=1 \
    -artifact_prefix=build/fuzz/artifacts/ \
    build/fuzz/corpus/stream_vbyte build/fuzz/fuzz-seeds/stream_vbyte

build/fuzz/fuzz/fuzz_dictionary -max_total_time=60 -max_len=16384 \
    -rss_limit_mb=2048 -malloc_limit_mb=256 -timeout=10 -print_final_stats=1 \
    -artifact_prefix=build/fuzz/artifacts/ \
    build/fuzz/corpus/dictionary build/fuzz/fuzz-seeds/dictionary

build/fuzz/fuzz/fuzz_run_length -max_total_time=60 -max_len=16384 \
    -rss_limit_mb=2048 -malloc_limit_mb=256 -timeout=10 -print_final_stats=1 \
    -artifact_prefix=build/fuzz/artifacts/ \
    build/fuzz/corpus/run_length build/fuzz/fuzz-seeds/run_length
```

- `-max_len`: no message is longer than 50 bytes, so 256 leaves room for every
  wrong-length case. A frame can hold 65,535 bytes, so 70,000 reaches the largest
  one and the frames after it, and in a store file the header too.
- `-rss_limit_mb` and `-malloc_limit_mb`: the targets allocate at most a few times
  the input size, so hitting either is a defect. libFuzzer reports it as
  out-of-memory, distinct from a crash.
- A Gorilla column of 70,000 bytes holds blocks of up to about 560,000 values, past the
  default block of 4,096, so several whole blocks fit, and the decoders reserve no more than
  the payload can hold, about 4.5 MB.
- `fuzz_pfor` takes `-max_len=16384`: each input goes through sixteen encoders, most with
  the adaptive scale trying several factors, so inputs of 70,000 bytes held it near 50 runs a
  second under ASan; at 16,384 it runs about twice as fast. That still holds a full
  1,024-value vector at width 64 (8,192 bytes) and a tail, and 2,048 values for part 2.
  `fuzz_dictionary` and `fuzz_run_length` take the same limit: each block nests PFOR blocks
  and tries several of them, and 16,384 bytes still hold over a vector of values.
- `fuzz_container` takes `-max_len=70000`: room for a footer of over 2,000 block entries, or a
  thousand sections, and for blocks past the writer's tiny ones. Each input goes through the parser
  three ways and is rebuilt once, about 125,000 runs a second under ASan.
- `fuzz_itch_store` takes `-max_len=70000`, room for a store of a few hundred records with its
  footer and every segment summary. A store's raw bytes are its one input that zstd can
  inflate, up to 65,535 bytes a record held as 64-bit values, which is why segments over 64
  records are not decoded: at 64 that is 33 MB, inside `-malloc_limit_mb`.
- `fuzz_itch_query` takes `-max_len=131072`: its seeds, 9,000 records over several seek groups,
  are near 60 KB, and the room above lets mutations grow them. Each input is queried twice by
  the indexed path and, when the whole decode accepts it, once more by the exact path, about
  140 runs a second under ASan on the Mac.
- `fuzz_itch_message` returns -1 for an input over 65,535 bytes, which no frame can
  declare, so it stays out of the corpus.
- The G-3 gate is 30 minutes per target under the heavy-job lock: every target on macOS, and on
  Linux each target whose decode reaches the SIMD kernels (`check_fuzz.py`'s `AVX2_PATH`), with
  `TICK_STORE_SIMD=avx2`. Those targets also decode each block with the scalar set forced and
  compare, so one run covers both sets. Linux also runs each target that verifies CRC32C on its
  input (`CRC32C_PATH`: `fuzz_container`, `fuzz_itch_store` and `fuzz_itch_query`), since
  there `store::crc32c()` runs the SSE4.2 kernel, which the Mac cannot. The oracles compute the
  CRCs they recompute with the same kernel, so these runs check the kernel for memory errors
  on fuzzed lengths and offsets; its agreement with the software table is the unit test
  `Crc32c.HardwareMatchesSoftwareOnRandomLengthsAndOffsets`.
- A G-3 run of one target is `scripts/fuzz_g3.py`'s three parts, one after another, each with
  the flags, caps, ASan options and `-max_len` of `fuzz/launch.txt`:
  1. the replay: `-runs=0` over the saved corpus and the seeds, which runs every input once
     through the target and its oracle, with the per-input leak check, exactly as the start of a
     fuzzing run does, and stops. A finding, a nonzero exit or no `Done` line fails the target
     there;
  2. the merge: `-merge=1` of the same two folders into an empty one, which keeps the smallest
     set of inputs that reaches all their coverage and features. libFuzzer restarts its merge
     child past an input that crashes it, so a second attempt fails the target too;
  3. the timed run: `-max_total_time=1800` on the merged folder alone. It loads that smaller
     set, prints `INITED`, and from then on runs only inputs it makes by mutation; its new
     inputs go to the merged folder and are then copied to the saved corpus.
  The results file records, per target, the replay's inputs, runs and seconds, the merge's
  kept inputs and attempts, and the timed run's seconds before and after its first new input,
  split at `INITED`. Before this, the timed run replayed the whole corpus inside its 30
  minutes: at `ea1dde4`, `fuzz_pfor` spent up to 567 of its 1,801 s on `debian` before its
  first new input (docs/NOTES.md, G-3). Then `scripts/fuzz_results.py` builds
  `results/fuzz-g3-<commit>.json` from each host's logs, and `scripts/check_fuzz.py` refuses it
  if a target is missing, a clean run's timed part is under 1,800 s, its replay or merge was
  not clean, or anything a fuzz binary is built from or launched with changed after its commit.
  Each host's `host.json`, the facts `fuzz_results.py` reads, is written by
  `scripts/fuzz_host.py` from the fuzz build's compile command and link flags and
  `fuzz/launch.txt`, with no absolute path and no host name in it.
  docs/NOTES.md, "The G-3 launch at the freeze", has each host's commands.
- `fuzz/launch.txt` sets the leak options: `detect_leaks=1:leak_check_at_exit=0` for the
  fuzzers on macOS, where ASan leaves leak detection off and the check at exit reports
  libFuzzer's own RSS thread, and `detect_leaks=1` on Linux, where it is the default, ahead of
  anything in `ASAN_OPTIONS`. libFuzzer then checks each input for a leak and exits 77 on one. Its
  `asan-test` step sets `detect_leaks=1` too, so a leaking unit test fails on the Mac as well.

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
`tests/unit/crashers/itch_message/`, `itch_frame/`, `stub_store/`, `container/`, `itch_store/`,
`itch_query/`, `columns/`,
`gorilla_timestamps/`, `gorilla_xor/`, `pfor/`, `stream_vbyte/`, `dictionary/` or
`run_length/` and fix the bug in the same commit. `fuzz_regression_test.cpp` replays those files through the same
oracles in every test build.
