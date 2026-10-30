# Machine probe: what this host can and cannot measure

Status: finished. Q1-Q6 all answered; two requested measurements cannot be produced on this host (see Gaps at the end).
Host: Apple M4 Max, macOS, arm64, shared.
Raw outputs: research/machine-probe/raw/. Probe sources: research/machine-probe/src/.
Numbers are quoted only if taken under .orchestrator/measure.sh.

## Q1. Machine facts

Sources: raw/q1-machine-facts.txt (captured by the earlier run, read and kept), raw/q1-isaprobe.txt (re-captured: the earlier
file held only a compile error, so "Q1 done" was not true of it), raw/q3-man-pages-fcntl-full.txt (cluster map from ioreg at the top).

- CPU: Apple M4 Max. 16 cores: 12 P (hw.perflevel0) + 4 E (hw.perflevel1). ioreg cluster-type: cpu0-3 = E (cluster 0),
  cpu4-9 = P (cluster 1), cpu10-15 = P (cluster 2).
- Caches: P core L1i 192 KiB, L1d 128 KiB, L2 16 MiB shared per 6 cores. E core L1i 128 KiB, L1d 64 KiB, L2 4 MiB shared by 4.
  No hw.l3cachesize is reported. Cache line 128 bytes (hw.cachelinesize). Page 16 KiB. The unqualified hw.l1*/hw.l2* sysctls
  report the E-core values, so any code reading them gets the smaller numbers.
- Memory: 64 GiB (hw.memsize 68719476736).
- OS: macOS 27.2 build 26B123; Darwin 27.2.0, xnu-13432.41.4~25 RELEASE_ARM64_T6041.
- Disk: /dev/disk3s5 mounted at /System/Volumes/Data, APFS, case-insensitive, FileVault on, internal SSD, "Apple Fabric",
  926 GiB size, 822 GiB available at capture. Allocation block and device block both 4096 bytes.

| Feature | sysctl | Executed (isaprobe) |
|---|---|---|
| NEON / AdvSIMD | hw.optional.arm.AdvSIMD=1, neon=1 | yes |
| PMULL | FEAT_PMULL=1 | yes |
| SVE (non-streaming) | no FEAT_SVE key at all | SIGILL (rdvl) |
| SVE2 (non-streaming) | no FEAT_SVE2 key | SIGILL (eor3) |
| SVE2 BitPerm BDEP/BEXT, non-streaming | no key | SIGILL (bdep) |
| SME | FEAT_SME=1, FEAT_SME2=1, FEAT_SME2p1=0, sme_max_svl_b=64 | yes, rdsvl=64 |
| Streaming SVE (smstart sm) | follows from SME | yes, rdvl=64 (512-bit), eor3 ok |
| FEAT_SME_FA64 (full A64 in streaming) | no key | absent: compact in streaming mode SIGILLs |
| BDEP in streaming mode (SSVE BitPerm) | no key | SIGILL |
| FEAT_CSSC (GPR ctz/cnt/popcnt) | FEAT_CSSC=0 | not tested |

So: NEON yes. SVE and SVE2 absent as normal instructions. SME/SME2 present with 512-bit streaming vectors, but streaming mode
has no BDEP and no FA64, so NEON and any FA64-only SVE instruction cannot be mixed in. No bit-deposit instruction of any kind
exists on this CPU. There is no PDEP/PEXT equivalent; it has to be done with tables or scalar code.

## Q5. x86-64 coverage from this Mac

Command: `bash research/machine-probe/src/q5_x86.sh`. Raw: raw/q5-x86.txt. Not a timing, so not under measure.sh.

- Compile `--target=x86_64-apple-darwin -mavx2 -mbmi2` and `-mavx512f -mavx512vbmi2`: both exit 0. Disassembly confirms real
  ymm instructions (vpshufb, vpmovmskb), pdepq, and vpcompressb/vpexpandb on zmm. The first AVX2 probe compiled but had been
  folded to scalar code, so it was rewritten; "exit 0" alone would have been a false positive.
- C++23 against libc++ headers for x86_64-apple-darwin: compiles.
- Link for x86_64-apple-darwin: works, but `libclang_rt.osx.a` is arm64-only (linker warning), so there are no x86-64 compiler-rt
  builtins or sanitizer runtimes in this toolchain.
