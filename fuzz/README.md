# Fuzzing

Eleven libFuzzer targets take raw bytes directly, with nothing checking them first:

| Target | Input | Oracle in `itch_oracles.hpp` |
|---|---|---|
| `fuzz_columns` | a whole column directory, as length-prefixed sections | `check_columns` in `columns_oracles.hpp`: each file sits in its own heap block, so ASan sees a read past its end. Errors repeat, every frame an accepted directory gives is one whole frame, re-exporting those frames rebuilds the same frames and tail, a cut of any file is refused, and a copy with one bit flipped is refused or gives frames that re-export cleanly |
| `fuzz_itch_message` | one payload | `check_message`: both decoders agree, decode then encode gives back the exact input, decoding that gives the same message, and a short buffer fails cleanly |
| `fuzz_itch_frame` | a byte stream | `check_stream`: frames are contiguous, every frame plus the reported tail rebuilds the input exactly, every stop is justified by the bytes, and a shorter input gives the same frames that fit |
| `fuzz_gorilla_timestamps` | a column of Gorilla delta-of-delta blocks | `check_timestamp_column` in `codec_oracles.hpp`: every block the decoder accepts re-encodes to exactly its bytes (the format has one encoding per sequence), decodes the same from an exact copy, and is refused once cut; refusals repeat and leave the output alone; a whole valid column decodes at its exact count and not one off; the accepted values re-encode and decode at three block sizes; and the input's bytes, read as 64-bit values, go through the encoder and back at a block size the input picks |
| `fuzz_gorilla_xor` | a column of Gorilla XOR blocks | `check_xor_column`: the same checks on the XOR codec, then the price-to-double mappings on every word: an inverse that accepts a double gives an integer that maps back to it, every Price(4) comes back through dollars, and every integer up to 2^53 through a double |
| `fuzz_pfor` | a column of patched frame of reference blocks | `check_pfor_column` in `codec_oracles.hpp`: the format leaves the encoder choices (scale, mode, width, base), so instead of bytes the oracle checks that every accepted block decodes the same from an exact copy and is refused once cut, refusals repeat and leave the output alone, the block's values re-encode with each of the four option sets of its transform to a block that decodes to them, and the same bytes twice; a whole valid column decodes at its exact count and not one off; the values re-encode and decode in all sixteen option sets at three block sizes; and the input's bytes, as 64-bit values, go through all sixteen and back at a block size the input picks, up to 97 or just past one 1,024-value vector |
| `fuzz_stream_vbyte` | a column of Stream VByte blocks | `check_svb_column`: the same checks with the encoder with and without delta, and one more: every field but zigzag has one valid value and the encoder takes zigzag only when strictly smaller, so re-encoding a block's values with its own delta flag gives exactly its bytes when the encoder makes the same zigzag choice, and otherwise a smaller block, or one the same size when the input took zigzag |
| `fuzz_dictionary` | a column of dictionary blocks | `check_dict_column`: the same checks as `fuzz_pfor` with the dictionary encoder, whose choices are the mode and the nested PFOR blocks', and one more: a block's entries are distinct and all used, so its entry count must be its count of distinct values, the encoder must write the same count, and it takes move-to-front only within its 2,048-entry limit |
| `fuzz_run_length` | a column of run-length blocks | `check_rle_column`: the same checks with the run-length encoder, whose choices are the nested PFOR blocks', and one more: runs are maximal, so a block's run count must be its count of maximal runs, and the encoder must write the same count |
| `fuzz_container` | a whole container file | `check_container` in `container_oracles.hpp`: the input goes in three ways, with CRCs verified, with `Checksums::skip`, and with every CRC its structure points at recomputed (`fixed_checksums`), so the bytes reach the structural checks below the CRCs (Z1). Skipping and recomputing must agree on every verdict, and errors repeat. An accepted file is rebuilt from its parsed values alone through `build_container`, the writer's encoding, and must equal the input byte for byte (the format has one encoding per file), or the input with its CRCs recomputed when they were skipped; every block's view must match its directory entry and every section must be found by its name at its own offset. Then a cut of it and a copy with one bit flipped must each be refused or pass the same checks in full. A cut is not required to fail: a file whose section holds a whole container in the right place leaves a valid file when cut there, so the unit suite checks the golden file at every length instead |
| `fuzz_stub_store` | a whole store file | `check_store` in `store_oracles.hpp`: errors repeat, an accepted file rebuilds the input exactly from its parsed values and holds the records its header counts, a cut of it is refused, and a copy with one bit flipped is refused or rebuilds to the flipped bytes |

## Build

Needs upstream clang: Apple clang has no fuzzer runtime. The build regenerates the
seeds from the real encoder and the real store writer into
`build/fuzz/fuzz-seeds/{message,frame,store,container,columns,gorilla_timestamps,gorilla_xor,pfor,stream_vbyte,dictionary,run_length}`.
They are never committed. The container seeds are files from the real `ContainerWriter`: empty,
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
not maximal, and lengths one short of and one past the count.

```sh
cmake --preset fuzz && cmake --build --preset fuzz
mkdir -p build/fuzz/corpus/itch_message build/fuzz/corpus/itch_frame \
    build/fuzz/corpus/stub_store build/fuzz/corpus/container build/fuzz/corpus/columns \
    build/fuzz/corpus/gorilla_timestamps build/fuzz/corpus/gorilla_xor \
    build/fuzz/corpus/pfor build/fuzz/corpus/stream_vbyte \
    build/fuzz/corpus/dictionary build/fuzz/corpus/run_length build/fuzz/artifacts
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

build/fuzz/fuzz/fuzz_container -max_total_time=60 -max_len=70000 \
    -rss_limit_mb=2048 -malloc_limit_mb=256 -timeout=10 -print_final_stats=1 \
    -artifact_prefix=build/fuzz/artifacts/ \
    build/fuzz/corpus/container build/fuzz/fuzz-seeds/container

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
`tests/unit/crashers/itch_message/`, `itch_frame/`, `stub_store/`, `container/`, `columns/`,
`gorilla_timestamps/`, `gorilla_xor/`, `pfor/`, `stream_vbyte/`, `dictionary/` or
`run_length/` and fix the bug in the same commit. `fuzz_regression_test.cpp` replays those files through the same
oracles in every test build.
