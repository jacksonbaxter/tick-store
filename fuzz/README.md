# Fuzzing

Thirteen libFuzzer targets take raw bytes directly, with nothing checking them first:

| Target | Input | Oracle in `itch_oracles.hpp` |
|---|---|---|
| `fuzz_columns` | a whole column directory, as length-prefixed sections | `check_columns` in `columns_oracles.hpp`: each file sits in its own heap block, so ASan sees a read past its end. Errors repeat, every frame an accepted directory gives is one whole frame, re-exporting those frames rebuilds the same frames and tail, a cut of any file is refused, and a copy with one bit flipped is refused or gives frames that re-export cleanly |
| `fuzz_itch_message` | one payload | `check_message`: both decoders agree, decode then encode gives back the exact input, decoding that gives the same message, and a short buffer fails cleanly |
| `fuzz_itch_frame` | a byte stream | `check_stream`: frames are contiguous, every frame plus the reported tail rebuilds the input exactly, and a shorter input gives the same frames that fit. Each of the two stops, the input's and the shorter input's, must sit where the last whole frame ends and pass `check_stop` against the bytes after it: end only with none left, a truncated prefix on 1 byte, a zero-length stop on a `00 00` prefix and a truncated body on a prefix declaring more than is left, each with the `expected` and `actual` `frame.hpp` gives it (0 and 0 for end, 2 and 1 for a truncated prefix, and for the other two the prefix's length, 0 for zero-length, and the bytes after the prefix). The unit suite hands `check_stop` wrong stops of each kind, since the cursor itself cannot be made to give one |
| `fuzz_gorilla_timestamps` | a column of Gorilla delta-of-delta blocks | `check_timestamp_column` in `codec_oracles.hpp`: every block the decoder accepts re-encodes to exactly its bytes (the format has one encoding per sequence), decodes the same from an exact copy, and is refused once cut; refusals repeat and leave the output alone; a whole valid column decodes at its exact count and not one off; the accepted values re-encode and decode at three block sizes; and the input's bytes, read as 64-bit values, go through the encoder and back at a block size the input picks |
| `fuzz_gorilla_xor` | a column of Gorilla XOR blocks | `check_xor_column`: the same checks on the XOR codec, then the price-to-double mappings on every word: an inverse that accepts a double gives an integer that maps back to it, every Price(4) comes back through dollars, and every integer up to 2^53 through a double |
| `fuzz_pfor` | a column of patched frame of reference blocks | `check_pfor_column` in `codec_oracles.hpp`: the format leaves the encoder choices (scale, mode, width, base), so instead of bytes the oracle checks that every accepted block decodes the same from an exact copy and is refused once cut, refusals repeat and leave the output alone, the block's values re-encode with each of the four option sets of its transform to a block that decodes to them, and the same bytes twice; a whole valid column decodes at its exact count and not one off; the values re-encode and decode in all sixteen option sets at three block sizes; and the input's bytes, as 64-bit values, go through all sixteen and back at a block size the input picks, up to 97 or just past one 1,024-value vector. The fuzz target bounds the values it takes and budgets the re-encodes ("Bounds" below) |
| `fuzz_stream_vbyte` | a column of Stream VByte blocks | `check_svb_column`: the same checks with the encoder with and without delta, and one more: every field but zigzag has one valid value and the encoder takes zigzag only when strictly smaller, so re-encoding a block's values with its own delta flag gives exactly its bytes when the encoder makes the same zigzag choice, and otherwise a smaller block, or one the same size when the input took zigzag |
| `fuzz_dictionary` | a column of dictionary blocks | `check_dict_column`: the same checks as `fuzz_pfor` with the dictionary encoder, whose choices are the mode and the nested PFOR blocks', and one more: a block's entries are distinct and all used, so its entry count must be its count of distinct values, the encoder must write the same count, and it takes move-to-front only within its 2,048-entry limit. Bounded as `fuzz_pfor` is |
| `fuzz_run_length` | a column of run-length blocks | `check_rle_column`: the same checks with the run-length encoder, whose choices are the nested PFOR blocks', and one more: runs are maximal, so a block's run count must be its count of maximal runs, and the encoder must write the same count. Bounded as `fuzz_pfor` is |
| `fuzz_container` | a whole container file | `check_container` in `container_oracles.hpp`: the input goes in three ways, with CRCs verified, with `Checksums::skip`, and with every CRC its structure points at recomputed (`fixed_checksums`), so the bytes reach the structural checks below the CRCs (Z1). Skipping and recomputing must agree on every verdict, and errors repeat. An accepted file is rebuilt from its parsed values alone through `build_container`, the writer's encoding, and must equal the input byte for byte (the format has one encoding per file), or the input with its CRCs recomputed when they were skipped; every block's view must match its directory entry and every section must be found by its name at its own offset. Then a cut of it and a copy with one bit flipped must each be refused or pass the same checks in full. A cut is not required to fail: a file whose section holds a whole container in the right place leaves a valid file when cut there, so the unit suite checks the golden file at every length instead |
| `fuzz_itch_store` | a whole ITCH store file | `check_itch_store` in `itch_store_oracles.hpp`: the input is read with `Checksums::skip` and, with every CRC its container structure points at recomputed (`fixed_checksums`), with CRCs verified, so the bytes reach the store's own checks below the CRCs: its sections, every block's metadata and segment summary, the directory, the column chunks, the zstd wrapper, the import lists and stock exceptions (Z1). Both reads must give the same verdict, the same error or the same records, and a refusal repeats. The encoding leaves the writer choices, so an accepted file is decoded, re-encoded and compared (Z2): its records and tail go back through the real writer at its segment size, that file must read back to the same records and tail, and writing them again must give the same bytes. Every segment is also read on its own, last to first, and must give what the sequential read gave. A file declaring more than 64 records a segment, or whose segments state more than 2^20 values in all, is opened and its directory checked but not decoded, since a segment's raw bytes reach 65,535 a record held as 64-bit values ("Bounds") |
| `fuzz_itch_query` | a whole ITCH store file | `check_itch_query` in `itch_query_oracles.hpp`: the input is read with every CRC its container structure points at recomputed, so the bytes reach the query engine's index decoders below the CRCs: the seek index, the locate runs and their directory, the time bases, the plain flag, and each piece and codec block the indexed path decodes on its own. Every rows query, minute aggregate, locate count and share sum must answer or refuse, and give the same result asked twice. The indexed path checks only what it reads, so it may answer a file the whole decode refuses; but when `SegmentReader` accepts every segment, every indexed answer must equal the exact path's, which maps each record as `tick_export` does, and the exact path's must equal what the oracle computes itself from the column form of the whole decode, no segment skipped. Both paths choose segments with the same summary test and seed raw rows the same way, so only that third answer can see a fault in either (review C-5). The column form shares the rest of the mapping with both paths, `row_values()` with `hot_fields()` and `fits_columns()`, reading only each record's type byte from its payload (review RD-6): a fault in which form a typed record takes, or in a hot or rare row's fields, that shows only outside plain segments passes this oracle. The column export's tests see a fault in a hot row's fields: a share count made one more failed `Columns.EveryFrameAndTheTailComeBack` and `Columns.GoldenAddOrderColumns`. Requests come from the file: windows over its summaries' times, a one-nanosecond window at a segment's first time and a window and locate steered by the input's last bytes, for every locate, locate 0 and two named ones. Files declaring more than 20,487 records a segment, a 16,384-value piece and a seek group and a few records, or whose segments state more than 135,168 values, are opened but not queried, and past 69,632 values the second ask and the scalar ask take two requests ("Bounds") |
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
the input that made that check quadratic (B-1 in `docs/REVIEW.md`). Each bounded target and each
other codec target also gets its costliest input at its `-max_len` under a name, listed in
`fuzz/bound_seeds.txt` and built in `fuzz/bound_seeds.hpp` ("Bounds").

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
  records are not decoded: at 64 that is 33 MB, inside `-malloc_limit_mb`. Files stating more
  than 2^20 values are not decoded either, so a column holds at most 8 MB ("Bounds").
- `fuzz_itch_query` takes `-max_len=131072`: its seeds, 9,000 records over several seek groups,
  are near 60 KB, and the room above lets mutations grow them. Each input is queried twice by
  the indexed path and, when the whole decode accepts it, once more by the exact path, about
  140 runs a second under ASan on the Mac. A file stating more than 135,168 values is not
  queried ("Bounds").
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
  minutes: at `ea1dde4`, `fuzz_pfor` spent up to 567 of its 1,801 s on the Linux host before its
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

## Bounds

**Why `-timeout=10`.** libFuzzer stops on any input whose one execution runs past `-timeout`
seconds and reports it as a finding (exit 70), as it does a crash. The limit is there so that an
input that makes the code under test hang, or do work that grows faster than its input, as the
dictionary decoder's repeat check once did (B-1 in `docs/REVIEW.md`), is a finding and not just
a slow run; libFuzzer's own default is 1,200 s. The 10 s came in with the first run commands at
`3aaad2f`, and until now no reason for it was written down. A timeout means something only while
the oracle's own work on one valid input stays well inside the limit. At `ede0176` it did not:
the Linux replay of `fuzz_dictionary` stopped at 11 s or more on a valid seed, one maximal block
whose values the oracle re-encoded in blocks of 3, which took 2.1 s an execution on the Mac. That
was the oracle, not the decoder.

**What was unbounded.** One input of a codec target held up to 528 maximal blocks of 2^20 values
in 16,384 bytes, and the oracle decoded every value about five times and re-encoded it at every
block size with every encoder: a PFOR block of 2^20 zeros, 5 KB, took 10.2 s an execution on the
Mac, and five 31-byte dictionary blocks 10.1 s. A store file's raw records inflate through zstd
to 65,535 bytes each, held as 64-bit values, so 759 bytes stated 4,194,240 values a segment, each
decoded five times and encoded twice. The query target's 131,072 bytes held 26 million values in
400 raw records, and a day of 18,342 typed records with a raw one every 64 took 2.4 s, where every
request decodes every segment it reaches on both paths.

**The bar.** One execution of any valid input must stay under 5 s, half of `-timeout`, on the
Linux host with its sibling cpu busy. Each bound is set on the Mac: the dearest input it admits,
a named seed (below), is predicted at no more than 4.4 s there, its Mac median times the largest
factor measured for its target (the Linux worst of five under the worst placement over the Mac
median: dictionary 6.04, run-length 7.19, PFOR 7.71, store 7.94, query 7.36). The 0.6 s under
the bar covers the 1.13 swing measured between sessions. So the Mac medians are held under 728,
612, 571, 554 and 598 ms. Where the least constant a bound's form allows (a whole number of
maximal blocks, a power of two) lands a target within about 3% over its Mac limit, it is taken
and said so: run-length is the one, its dearest seed at 617 ms, 0.8% over.