- Rosetta: ABSENT. `arch -x86_64 /usr/bin/true` gives "Bad CPU type in executable", and /usr/bin/true does carry an x86_64 slice
  (`lipo -archs`: x86_64 arm64e arm64e.x1), so the failure is not a missing slice. /Library/Apple/usr/libexec/oah holds only
  RosettaLinux (the guest tool for Linux VMs); no oahd running. Our own x86_64 binary fails the same way (exit 126).
- So the AVX2 run and the AVX-512 fault test could not be done: not established, not estimated. Rosetta was not installed.
- x86_64-unknown-linux-gnu, C, no link: hosted compile FAILS (`stdlib.h` not found, via immintrin.h -> mm_malloc.h: no Linux libc
  headers here). With `-ffreestanding`, both AVX2 and AVX-512 VBMI2 translation units compile to ELF x86-64 objects and the
  disassembly shows vpcompressb/vpexpandb.
- x86_64-unknown-linux-gnu, C++: `<vector>` fails even with the libc++ headers pointed at explicitly (no libc underneath).
  `-ffreestanding -nostdinc++ -isystem .toolchain/llvm/include/c++/v1` compiles a TU using `<bit>` and `<cstdint>` plus immintrin.h.

Result: on the Mac, x86-64 SIMD code can be compile-checked for both targets (AVX2, AVX-512 VBMI2), and for Linux only in
translation units free of libc-dependent headers. None of it can be executed here, so x86-64 SIMD correctness and speed can only
be established on the Linux host.

## Q6. Toolchain

Command: `bash research/machine-probe/src/q6_toolchain.sh`. Raw: raw/q6-toolchain.txt. Sources: src/cxx23probe.cpp, src/fuzz_target.cc.
clang 23.1.4, `-std=c++23`, _LIBCPP_VERSION 230104, process loads /usr/lib/libc++.1.dylib (dyld trace in the raw file).

All nine are defined, and each feature was also used, linked against the system libc++, and run, because a header macro does not
prove that the older system dylib has what it needs.

| Feature | Macro | Value | Used, linked, ran |
|---|---|---|---|
| std::expected | __cpp_lib_expected | 202211L | yes |
| std::span | __cpp_lib_span | 202002L | yes |
| std::mdspan | __cpp_lib_mdspan | 202207L | yes |
| std::print | __cpp_lib_print | 202207L | yes |
| std::bit_cast | __cpp_lib_bit_cast | 201806L | yes |
| std::byteswap | __cpp_lib_byteswap | 202110L | yes |
| std::unreachable | __cpp_lib_unreachable | 202202L | yes |
| std::flat_map | __cpp_lib_flat_map | 202511L | yes |
| std::ranges::to | __cpp_lib_ranges_to_container | 202202L | yes |

- std::unreachable was compiled and linked; executing it is UB, so that is the extent of the check.
- libFuzzer + ASan + UBSan: links (`-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined`), clean target ran 200000
  units. Planted heap overflow: ASan reported heap-buffer-overflow, exit 134. Planted signed overflow: UBSan reported the runtime
  error, exit 77. The sanitized binary links the toolchain's @rpath/libclang_rt.asan_osx_dynamic.dylib alongside system libc++.
- Nothing failed.

## Q2. Cold-cache methods

Fixes to the inherited probe before use: disks() read uninitialised counters when a driver had no Statistics; the dirty test could
pass on a still-resident page, so it now re-evicts and re-verifies; a mincore partial-residency control and a held-mapping case
were added. Three independent signals on each claim: mincore on a fresh untouched mapping, IOKit disk0 "Bytes (Read)" delta, timing.

### Plain APFS, one 1 GiB file (65536 16 KiB pages)
Command: `src/scenarios/run_measured.sh q2a-methods bash src/scenarios/q2a_methods.sh`.
Raw: raw/q2a-methods.txt, raw/q2a-methods.measure.txt (load1 2.71 at start, 2.45 at end).

