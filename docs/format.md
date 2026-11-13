# The tick-store container format, version 1

One file holds a store's blocks and named sections. The code is
`include/tick_store/store/container.hpp` (layout and parser), `container_writer.hpp` (commit and
durability) and `container_reader.hpp` (mapping). The golden file
`tests/unit/golden/container-v1.tsc` is this layout in 305 bytes, and `tests/unit/container_test.cpp`
types it out field by field.

```
[header 16][block and section bytes, in the order written][footer][trailer 24]
```

Every multi-byte field is an unsigned little-endian integer (F2). Offsets are from the start of the
file unless a row says otherwise. There is no padding and no alignment anywhere.

## Header, 16 bytes

These 16 bytes are the same in every version (F1).

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 8 | magic | `TICKSTOR` |
| 8 | 4 | format version | 1 |
| 12 | 4 | header size | 16 in version 1 |

## Data region

From the header's end to the footer, the blocks' payloads and the sections' bytes, back to back in
the order they were written. Each list, blocks and sections, is in file order in the footer, and
together they tile the region exactly: no gap, no overlap, no byte that nothing claims. An empty
section takes no bytes and shares its offset with whatever follows.

A block's payload is opaque to the container. For every codec but `raw` and `zstd` it is one or
more blocks in the codec's own frame (`codec/block.hpp`), back to back, each carrying its value
count. A `zstd` payload is an 8-byte header, then one zstd frame: a u16 inner codec id (readable,
neither `none` nor `zstd`), a u16 reserved as 0, a u32 inner size from 1 to the reader's limit, all
little-endian, then exactly one frame whose content size is the inner size and whose content is a
payload of the inner codec (`codec/zstd_frame.hpp`). The ITCH store's layout over this container
is in `docs/store.md`.

## Footer

| Offset in footer | Size | Field |
|---|---|---|
| 0 | 8 | block count B |
| 8 | 8 | section count S |
| 16 | 8 | metadata area size M |
| 24 | 32 B | block entries |
| 24 + 32 B | 64 S | section entries |
| 24 + 32 B + 64 S | M | metadata area |

The footer is exactly `24 + 32 B + 64 S + M` bytes, from the footer offset to the trailer.

### Block entry, 32 bytes

| Offset | Size | Field | Rule |
|---|---|---|---|
| 0 | 8 | payload offset | where the previous block or section ended; the first at 16 |
| 8 | 8 | metadata offset | from the metadata area's start; where the previous block's metadata ended |
| 16 | 4 | payload size | fits before the footer |
| 20 | 4 | metadata size | fits in the metadata area |
| 24 | 4 | CRC32C | over the codec id as 2 little-endian bytes, then the payload |
| 28 | 2 | codec id | readable in the registry (`codec/registry.hpp`) |
| 30 | 2 | reserved | 0 |

A block's metadata is the store's own bytes for it: a zone map, a row count. It lives in the footer,
so a scan of every block's metadata reads the footer alone and never a payload page. The metadata of
all blocks, in block order, tiles the metadata area exactly.

### Section entry, 64 bytes

| Offset | Size | Field | Rule |
|---|---|---|---|
| 0 | 8 | offset | where the previous block or section ended |
| 8 | 8 | size | fits before the footer |
| 16 | 4 | CRC32C | over the section's bytes |
| 20 | 1 | name length L | 1 to 43 |
| 21 | 43 | name | L bytes of `a-z 0-9 _ . -`, then zeros |

Names are unique. Sections hold whatever a store keeps once per file: indexes, dictionaries, book
checkpoints.

## Trailer, 24 bytes

The same in every version.

| Offset in trailer | Size | Field | Value |
|---|---|---|---|
| 0 | 8 | footer offset | 16 to file size - 48 |
| 8 | 4 | footer CRC32C | over the footer's bytes |
| 12 | 4 | reserved | 0 |
| 16 | 8 | magic | `ROTSKCIT`, the header magic backwards |

## Checks at open, in order

1. The file holds a header: at least 16 bytes. Then magic, version, header size (F1).
2. The file holds a header, an empty footer and a trailer: at least 64 bytes.
3. The trailer's magic. A file missing its last write has none, so this is the incomplete-file check (F7).
4. The trailer's reserved field, then the footer offset against the file size (M5).
5. The footer CRC, unless checksums are skipped.
6. B, S and M against the footer's size, each compared with the room left by division, so no product
   or sum can wrap (D7).
7. One walk over both entry lists in file order: each entry's fields, then that it starts where the
   last extent ended and fits before the footer. Then that the extents end at the footer and the
   metadata fills its area.
8. Section names unique.

Each failure is a typed `StoreError` naming the field, its offset, and the expected and actual values
(D2). `tests/unit/container_test.cpp` has a corrupt fixture for each check, asserting the exact error,
and cuts the golden file at every length.

A block's CRC is checked when the block is looked up, and a section's when it is, so opening a file
reads only its two ends; `verify_all()` checks every one. Skipping CRCs takes the named option
`Checksums::skip` (F3). The fuzzer uses it to reach the structural checks below the CRCs, and so may a
caller that has verified the file once already.