**The bounds.** Five targets pass bounds to their oracles; the unit suite calls the same oracles
with none (`FuzzRegression/BoundSeeds.PassTheWholeCheck` and every other test). Each constant
is in `fuzz/codec_oracles.hpp`, `itch_store_oracles.hpp` or `itch_query_oracles.hpp`, with the
figures it was set from beside it.

| | `fuzz_dictionary` | `fuzz_run_length` | `fuzz_pfor` |
|---|---|---|---|
| Cap: whole check while twice the values before a block plus its count fit (`decoded_values`) | 262,144 (2^18) | 1,048,576 (2^20) | 1,048,576 |
| Compare limit, from the input's start (`compared_values`) | 1,310,720 (cap + one maximal block) | 2,097,152 (cap + one) | 2,097,152 (cap + one) |
| Decode limit (`single_decoded_values`) | the compare limit | the compare limit | 3,145,728 (one more) |
| Budget (`cross_checked_values`) | 5,123 (one block) | 17,410 (four blocks) | 17,410 (four blocks) |
| Prefix past the budget (`past_budget_blocks` x `kPastBudgetValues`) | 64 blocks x 64 values | 64 x 64 | 64 x 64 |
| Dearest input, Mac median (worst of five) | 685 ms (692) against 728 | 617 (619) against 612 | 564 (570) against 571 |