| Method | Resident after | Next read | disk0 delta | Verdict |
|---|---|---|---|---|
| msync(MS_INVALIDATE) | 0/65536 | 170 ms, 6017 MiB/s | 1024.0 MiB | evicts, 3.6 ms |
| msync(MS_SYNC + MS_INVALIDATE) | 0/65536 | 166 ms, 6171 MiB/s | 1024.0 MiB | evicts, 4.0 ms |
| F_NOCACHE set, nothing read | 65536 | 39 ms | none | no effect |
| F_NOCACHE read of a resident file | 65536 | 39 ms | none | no eviction, served from cache |
| madvise DONTNEED, FREE | 65536 | 39 ms | none | no effect |
| madvise FREE_REUSABLE | 65536 | 39 ms | none | EPERM |
| madvise PAGEOUT | 65536 | 40 ms | none | ENOTSUP |
| msync MS_DEACTIVATE, MS_KILLPAGES | 65536 | 39-42 ms | none | no effect |

- Warm baseline: 1024 MiB in 39-40 ms, about 26 GB/s, no device reads.
- F_NOCACHE: a read of a resident file is served from the cache (41.7 ms, disk0 none), so it neither forces a cold read nor evicts.
  On an already-cold file it reads the device and leaves 0 pages resident (337 ms, 3039 MiB/s, disk0 1024.0 MiB). It is a
  per-descriptor flag set by the reader, so it cannot help with a third-party reader.
- mincore control: after evicting, `dd bs=1m count=256` left 17472/65536 pages (26.66%): 25% plus readahead. mincore sees partial state.
- Held mapping: with another process holding the file mapped with every page touched, msync still went to 0/65536 (63 ms) and the
  next read took 1024.0 MiB from disk0. So a long-lived server's mapping does not block eviction. A server's own user-space buffers
  are another matter and cannot be evicted from outside: restart servers between runs.
- Dirty data: msync(MS_INVALIDATE) with no prior fsync lost nothing in either case (dirtied by mapping, or by write(2)); 16384
  4 KiB blocks, 0 mismatched after re-eviction, read back from the device.
- mmap first touch on a cold file: 1014 MiB/s, 1026.3 MiB off disk0, against 6 GB/s for sequential read(). This is a real
  property of the access path and it matters for comparing mmap-based readers (QuestDB) with read()-based ones.

### Q2b. Third-party eviction: a separate process, a live holder
Command: `src/scenarios/run_measured.sh q2b-thirdparty bash src/scenarios/q2b_thirdparty.sh`.
Raw: raw/q2b-thirdparty.txt, raw/q2b-thirdparty.measure.txt (load1 1.41 at start, 1.41 at end). This replaces the dead run's partial record.
Added for this run: `cachelab holder fd|map|mapdirty`, a live process that keeps its descriptor or mapping across the eviction and
then re-reads through it. The inherited script only had unmodified dd, so it did not test "another process has it open".

The answer is yes. Eviction needs only read access to the file: the evictor opens O_RDONLY and maps PROT_READ.

| Case (1 GiB unless noted) | Evict | Resident after | Next read | disk0 delta |
|---|---|---|---|---|
| Warm dd x5, no eviction | none | n/a | 42-43 ms | about 10 MiB, machine noise, see below |
| Unmodified dd x5 after outside msync | 3.3-3.6 ms | 0/65536 | 165-168 ms | 1025 MiB |
| dd x5 of a fresh clonefile(2) | 0.09-0.12 ms | clone 0/65536, source stayed 65536 | 165-189 ms | 1024-1025.5 MiB |
| Holder keeps open fd x3 | 3.5-3.6 ms | 0/65536 | holder pread 163 ms, 6.3 GiB/s | 1024.0 MiB |
| Holder keeps touched read-only mapping x3 | 17-20 ms | 0/65536 | holder re-touch 1200-1227 ms, about 850 MiB/s | 1024 MiB |
| Holder's dirty MAP_SHARED mapping, 64 MiB, no sync, x3 | 12-15 ms | 0/4096 | holder re-touch 92-97 ms | 64.0 MiB |

- A live holder goes cold through the descriptor or mapping it already has, so the server does not have to reopen anything for the
  OS cache to be cold. Its own user-space buffers are still out of reach.
- A held mapping makes eviction about 5x slower (17-20 ms against 3.5 ms), presumably from tearing down its page mappings; the cause was not tested.
- Dirty data: outside eviction of another process's unsynced mapping lost nothing. 0 mismatched through the live mapping, and 0 of
  16384 4 KiB blocks after a second eviction forced the read back off the device. All three rounds.
