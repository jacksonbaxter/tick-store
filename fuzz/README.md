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

**The bounds.** The three codec targets whose blocks hold 2^20 values in a few bytes, the store
target and the query target pass bounds to their oracles; the unit suite calls the same oracles
with none (`FuzzRegression/BoundSeeds.PassTheWholeCheck` and every other test). Numbers are
`fuzz/codec_oracles.hpp`, `itch_store_oracles.hpp` and `itch_query_oracles.hpp`, with their
reasons beside them.

- `fuzz_dictionary`, `fuzz_pfor`, `fuzz_run_length` (`kFuzzBounds`):
  - **Cap: 1,048,576 values an input** (`kFuzzDecodedValues`, one largest block). Part 1 takes a
    block only while the values taken and the count its header states stay within it. That block
    and everything after it are not examined, as if the input ended before it, and the column
    checks run on the blocks taken. So every block the format allows is still taken whole as an
    input's first. Why one block: what part 1 never budgets costs about five decodes of every
    value taken, and the dearest block, the dictionary's (below), takes about 0.65 s in those
    decodes alone on the Mac, so a second would pass what the costliest input may take.
  - **Budget: 8,192 values** (`kFuzzCrossCheckedValues`, two blocks of the largest oracle block
    size, 4,096). Each block's re-encodes with every encoder and its codec's extra check run on
    all its values while the budget lasts, on what is left of it where it runs out, and on the
    first 64 values (`kPastBudgetValues`) of every block after; the column re-encodes at block
    sizes 3, 64 and 4,096 run on the first 8,192 values taken. Why two: the fewest that give
    each column re-encode a block boundary and a block repeating the one before at every size,
    and reach eight full vectors of the packed codecs; each 4,096 more costs the dictionary's
    costliest input about 0.14 s on the Mac.
  - **Never budgeted**, on every block taken: the decode by the chosen kernel set and by the
    scalar set and their agreement, the decode from an exact copy, the refusal checks (a
    refusal repeats and leaves the output alone), the cut check, and the column's exact-count
    and wrong-count checks. Part 2, the input's bytes as values, is bounded by `-max_len`
    (2,048 values) and unchanged.
- `fuzz_itch_store` (`kStoreFuzzBounds`): **cap: 1,048,576 values stated** over every segment,
  column and piece (`SegmentEntry::values`), beside the 64-record segment cap it had. The reader
  decodes a column only when its stated count is the one its records need, and to exactly that
  many values, so the sum bounds every decode and re-encode the check makes. A file stating more
  is opened with CRCs skipped, with the scalar set and with CRCs recomputed, the three verdicts
  compared, and nothing decoded. No budget: within the cap every check runs.