The dictionary keeps a one-block budget: two blocks took its dearest input to 855 ms and four to
1,138, past its 728.

What each block gets, read in order from the input's start:

- **Within the cap, never budgeted:** the decode by the chosen kernel set and by the scalar set
  and their agreement, the decode from an exact copy, the refusal checks (a refusal repeats and
  leaves the output alone), the cut check, the decoder-side block check (a dictionary's entry
  count, a run-length block's run count) and the column's exact-count and wrong-count checks.
- **Within the cap, budgeted:** each block's re-encodes with every encoder, decoded back, and its
  codec's encoder-side check, on all its values while the budget lasts and on the part of the
  block it reaches. Then the **prefix**: each of the next 64 blocks within the cap gets the same
  on its first 64 values (all of them in a shorter block). A block within the cap past the
  budget and past those 64 gets the never-budgeted checks only. The column re-encodes at block
  sizes 3, 64 and 4,096 take the first budget's values.
- **Past the cap, up to the compare limit:** the block is decoded by the scalar set in place and
  by the chosen set from an exact copy, the two must agree, refusal or values, and a refused
  block must leave the output alone. Nothing else.
- **Past that, up to the decode limit (PFOR only):** decoded once, in place, with the chosen
  kernel set. **Past the decode limit:** not examined.
- Part 2, the input's bytes as values (2,048 words at -max_len), is unchanged and unbounded.

The store target (`kStoreFuzzBounds`): a file whose segments state at most 1,048,576 values in all
takes every check; one stating up to 4,194,304 is read whole by the chosen kernel set and with the
scalar set forced and the two reads compared; one stating more is opened and nothing more.

The query target (`kQueryFuzzBounds`) is bounded in work, not values: its measure is the work a
file's directory says each part of the check will take, in ns on the Mac (`query_work`, each
segment, piece and value weighted by what it was fitted to cost). Up to 450M in the whole-check
measure (`kWholeCheckWork`) the file takes the whole check, the cap (`work`); the repeat ask takes
every request up to 365M, the budget (`cross_checked_work`), and two past it; past the cap, up to
450M in the compare measure (`kComparedAsksWork`, which gives a plain segment no weight), the
compare limit (`compared_work`), the indexed path is asked every request by both kernel sets and
the answers compared; past that the file is opened and not queried. The value counts below are a
ceiling derived from the measure, every value taken at the cheapest weight, not a bound of their own.