## Codec registry (F4)

`include/tick_store/codec/registry.hpp` is the one table of ids. An id is never reused. Its status is
`active` (written and read), `read_only` (read, never written: how a codec leaves service while files
that use it must stay readable) or `reserved` (never valid in a file, refused at open as an unknown
codec). A reserved entry stays in the table with its reason.

| Id | Name | Status |
|---|---|---|
| 0 | none | reserved: never assigned, so a zeroed entry names no codec |
| 1 | raw | active |
| 2 | gorilla_timestamps | active |
| 3 | gorilla_xor | active |
| 4 | pfor | active |
| 5 | stream_vbyte | active |
| 6 | dictionary | active |
| 7 | run_length | active |
| 8 | zstd | active: zstd level 1 around a payload of another codec, header above |

A new codec takes the next id and changes no format version. A reader older than the id refuses a
block that names it as an unknown codec, rather than misread it (D11). A change to an existing codec's
bytes is a new id, never an edit to the old one.

## Version policy (F5)

- The header's first 16 bytes and the 24-byte trailer never change, so any reader can find the
  version and the footer of any file.
- Any change to what a byte means, outside the codec registry, is a new format version. Writers write
  only `kContainerVersion`. Readers accept every version from `kOldestReadableVersion` to
  `kContainerVersion`, each through its own parse path, and refuse anything else as an unsupported
  version.
- Each version keeps a checked-in golden file, `tests/unit/golden/container-v<N>.tsc`, which the
  reader must open with every value, for as long as that version is readable. The writer test compares
  only against the current version's file.
- Raising `kOldestReadableVersion` drops a version. That is a decision recorded in
  `docs/NOTES.md`, and its golden file goes with it.

## Writing and durability (M1-M4, M8)

A file is written to `<path>.partial` and published by rename. The footer and trailer, the commit
record, go last. `Durability` names what a commit has made safe:

| Mode | Class | Before the rename | After the rename | A crash can leave |
|---|---|---|---|---|
| `page_cache` | A | nothing flushed | nothing flushed | the old file, the new one, no file, or a file open refuses |
| `device_flush` | B | macOS `fcntl(F_FULLFSYNC)`; Linux `fdatasync` | macOS `F_FULLFSYNC` on the directory; Linux `fsync` on it | the old file or the whole new one |

On macOS, `F_FULLFSYNC` falls back to `fsync` only where the file system answers `ENOTSUP`, `EINVAL`
or `ENOTTY`. Flush granularity is one flush per file, at commit. A failed flush is fatal: it is
returned once and never retried, the writer is failed, and before the rename the old file stays as it
was (M3). The system calls go through one seam, `FileOps`, so the tests record them and inject a
failure or a crash at each.

What the tests show and what they cannot. The call-sequence tests (`ContainerCommit` in
`container_writer_test.cpp`, `SyncFile` and `SyncDirectory` in `durability_test.cpp`) show each
mode makes its flushes, on the right kind of descriptor, in the order above. The crash tests
(`ContainerTornWrite`) end the process at each call, which models a process crash: the page cache
outlives the process, so every byte written before the crash survives whether it was flushed or
not. They fail if a flush call is missing, since two of their crash points are the flushes, but not
if a flush is made and does nothing: with every `PosixFileOps` flush made a no-op that returns 0,
all 166 store and durability tests pass. That a flush reaches the device, so class B survives power
loss, rests on the operating system's documented semantics of those calls; showing it would take a
power-cut test (dm-flakey on Linux, for one), which no unit test is (review C-9).

## SIGBUS stance (M7)

The reader maps the whole file read-only at its `fstat` size and proves every stored offset and size
against that length before reading through it (M5). A read past the end of the mapping therefore
cannot come from the file's contents. It can come only from the file shrinking under the mapping, or
from a device error under a mapped page, and either raises SIGBUS.

Nothing catches the signal. A handler cannot tell which read faulted or resume it safely, and code that
tried would turn a lost page into wrong answers. ClickHouse and QuestDB reached the same conclusion.
The process dies, and the cause is prevented instead:

- A file is published whole by rename and never written, truncated or extended after. A writer that
  replaces a file makes a new inode, and a reader's mapping keeps the old one alive until it unmaps.
- The store never truncates a file a reader may hold. A partial file is never opened by a reader.
- So only something outside the store, cutting a published file in place, can raise the signal. That
  is outside the contract, like overwriting the file.

`munmap` gets the length that was mapped, never one read from the file, and a failure aborts, since a
destructor has nowhere to report it (M9).

## Over-read contract (A2)

Nothing in the parser or the reader reads outside the file's bytes, and a view's span holds exactly a
payload, with no padding guaranteed after it. A decoder given a payload must stay inside it. The
CRC32C kernels read exactly their input too.

## Lifetimes (A3)

Views from `ContainerParser` borrow its input span. Views from `ContainerReader` hold a shared
reference to the mapping, so their spans stay valid after the reader is destroyed, and the mapping is
unmapped when its last holder goes.