- The disk counter is machine-wide: warm dd showed about 10 MiB of someone else's reads in this run and none in q2c. Treat it as a
  lower-bound check on a large read, not an exact figure.

### Q2c. A directory tree, as a server's data directory
Commands: `src/scenarios/run_measured.sh q2c-setup bash src/scenarios/q2c_tree_setup.sh` (setup, not timed), then
`src/scenarios/run_measured.sh q2c-directory bash src/scenarios/q2c_directory.sh`.
Raw: raw/q2c-directory.txt, raw/q2c-directory.measure.txt (load1 1.59, 1.57), raw/q2c-setup.txt. Reader: unmodified `find -exec cat`, x3 each.

| Tree | Warm read | msync evict | Cold read after | disk0 delta | Resident after | clonefile(2) | Clone read |
|---|---|---|---|---|---|---|---|
| big: 256 files of 4 MiB, 1 GiB | 68-69 ms | 6.8-7.7 ms | 478-479 ms | 1024 MiB | 0/65536 | 1.3-1.6 ms | 470-477 ms, 1024 MiB |
| small: 4096 files of 64 KiB, 256 MiB | 74 ms | 50-54 ms | 720-749 ms | 256-256.6 MiB | 0/16384 | 18-19 ms | 733-756 ms, 256 MiB |

- Eviction scales per file (about 13 us each) and works across the tree with no failures.
- A clone of the directory reads exactly as cold and leaves the source hot, at a fraction of the cost. It only helps a reader that
  can be pointed at the clone.
- Metadata stays warm on this route: a `find -exec stat` walk right after eviction took 8 ms (big) and 17 ms (small) with no device
  reads. The evictor has to open every file, so it warms the metadata itself; msync cannot produce cold metadata whatever the kernel does.

### Q2d. Disk-image route (per-run APFS sparse image inside the repo)
Commands: `src/scenarios/run_measured.sh q2d-image bash src/scenarios/q2d_image.sh`, and
`src/scenarios/run_measured.sh q2d-tree bash src/scenarios/q2d_tree.sh` (added: q2d_image.sh had no directory case).
Raw: raw/q2d-image.txt (load1 1.76, 1.73), raw/q2d-tree.txt (load1 2.19, 1.90), each with its .measure.txt.

- Detach and attach evicts: inner file 0/65536 on every cycle, 6 of 6. Next read 420-430 ms, 2383-2440 MiB/s, disk0 1024.0 MiB and
  disk5 (the virtual disk) 1024.0 MiB. Unmodified dd after a cycle: 433 ms, 1024.0 MiB on disk0.
- The feared failure, a read served from the host cache of the backing file, did not happen. The backing file stayed at 126/66369
  pages through three cycles with no host eviction, and evicting it on the host changed nothing (420-430 ms either way). So host
  eviction of the image is unnecessary. The backing file's resident count did not rise after a 1 GiB inner read.
- It costs 184-339 ms per detach or attach, so about 0.6 s per cycle.
- Metadata really does go cold, unlike the msync route: on the 4096-file tree the first walk after a cycle took 53-73 ms with 1.5-2.0 MiB
  off disk0, and the second took 17 ms with none. An earlier version of this scenario ran `resid` first, which walked the tree and
  warmed the metadata; that false "stays cached" result was fixed in the script and the run replaced.
- The volume cannot be detached with a file open: `hdiutil detach` exits 16, "couldn't unmount: Resource busy", and it stays mounted.
  It succeeds once the holder exits. So this route requires a stopped server. `detach -force` was not tested on a held volume.
- hdiutil on this OS prints deprecation warnings for all three verbs used here (`create -size -volname`, `attach -nobrowse -mountpoint`,
  `detach`), pointing at `diskutil image attach` and `diskutil eject`. They still work. The replacements were not tested.

### Q2e. Overhead
Command: `src/scenarios/run_measured.sh q2e-overhead bash src/scenarios/q2e_overhead.sh`, three rounds of each. Raw: raw/q2e-overhead.txt
(load1 1.59 at start, 4.92 at end; the cause of the rise is not known and measure.sh only gates on load at start). Repeats:
`... q2e-image-durlab bash src/scenarios/q2e_image_durlab.sh` (load1 1.71, 1.65) and `... q2e-partial bash src/scenarios/q2e_partial.sh` (load1 1.60, 1.54).
Raw: raw/q2e-image-durlab.txt, raw/q2e-partial.txt, each with its .measure.txt.