On the public page this reads: **fuzzing compares the SIMD kernels with the scalar ones on every
codec block that ends within an input's compare limit, 1,310,720 values from its start for the
dictionary and 2,097,152 for run-length and PFOR, in blocks of up to 1,048,576 values, the
format's largest. The blocks within the cap, where twice the values before a block plus its own
count stay within 262,144 values (dictionary) or 1,048,576 (run-length, PFOR), also get the
refusal, cut, block and column checks; the re-encode checks take the first 5,123 values
(dictionary) or 17,410 (run-length, PFOR) and the first 64 values of each of the next 64 blocks.
Past the compare limit PFOR decodes each block that ends within 3,145,728 values once, in place,
with the chosen kernel set, and nothing more is examined. The store writes codec blocks of 4,096 values, which every
codec target takes whole as an input's first block.**

**The count table.** For each bounded target, every loop the oracle runs, or runs in what it
calls, whose trip count depends on the input, with the constant that limits it; then the most
values one execution can decode, encode and compare, and the most parts it examines, each as an
expression in the constants and as a number; then the named seed that reaches each, or the share a
seed reaches and why. How each count is taken is in `fuzz/oracle_counts.hpp`. A decode counts what
it can decode: a block the count its header states where the header and the payload it states are
all there, else none, since every block decoder refuses a cut header or payload before decoding
anything; a column the values of the blocks the column walk decodes before it stops, since
`decode_column` refuses a block that would pass the count asked before decoding it. An encode
counts its values; compared counts the values the scalar set decodes for a comparison; blocks every
codec block part 1 examines, in any tier; segments every store segment read. The oracles count as
they work, and `FuzzOracles.TheCountTableHoldsTheConstants` holds each most below to the constants,
`FuzzRegression/BoundSeeds.CountWhatTheTableGivesThem` each named seed's counts to the figures in
`kSeedCounts` and to its target's most.

