# Machine probe: what this host can and cannot measure

Status: in progress. Host: Apple M4 Max, macOS, arm64, shared.
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

## Q2. Cold-cache methods (in progress)

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