- `fuzz_itch_query` (`kQueryFuzzBounds`): **cap: 135,168 values stated** (33 column blocks: the
  two-piece seed's 134,341, rounded up); a file stating more is opened with CRCs recomputed and
  not queried. **Budget: 69,632 values** (17 column blocks: the 9,000-record day seeds' 66,874
  at most, rounded up). Within it, all 16 requests take every check. Past it, the indexed path's
  second ask and its ask with the scalar set forced take the first two requests
  (`kQueryPastBudgetRequests`: the whole file with no locate and at locate 0, so rows, locate
  counts, minute aggregates and the share sum each meet both); the first indexed ask, the exact
  path, the column form and every comparison between them still take all 16.

**The costliest inputs.** Each is built in `fuzz/bound_seeds.hpp` from the real encoders and
writer, written by `generate_seeds` into its target's seeds under the name in
`fuzz/bound_seeds.txt`, so every replay (`check.sh`, CI, G-3) shows the bound again, and the unit
suite runs each through its oracle with no bounds and holds it to the side of the bounds it was
built for. Times are one execution each on the Mac, fuzz build, `-detect_leaks=0` so libFuzzer
runs the input once, and peak RSS from libFuzzer's final stats:

| Seed | How it was found | Before | After |
|---|---|---|---|
| `dictionary/bound-costliest` | the dearest block to decode in the fewest bytes: 2^20 values cycling down through 2,048, a sorted move-to-front block of 2,048 entries whose every rank is the last, 2,047 moves a value (7,342 B); then 2^20 equal values (31 B), past the cap; then bytes for part 2 at blocks of one value, its dearest | timeout | 969 ms, 144 MB |
| `run_length/bound-costliest` | two blocks of one run of 2^20 values (48 B each), then part 2's bytes | 1,132 ms | 48 ms |
| `pfor/bound-costliest` | of the shapes timed at width 0, the dearest: 2^20 values rising by 7 under head delta and a scale (6,159 B), twice, then part 2's bytes | timeout | 319 ms |
| `pfor/bound-maximal-delta` | not the costliest: a maximal delta block, so the delta kernel sums past the budget | timeout | 216 ms |
| `stream_vbyte/bound-costliest` | one byte and two control bits a value at least: the largest block of one small value in 70,000 B, 55,989 values, without delta, dearer than with | 111 ms | 111 ms |
| `gorilla_timestamps/bound-costliest` | one bit a value at least: the largest block of a regular clock, 559,856 values | 339 ms | 343 ms |
| `gorilla_xor/bound-costliest` | the largest block of one price repeated, 559,873 values | 377 ms | 385 ms |
| `itch_store/bound-costliest` | typed records cost the most a value (about 4.4 µs) but 70,000 B hold about 36,000 of them; raw bytes cost about 0.37 µs and reach the cap in under 700 B. Dearest of the mixes timed: 3,840 add orders in 60 segments of 64, then 64 raw records of 15,831 B, 1,048,419 values in 68,990 B | 517 ms | 519 ms, 263 MB |
| `itch_query/bound-costliest` | typed records outside plain segments cost the most a value: the day of 18,342 records with a raw one every 64, one segment, 135,165 values in 129,649 B, past the budget | 2,390 ms | 1,452 ms |
| `itch_query/bound-costliest-within-budget` | the same day of 9,504 records with a raw one every 4,500, 69,632 values, the most within the budget | 1,289 ms | 1,291 ms |

The limit they are held to is half of `-timeout`, 5 s, on the Linux host with its sibling cpu
busy. That host is more than 5.2 times slower than the Mac on this work (the `ede0176` seed: 2.1 s
on the Mac, 11 s or more there), so an input must stay under about 0.96 s on the Mac. The
dictionary's is at 0.97 s, its floor the never-budgeted decodes of the move-to-front block, and
the query's at 1.45 s, its floor one indexed ask, one exact ask and the column form; neither has
been timed on the Linux host.

**What the bounds leave unchecked in the fuzz targets.** The unit suite checks all of it on the
named seeds and on everything else it runs.

- Codec inputs: nothing past the first 2^20 values of an input is examined at all, so a second
  maximal block is never decoded by a fuzz target. Past the first 8,192 values, the re-encodes
  and the extra check see 64 values a block and the column re-encodes none: an encoder fault that
  shows only on a value past those passes the fuzz targets (proven: an encoder change at value
  100,000 passes `fuzz_pfor`'s replay and fails `BoundSeeds.PassTheWholeCheck/pfor`). The
  dictionary's and run-length's extra check compare the input block's entry or run count only
  when the whole block is in the budget; otherwise they check the re-encoded block of the values
  taken. Byte equality with the input holds only for a whole block (only Gorilla is canonical,
  and it is not bounded).
- Store files stating more than 2^20 values, and query files stating more than 135,168, are only
  opened. Past the query budget, the repeat ask and the scalar ask cover two of the 16 requests.

**Every target.** What bounds one execution's time and memory at its `-max_len`, with
`-rss_limit_mb=2048` and `-malloc_limit_mb=256` over all of them, and the slowest input of each
target's saved corpus and seeds, each run once (`-detect_leaks=0`) on the Mac:

TABLE_HERE

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