*The loops, codec targets.* Letters: C cap, L compare limit, S decode limit, B budget, N = 64
prefix blocks, P = 64 prefix values, E encoders (dictionary 2, run-length 1, PFOR 16), Eb the
encoders a block is re-encoded with (2, 1, 4: a PFOR block's own transform), X the encoder-side
check's encodes (2, 1, 0), K = 3 oracle block sizes, W = 2,048 part 2 words (16,384 bytes / 8),
M = 16,384 bytes (-max_len), m the least valid block (the 8-byte header and the least payload
a valid block has: dictionary 30 bytes, where its header checks pass at 29 but a valid block needs
the Huffman stream's 8-bit longest-code field; run-length 44; PFOR 16).

| Loop | Limited by |
|---|---|
| part 1's walk over blocks, within the cap and past it | each block it takes but a refused one, which ends the walk, holds m bytes or more, so at most ceil(M / m) blocks: 547, 373, 1,024; the walk also stops past S (L where there is no decode-once tier) |
| each block's decodes, re-encodes and checks | the block's stated count, at most kMaxBlockValues = 2^20, and the tiers above: within the cap 2 x before + count <= C; past it before + count <= L or S |
| the re-encodes' values | B in all, then P values in each of at most N blocks |
| the encoders a block is re-encoded with; the column re-encodes | Eb; E x K, each on at most B values |
| distinct_count's sort, maximal_runs (decoder-side checks) | the block's values |
| part 2's words and its encoders | W; E |
| inside every block decoder and encoder (`block.cpp`, `pfor.cpp`, `dictionary.cpp`, `run_length.cpp`, `patch_list.cpp`, `lane_pack.cpp`, `int_transform.hpp`, `bit_stream.hpp`, `byte_cursor.hpp`, `simd/`) | a block's values, entries, runs, patches or bytes, each at most its stated count (a dictionary's entries and a run-length block's runs are refused past its count: `dictionary.cpp`, "dict entries"; `run_length.cpp`, "rle runs") or its bytes; fixed widths otherwise (8 lanes, 8 bytes, 64 bits, 256 table entries, the 4 head-delta heads, the PFOR widths and at most `kCommonDivisors` scale candidates, the dictionary's 3 modes); the move-to-front decode moves at most `kDictMtfMaxEntries` - 1 = 2,047 entries a value; the dictionary's repeat scan stops at `kRepeatStepsPerEntry` x entries + 16 probes; a Huffman code read takes at most the 64 bits of one word |
| `decode_column`'s walk | refuses a block that would pass the count before decoding it, so at most the count's values |

*The block term.* Part of a block's cost does not grow with its values, so an input of many small
blocks is bounded by the block count above, not the value counts: every Huffman dictionary decode
rebuilds its tables of 2^10 and 2^11 slots (`kHuffmanTableBits`, `kHuffmanPairBits`), every PFOR
encode starts a fresh encoder `Scratch`, every dictionary check sorts the block's values for
`distinct_count`, and a block's decode nests the decodes of its parts, up to 2 for a dictionary
block (its entries and its indices) and 3 for a run-length block (run values, run lengths, the
runs expanded). The move-to-front factor, up to 2,047 entries moved a value, is a value term and
is reached by `dictionary/bound-costliest`. The named seeds `bound-most-blocks` reach the block
term: each holds as many one-value blocks as -max_len holds, every one within the cap and the
budget, so each takes the whole of part 1 and every re-encode.

*The loops, store target.* The reader's and writer's loops run over a file's container sections
and blocks (each entry fixed-size, so at most -max_len / entry size), its segments, columns
(`kColumnCount`), pieces, values (each column decoded only at its stated count, the sum at most
the cap or the compare limit), records (at most `kFuzzMaxSegmentRecords` = 64 a segment, each raw
record at most 65,535 bytes), locates (at most 65,536, a bitmap of 8,192 bytes), symbols (at most
the section's bytes / entry size) and open-addressing hash probes (at most the table, grown by
load). The oracle's own: five reads and two writes of the file, the segments in each.

*The segment term, store target.* Each rewrite finishes every segment it writes, and finishing one
zeroes a counting array of 65,538 entries (`SegmentBuilder::finish`, `itch_segment.cpp`), whatever
the segment holds. A segment holds a record or more, so it has a type block, whose metadata is at
least the 63-byte segment summary, a seek block and one more (raw_length for a raw record; locate,
timestamp and tracking for a typed one), each a 32-byte footer entry, the two others' metadata 12
bytes each: 183 bytes. So 70,000 bytes hold at most G = 382 segments (`kStoreMostSegments`), read
five times, 1,910 segment reads, and written twice, 764 segments finished. The writer's smallest
segment is one one-byte raw record in 317 bytes, so the most a written file holds is 219, which
`itch_store/bound-most-segments` is; a hand-made file of smaller payloads, up to 382, is not built.

*The loops, query target.* As the store's, with the segment cap `kQueryFuzzMaxSegmentRecords`; and
the requests, at most 16 (4 windows x 4 locates); for each, one call for rows and one for the
minute aggregate or the locate counts, and one share-sum call an ask; each call decodes each
segment at most once (the time before a segment is kept, `engine.last_ts`), so at most the file's
values T; the minute aggregate's bucket moves, at most min(typed records, `kMostMinutes` = 4,692)
squared a call, counted in the work measure; the column form's 65,536 locate counters a request.
Every one of these is a term of the work measure, so the measure, not T, is the query's bound. T is
held by it: every value costs at least the plain segment's weight, so T is at most (work limit -
one plain segment's weight) / that weight.

*The most one execution can decode, encode and compare, and examine.*

| Target | Decoded | Encoded | Compared | Parts |
|---|---|---|---|---|
| dictionary | 5C + 2(L - C) + (S - L) + (Eb + E K) B + 2 E W = **3,457,048** | (2 Eb + X)(B + N P) + E K B + E W = **90,148** | L + E W = **1,314,816** | ceil(M / m) = **547** blocks |
| run-length | the same = **7,413,768** | the same = **118,796** | the same = **2,099,200** | **373** blocks |
| PFOR | the same = **9,359,464** | the same = **1,040,496** | the same = **2,129,920** | **1,024** blocks |
| store | max(5 Cs, 2 Ls) = 8 x 2^20 = **8,388,608** | 2 Cs = **2,097,152** | Ls = **4,194,304** | 5 G = **1,910** segment reads, 2 G = 764 written |
| query | max(133 Tb, 105 Tc, 66 Tx) = 66 x 225,000 = **14,850,000**, a ceiling | **0** | 33 max(Tc, Tx) = **7,425,000**, a ceiling | the work measure: cap 450M, budget 365M, compare limit 450M |

Why: within the cap a value is decoded five times at the most, by the chosen set, the scalar set
and the exact copy, and by the column decoder at its count and one more; the cut check and a
one-block column asked for one fewer decode nothing, the decoders refusing them first. A value
the re-encodes take counts twice in the cap's measure and is decoded six times, so a such values
leave 5C - 4a, and the re-encodes' own decodes are taken in full beside the 5C: B values once by
each block encoder and at each oracle block size with every encoder. The prefix's decodes, at most
N P values by each block encoder, are fewer than the 4a the budget's values leave. Each re-encoded
value is encoded twice by each block encoder (none of these codecs is canonical, so the same bytes
are asked twice) and X times by the codec's check; past the cap each compared value is decoded
twice and each decoded-once value once; part 2 encodes W words with every encoder and decodes them
twice. The store reads a file within its cap Cs five times and writes it twice, and past it, up to
Ls, reads it twice. For the query, Tb = 39,910, Tc = 49,460 and Tx = 225,000 are the value ceilings
the budget, the cap and the compare limit give at the cheapest weight; an ask is 33 calls; within
the budget four asks and the column form's read (133 T), past it the repeat ask's 5 calls (105 T),
past the cap two asks (66 T); the query writes nothing.

*What the named seeds reach* (decoded / encoded / compared, then blocks or segments):

| Seed | Counts | Share of the most, and why |
|---|---|---|
| `dictionary/bound-costliest` | 3,457,048 / 65,572 / 1,314,816, 3 blocks | decoded and compared all; encoded short by 24,576, its one block within the cap leaving the prefix unreached |
| `dictionary/bound-prefix` | 816,143 / 90,148 / 257,021, 67 blocks | encoded all |
| `dictionary/bound-most-blocks` | 14,187 / 10,636 / 4,641, 546 blocks | 99.8% of the blocks: 545 blocks of the least, 30 bytes, and the tail's header, where the most is 546 valid blocks and a refused one in the 4 bytes they leave |
| `run_length/bound-costliest` | 7,413,768 / 106,508 / 2,099,200, 3 blocks | decoded and compared all; encoded short by 12,288 (the prefix) |
| `run_length/bound-prefix` | 3,159,042 / 118,796 / 1,029,118, 67 blocks | encoded all |
| `run_length/bound-most-blocks` | 6,700 / 4,280 / 2,420, 373 blocks | all the blocks |
| `pfor/bound-costliest` | 8,957,032 / 1,007,728 / 2,129,920, 4 blocks | 96% of the decoded: its decoded-once block holds the 646,144 values the bytes its dear blocks leave hold, not 2^20; compared all; encoded short by 32,768 (the prefix) |
| `pfor/bound-decode-limit` | 9,359,464 / 1,007,728 / 2,129,920, 4 blocks | decoded and compared all: three maximal blocks of zeros, 5,131 bytes each |
| `pfor/bound-prefix` | 4,068,450 / 1,040,496 / 1,059,838, 67 blocks | encoded all |
| `pfor/bound-most-blocks` | 121,801 / 90,056 / 33,791, 1,024 blocks | all the blocks |
| `itch_store/bound-costliest` | 5,242,735 / 2,097,094 / 1,048,547, 305 segment reads | 29 values short of the cap's: raw bytes come in whole records |
| `itch_store/bound-costliest-compared` | 8,388,550 / 0 / 4,194,275, 122 segment reads | 29 short of the compare limit's |
| `itch_store/bound-most-segments` | 76,650 / 30,660 / 15,330, 1,095 segment reads | 57% of the segment reads: 219 segments, the writer's most (above), of the format's 382 |
| `itch_query/bound-compared-plain-values` | 4,701,758 / 0 / 2,350,879 | 99.998% of the compare limit in its measure; 177,738 values, 79% of Tx, which takes every value at the cheapest weight |
| `itch_query/bound-compared-plain-segments` | 291,158 / 0 / 145,579 | 118 one-record plain segments, as many as -max_len holds; 31% of the compare limit |
| `itch_query/bound-falling-minutes` | 2,230,500 / 0 / 615,405 | 15% of the decoded ceiling: the dearest query files are dear in segments, blocks and minute moves, not values |

The other named seeds' counts are in `fuzz_regression_test.cpp` (`kSeedCounts`); each is within
its target's most.

**The named seeds.** Each is built in `fuzz/bound_seeds.hpp` from the bound constants, the real
encoders and the real writer, written by `generate_seeds` into its target's seeds under the name
in `fuzz/bound_seeds.txt`, so every replay (`check.sh`, CI, G-3) shows the bound again; the unit
suite runs each through its oracle with no bounds and holds it to the tiers it was built for.
Times: one execution each on the Mac, fuzz build, `-detect_leaks=0`, median and worst of five
interleaved rounds over every seed, each run started with load1 under 8, on the tree this section
was written with; Linux is the median times its target's factor, a prediction (none of these
seeds has been timed there).