msync route (the chosen one): it adds no layer to the read path. A read after msync matches a read of a clone that was never cached
(165 ms for both on 1 GiB). The cost is the eviction step, per run:

| Tree | Evict loop | Whole evictor process, with walk and two mincore passes | mincore proof alone |
|---|---|---|---|
| 256 files, 1 GiB | 7.5-8.7 ms | 47-51 ms | 21-22 ms |
| 4096 files, 256 MiB | 52.6-53.1 ms | 172-174 ms | 61-66 ms |

Image route, same process and same reads, both proven cold first (the image's numbers are all three rounds):

| Measure | Plain | Image | Effect |
|---|---|---|---|
| Sequential 1 GiB | 163 ms, 6277-6289 MiB/s | 436-445 ms, 2301-2348 MiB/s | 2.7x slower |
| Random 16 KiB F_NOCACHE p50 / p99 | 98.2-98.6 / 119.5-120.0 us | 113.0-113.8 / 195.2-195.3 us | +15% / +63% |
| Random 4 KiB F_NOCACHE p50 / p99 | 71.5-73.8 / 88.1-96.7 us | 80.0-82.6 / 100.0-115.8 us | about +11% / +15% |
| 4 KiB append + fsync p50 / p99 | 49-57 / 287-316 us | 128 / 1203 us | 2.4x / 4x |
| 4 KiB append + F_BARRIERFSYNC p50 / p99 | 288-291 / 826-839 us | 12126 / 20217 us | 42x / 24x |
| 4 KiB append + F_FULLFSYNC p50 / p99 | 4007-4008 / 5081-5092 us | 12920 / 16044 us | 3.2x / 3.2x |

- Plain random rows are rounds 2-3 only. Round 1 plain ran with 128 pages left resident (next bullet; F_NOCACHE min 1.2-1.3 us shows
  those reads hit cache), and its p50 was also higher (104.5 and 78.3 us), so the plain p50 range including it is wider but still below the image.
- Image durability figures are from raw/q2e-image-durlab.txt (1000 samples per mode, load1 under 2). The 200-sample block inside
  raw/q2e-overhead.txt agrees, so the 4.92 load was not the cause. In the image, F_BARRIERFSYNC costs the same as F_FULLFSYNC, about
  55 device ops per sample against 16 on plain, so the image erases the difference Q3 measures. The 1000-sample run has one 270 ms outlier
  (max of F_BARRIERFSYNC). An earlier run of the same script, replaced because its script ended with a stray exit 1, also had one.
- Partial eviction: 1 of about 38 recorded eviction passes left 128/65536 pages resident (residency lines reading 0 or near it: q2a 6, q2b 14, q2c 6, q2e 12; q2e-overhead round 1 plain, right after a cold
  read). A targeted repeat of that sequence did not reproduce it, 0 of 60 cycles. The cause is not established. It is rare, it is real,
  and only the mincore check caught it, which is why the proof step below is not optional.

### Q2 verdict
- (a) A single file of ours: `msync(MS_INVALIDATE)` on a fresh read-only `MAP_SHARED` mapping of the whole file. It costs 3.5 ms per GiB,
  works from a separate process, works with the file open or mapped elsewhere, and lost no dirty data. For repeated random reads
  also read through F_NOCACHE so the run does not warm itself, but evict first: F_NOCACHE alone is served from cache on a resident file.
- (b) A server's data directory: the same msync across every file in the tree, with the server stopped, then start it and query.
  Cost 47-174 ms for these trees. The call also works on a running server, but stop it anyway, since its own buffers cannot be evicted
  from outside. Order: stop server, evict, prove, start server, query, check the device delta. Do not use the image route to compare
  systems: it is colder only in metadata (1.5-2 MiB and 35-55 ms on 4096 files) and in exchange takes sequential reads down 2.7x and
  sync-heavy writes up 2.4-42x, which would misstate any system compared through it.
- Stated limit of (b): metadata stays warm. Of the routes tested, only the image takes metadata cold, so a metadata-sensitive test has to
  run there, be labelled as image numbers, and not be compared with plain-volume numbers. A clonefile(2) of the tree is equally cold for
  data and keeps the source hot if a system can be pointed at a copy. Its metadata has just been written, so it should not be expected
  to help with metadata (inferred, no separate test).
- How eviction is proven, on every run:
  1. mincore on a fresh, untouched mapping of every file must read 0 resident. If not, evict again; never proceed on a non-zero count.
  2. After the read, the disk0 "Bytes (Read)" delta must be at least the bytes the query should read. Use disk0 and not the virtual disk.
     The counter is machine-wide, so it can overstate but a shortfall is conclusive.
  3. Timing should be in the cold range: 1 GiB sequential about 165 ms cold, 42 ms warm.
- Not tried: `sudo purge` and anything else needing privilege or a reboot. Those are not measured.

## Q3. fsync vs F_BARRIERFSYNC vs F_FULLFSYNC on this APFS volume
Command: `bash src/scenarios/q3_durlab.sh`, one measure.sh call per size, each `bin/durlab scratch/q3 ROUNDS ITERS BYTES` on a fresh
directory. /dev/disk3s5, APFS, internal SSD. Each sample is write() + sync, in blocks that rotate order each round. 0 failures in all runs.
Raw: raw/q3-durlab-plain-4k.txt (load1 2.09, 2.11), raw/q3-durlab-plain-4k-rep.txt (2.11, 1.92), raw/q3-durlab-plain-64k.txt (1.92, 1.84),
raw/q3-durlab-plain-256b.txt (1.84, 1.60), each with its .measure.txt.

4 KiB append, 4000 samples per mode, two independent runs, in us:

| Mode | p10 | p50 | p90 | p99 | max | Device ops per sample |
|---|---|---|---|---|---|---|
| write only | 2.3 / 2.7 | 6.4 / 6.4 | 18 / 22 | 38 / 48 | 275 / 275 | 0 |
| fsync | 27 / 36 | 49 / 57 | 91 / 107 | 316 / 287 | 2401 / 538 | 1.05 / 1.04 |
| F_BARRIERFSYNC | 144 / 166 | 288 / 291 | 555 / 538 | 826 / 839 | 2088 / 1499 | 16.2 / 16.1 |
| F_FULLFSYNC | 3892 / 3886 | 4008 / 4007 | 4183 / 4188 | 5092 / 5081 | 23338 / 20780 | 16.5 / 16.3 |

Other sizes, p50 / p99 in us:

| Mode | 256 bytes (n=4000) | 64 KiB (n=2000) |
|---|---|---|
| fsync | 42 / 289 | 64 / 242 |
| F_BARRIERFSYNC | 278 / 814 | 291 / 853 |
| F_FULLFSYNC | 4010 / 5101 | 3956 / 5024 |

- F_FULLFSYNC is a very stable 4 ms, 70-80x plain fsync at p50 and 14x F_BARRIERFSYNC, and barely depends on size from 256 bytes to 64 KiB.
- fsync and the other two are different operations: fsync issues about 1 device op per sample, the other two about 16. So fsync on
  this volume does much less than the barrier. F_BARRIERFSYNC and F_FULLFSYNC issue the same number of device writes; so the write count does not
  explain the 3.7 ms gap between them.
- These are latencies and device-op counts only. What each call guarantees across power loss was not tested, so nothing here ranks them on
  durability. Any fsync number quoted on macOS must name the primitive: the ratio between them is 70-80x.

## Q4. Timers, pinning, QoS, performance counters
Commands: `src/scenarios/run_measured.sh q4-timelab bash src/scenarios/q4_timelab.sh` (full, x3), `... q4-timelab-qos bash src/scenarios/q4_timelab.sh qos`
(x3), `... q4-timelab-qos-taskpolicy /usr/sbin/taskpolicy -b bin/timelab qos` (control), `... q4-pmclab bin/pmclab`.
Raw: raw/q4-timelab.txt (load1 1.54, 1.47), raw/q4-timelab-qos.txt (1.47, 1.89), raw/q4-timelab-qos-taskpolicy.txt (1.89, 1.82),
raw/q4-pmclab.txt (1.82, 1.84), each with its .measure.txt.

Fix to the probe: `"isb; mrs ..."` in timelab.cpp produced no mrs at all, because ';' starts a comment in Darwin arm64 assembly. The first
run reported all-zero counter deltas and 4.4e14 ticks from an uninitialised register. Now split with a newline, and confirmed by disassembly.

Clocks:
- mach timebase: numer=125, denom=3, 41.667 ns per tick, 24.000 MHz (hw.tbfrequency 24000000). The rate was cross-checked against
  CLOCK_MONOTONIC_RAW over about 200 ms, three times.
- std::chrono::steady_clock: period is 1 ns but real resolution is one tick. The smallest non-zero delta is 41 ns, then 42 and 83, and over half
  of consecutive reads return the same value. Read cost 11.9-12.3 ns (mean over 20M calls), against 4.0 ns for mach_absolute_time.
  high_resolution_clock has the same 1 ns period.
- So any single measurement under about 42 ns is not resolvable with steady_clock, and timing a loop needs enough iterations to dwarf one tick.
- Raw cntvct_el0 from EL0: CNTFRQ_EL0 reads 1 GHz and the counter does advance at 1 GHz (210025699 ticks in 210.026 ms, and the same ratio in the
  other two runs). But its deltas come in steps of 41/42, so it is still 24 MHz underneath. It offers no finer clock. A few hundred 17/18 deltas per
  2M reads are unexplained.

CPU placement:
- THREAD_AFFINITY_POLICY: `thread_policy_set` returns kr=46, KERN_NOT_SUPPORTED, and `thread_policy_get` too (tag=0). The tag is
  rejected outright, so it cannot have any effect. No pinning exists on this host via this call.
- THREAD_EXTENDED_POLICY and THREAD_PRECEDENCE_POLICY calls return success. Their effect was not separately measured. QoS results below do
  not change with or without them (q4-timelab vs q4-timelab-qos).
- QoS: no thread-level hint moved work in a foreground process. Requested QoS is honoured as a label (`pthread_get_qos_class_np` reads back
  0x21, 0x19, 0x15, 0x11, 0x9) and every call returned 0, yet USER_INTERACTIVE, USER_INITIATED, DEFAULT, UTILITY and BACKGROUND all ran on
  P cores, 114 of 114 spins across raw/q4-timelab.txt and raw/q4-timelab-qos.txt, at the same speed (642-718 ticks/chunk for all of them). Same for QoS set by the
  thread on itself, PRIO_DARWIN_BG on the thread, and a 3 s BACKGROUND spin. An earlier run, since overwritten, had 4 of 14846 samples on an E core.
- Positive control, so the above is not a blind spot: `taskpolicy -b` on the same binary put every thread on cpu0-3, 19 of 19 spins, at
  2134-2836 ticks/chunk, 3-4x slower, and including USER_INTERACTIVE. So E cores are visible to the probe, and a process-level
  background policy overrides thread QoS completely.
- Placement among P cores is not stable: threads crossed between clusters (cpu4-9 and cpu10-15) in 16 of 114 spins, and nothing here can
  prevent it. The test ran with load1 under 2; behaviour under P-core contention was not tested.

Performance counters:
- kpc is not reachable with no entitlement and no sudo. `dlopen` of kperf succeeds and the read-only counts answer (7 counters, 5 configurable),
  but `kpc_force_all_ctrs_get` and `kpc_force_all_ctrs_set(1)` both return -1, errno 1, Operation not permitted, and `kpc_get_counting` also sets
  errno 1. pmclab stopped at that point, once, as told. No sudo and no entitlement were tried, so which of the two would unlock it is unknown.
  No counter state changed.
- Separate fact, not a kpc route and not a stand-in for it: public `proc_pid_rusage` (RUSAGE_INFO_V6) returns process-wide cumulative instruction and
  cycle counts with no privilege, and is linear (5.00 instructions and 6.00 cycles per xorshift round at 500M and 1000M rounds). It has no cache-miss,
  branch or other selectable events, and no per-thread or per-region control. The errno=1 on its lines is stale from the kpc step; rc is 0.

## Gaps
Neither of these is estimated or substituted here.
- Hardware performance counters (cache misses, branch misses, configurable events): kpc denied, EPERM. Needs a user decision on sudo or an entitlement.
- CPU pinning: THREAD_AFFINITY_POLICY is not supported on this host, so single-core benchmarks cannot be pinned. Thread QoS does not control placement either.
- Power-loss behaviour of the three sync primitives was not tested; Q3 is latency and device ops only.