These times and the dearest-input row above came in with `0dbdb95`, and the every-target table
below with `126b03f`, measured on the same fuzz targets' code; none is from the judged run. That
code is the same at `8616263`, the commit G-3 judges: its later changes to `fuzz/` are to this
file and to `bound_seeds.hpp`'s builders and comments. The judged seed gate ran on the Mac at
`8616263`, with the fuzz build and the leak check on: five runs of each of the 31 named seeds,
each run's time divided by libFuzzer's count of executions (two where the leak check reran the
input, one for the two Gorilla seeds), against 5,000 ms an execution, half of `-timeout=10`. All
31 passed. The worst was `dictionary/bound-one-value-blocks` at 682.0 ms (median 681.0), and each
seed's median was from 3.1% below its median above (`itch_query/bound-costliest-compared`, 386.5
against 399) to 0.9% above it. No results file has a kind for the gate's table, so it is kept
with the run's records; the Linux host's gate, the bar these bounds were set for, is not
collected yet.

| Seed | Shape | Mac median (worst) | Linux predicted |
|---|---|---|---|
| `dictionary/bound-one-value-blocks` | the dearest: 300 one-value blocks, as many as -max_len holds, then a move-to-front block of 2,048 entries, every rank the last, of the cap less 600 values, then a maximal one, compared | 685 (692) | 4,137 |
| `dictionary/bound-small-blocks` | the first block shortened by 189 blocks of 64 values, all compared | 670 (687) | 4,047 |
| `dictionary/bound-costliest` | the cap's move-to-front block, then a maximal one, compared | 667 (676) | 4,029 |
| `dictionary/bound-prefix` | the budget's block, the prefix's 64 blocks, the cap's rest | 373 (375) | 2,253 |
| `dictionary/bound-most-blocks` | 545 one-value blocks | 95 (96) | 574 |
| `run_length/bound-costliest` | 2^20 runs of one value (11,302 B), then a maximal block of 467,968 runs, compared | 617 (619) | 4,436 |
| `run_length/bound-small-blocks` | the first block shortened by one block of 64 runs | 612 (621) | 4,400 |
| `run_length/bound-one-run-blocks` | the dearest block, then a one-run maximal block compared and one not examined | 551 (554) | 3,962 |
| `run_length/bound-prefix`, `bound-repeated-run-value`, `bound-most-blocks` | the prefix; a refused first block; 372 one-value blocks | 61 (62), 68 (70), 34 (35) | 439, 489, 244 |
| `pfor/bound-costliest` | 7 i^2 under delta of delta with the scale (2^20), a maximal head-delta block compared, a head-delta block of 646,144 values decoded once | 564 (570) | 4,348 |
| `pfor/bound-small-blocks` | the first block shortened by 128 blocks of 64 values | 553 (558) | 4,264 |
| `pfor/bound-prefix` | the prefix | 477 (480) | 3,678 |
| `pfor/bound-delta-past-cap`, `bound-refused-past-cap` | a maximal delta block compared past the cap; a refused one | 283 (286), 283 (285) | 2,182, 2,182 |
| `pfor/bound-decode-limit` | three maximal blocks of zeros: within the cap, compared, decoded once | 276 (279) | 2,128 |
| `pfor/bound-most-blocks` | 1,023 one-value blocks, the smallest any of the 16 encoders writes | 100 (105) | 771 |
| `itch_store/bound-costliest` | 3,840 add orders beside 64 raw records, 1,048,547 values | 514 (515) | 4,081 |
| `itch_store/bound-costliest-compared` | the cap's add orders beside raw bytes to the compare limit | 461 (468) | 3,660 |
| `itch_store/bound-most-segments` | 219 segments of one one-byte raw record | 232 (235) | 1,842 |
| `itch_query/bound-costliest-within-budget`, `bound-costliest`, `bound-costliest-compared`, `bound-move-to-front`, `bound-falling-minutes` | the dearest found within the budget, past it, past the cap, with move-to-front shares, with falling minutes | 423 (434), 414 (423), 399 (409), 367 (373), 278 (282) | 3,113 at most |
| `itch_query/bound-compared-plain-values`, `bound-compared-plain-segments` | add orders on locate 0 in plain segments of the segment cap, to the compare limit; in plain segments of one record, to -max_len | 199 (200), 74 (76) | 1,465, 545 |
| `stream_vbyte`, `gorilla_timestamps`, `gorilla_xor` (unbounded) | the largest block of the cheapest value | 111 (112), 341 (342), 379 (382) | |

The block and segment terms are cheap beside the value terms: the most-blocks seeds take 95, 34 and
100 ms and the most-segments seed 232, each well under its target's dearest. The query's plain
seeds, the shapes its compare measure prices least, take 199 and 74 ms, under every other query
seed, so the measure misses no term they reach.

Of the shapes searched beside them at these bounds (`codec_oracles.hpp` gives each), none was
dearer than its target's named seeds: one-value blocks before the cap's block (PFOR 213, 560 ms;
run-length 109, 557), small blocks after it (dictionary 189, 670; PFOR 128, 555), mixes of the
two, and for run-length many-run blocks past the cap.

**What the fuzz targets leave unexamined, in figures.** The unit suite checks all of it on the
named seeds and on everything else it runs.

- A codec input past its compare limit, 1,310,720 values from its start for the dictionary and
  2,097,152 for run-length, and past PFOR's decode limit, 3,145,728: not examined. Between PFOR's
  compare and decode limits: decoded once, in place, with the chosen kernel set, not compared.
  Past each cap (262,144; 1,048,576; 1,048,576 values) up to the compare limits: only the kernel
  comparison, under the sanitizers as every decode is; the unit
  suite's fixed grid (`Grid/OracleGrid.PassTheWholeCheck`) runs every check on one dictionary
  block of each size from just past the cap to the largest, in every mode. -max_len
  holds at most three maximal PFOR blocks and two maximal dictionary blocks beside the cap's, so
  a PFOR input can reach its decode limit and a dictionary input its compare limit; a run-length
  input holds about 100 one-run maximal blocks, of which the one after the cap is compared.
- Within the cap: re-encodes and the encoder-side check skip every value past the budget's 5,123
  (dictionary) or 17,410 (run-length, PFOR) and the prefix's 64 x 64 = 4,096, and the column
  re-encodes every value past the budget's. An encoder fault on such a value passes the fuzz
  targets and fails `BoundSeeds.PassTheWholeCheck`.
- Store files stating more than 4,194,304 values are only opened; between 1,048,576 and 4,194,304
  only read and compared.
- Query files past 450M of work in either measure are only opened; past the cap only asked and
  compared; past the budget, 365M, the repeat ask takes 2 of the 16 requests.

**Which steps run the grid and the named seeds' tests.** The fixed grid (`Grid/OracleGrid.*`,
144 tests), the named seeds' tests (`FuzzRegression/BoundSeeds.*`, 62) and the named seeds'
scalar-capped parts (`simd_capped_to_scalar_bound_seed*`, 7) run in full in `check.sh`'s
`dev-test` and `asan-test` steps, CI's unit and asan-ubsan jobs, on each host. The `tsan-test`
step leaves all three out (`TSAN_LEAVES_OUT` in `check.sh`), and `scripts/coverage.sh`, which runs
the unit binary once rather than through ctest, leaves out the first two; both still run
`FuzzOracles.*`, the rest of `FuzzRegression` and every other test. These tests are
single-threaded re-runs of the fuzz oracles on fixed inputs, each already run in full under the
plain build and under AddressSanitizer and UBSan, and they are what took the Linux tsan and
coverage jobs from 16 and 9 minutes to over 55. On the Mac dev build, one ctest entry at a time on
a loaded machine, 137 of the grid's tests took 3,373 s and the 62 seed tests 757 s (the run was
stopped before the grid's last 7 and the scalar-capped parts).

**Every target.** What bounds one execution's time and memory at its `-max_len`, with
`-rss_limit_mb=2048` and `-malloc_limit_mb=256` over all of them, and the slowest input of each
target's saved corpus and seeds, each run once (`-detect_leaks=0`) on the Mac on the tree of the
named seeds' times above:

| Target | -max_len | What bounds one execution | Inputs run | Slowest, ms (input) |
|---|---|---|---|---|
| `fuzz_columns` | 70,000 | the input's bytes | 445 | 1 (`columns/seed-00000`) |
| `fuzz_container` | 70,000 | the input's bytes | 779 | 0 (a corpus file) |
| `fuzz_dictionary` | 16,384 | the bounds above | 3,156 | 695 (`dictionary/bound-one-value-blocks`) |
| `fuzz_gorilla_timestamps` | 70,000 | the input's bytes: the largest block of the cheapest value | 1,411 | 347 (`gorilla_timestamps/bound-costliest`) |
| `fuzz_gorilla_xor` | 70,000 | the same | 2,085 | 384 (`gorilla_xor/bound-costliest`) |
| `fuzz_itch_frame` | 70,000 | the input's bytes | 5,409 | 1 (a corpus file) |
| `fuzz_itch_message` | 256 | the input's bytes | 1,140 | 0 (a corpus file) |
| `fuzz_itch_query` | 131,072 | the bounds above | 2,510 | 428 (`itch_query/bound-costliest-within-budget`) |
| `fuzz_itch_store` | 70,000 | the bounds above | 2,431 | 521 (`itch_store/bound-costliest`) |
| `fuzz_pfor` | 16,384 | the bounds above | 5,569 | 566 (`pfor/bound-costliest`) |
| `fuzz_run_length` | 16,384 | the bounds above | 2,532 | 635 (`run_length/bound-small-blocks`) |
| `fuzz_stream_vbyte` | 70,000 | the input's bytes: the largest block of the cheapest value | 1,951 | 113 (`stream_vbyte/bound-costliest`) |
| `fuzz_stub_store` | 70,000 | the input's bytes | 933 | 1 (a corpus file) |

Every input exited 0. Each bounded target's slowest input is one of its named seeds; a single
run is noisier than the medians above (`run_length/bound-small-blocks` took 635 ms here against
its median of 612).

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
