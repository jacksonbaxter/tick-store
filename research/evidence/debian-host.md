# Evidence: debian host feasibility (Phase 1)

Status: desk research written before the host was contacted, then checked on the host. Results, and every place the host contradicted
this file, are in the host-check report, which is kept outside this repo (`debian-check/README.md`, see `research/README.md`) and takes precedence. Everything below comes from published sources or cached
copies, confirmed in primary text.

CACHE is the research cache, kept outside this repo; `CACHE/...` paths are labels into it.

## Q1. Packaging (x86-64 Linux, Debian 13, no sudo/apt/Docker)

Verification note: every line below was re-read from the primary file, not taken
from the transcript digest. Wheel and tarball SHA-256 values were recomputed on
this Mac and match PyPI / GitHub. ELF metadata was re-parsed from the extracted
wheels with a fresh parser (the digest's first parse lumped GLIBCXX into GLIBC and
its first `pext` count was a substring false positive; neither is repeated here).

### Debian 13 baseline

| Fact | Quote / value | Location | URL |
|---|---|---|---|
| glibc in trixie | `Package: libc6 (2.41-12+deb13u4)` | CACHE/downloads/dh-debian/libc6-trixie.html line 94 | https://packages.debian.org/trixie/libc6 |
| glibc in trixie, 2nd source | `glibc \| 2.41-12+deb13u4 \| trixie \| source` | dh-debian/glibc-madison2.txt line 6 | https://qa.debian.org/madison.php?package=glibc&table=debian&text=on |
| default python3 | `Package: python3 (3.13.5-1)`; depends `python3.13 (>= 3.13.5-1~)` | dh-debian/python3-trixie.html lines 93, 316-317 | https://packages.debian.org/trixie/python3 |
| python3.13 package | `python3.13 \| 3.13.5-2+deb13u5 \| trixie` | dh-debian/python3.13-madison.txt line 1 | https://qa.debian.org/madison.php?package=python3.13&table=debian&text=on |
| libstdc++ | `Package: libstdc++6 (14.2.0-19)` | dh2-pkg/deb-libstdcpp6.html line 92 | https://packages.debian.org/trixie/libstdc++6 |
| GCC 14 symbol level | `GCC 14.1.0: GLIBCXX_3.4.33, CXXABI_1.3.15` | dh2-pkg/gcc-abi.html line 131 | https://gcc.gnu.org/onlinedocs/libstdc++/manual/abi.html |
| libc6 file list | `/usr/lib/x86_64-linux-gnu/libdl.so.2`, `libpthread.so.0`, `librt.so.1`, `libresolv.so.2` all in libc6 | dh2-pkg/deb-libc6-filelist.html lines 317, 327-329 | https://packages.debian.org/trixie/amd64/libc6/filelist |
| linux-perf in trixie | `Package: linux-perf (6.12.113-1)` | dh2-pkg/deb-linux-perf.html line 95 | https://packages.debian.org/trixie/linux-perf |

The host's actual patch level is not known from here. See "To verify on the host".

### pyarrow 26.0.0 (Mac pin exists for Linux: yes)

- Current version on PyPI: `26.0.0`, uploaded 2026-10-09; same as the Mac pin. Source:
  dh-pypi/pyarrow.json `info.version`, `urls[]`. URL https://pypi.org/pypi/pyarrow/json
- Requires-Python `>=3.11` (json `info.requires_python`; wheel METADATA line 19).
- manylinux x86-64 wheel tags: only `manylinux_2_28_x86_64` (plus musllinux_1_2, not relevant).
- CPython: cp311, cp312, cp313, cp314, cp314t, cp315, cp315t.
- CPython 3.13 wheel: `pyarrow-26.0.0-cp313-cp313-manylinux_2_28_x86_64.whl`, 53,926,722 bytes,
  sha256 `240bd18a7487f8767616a948a69dd4e740a8bc36a1c9da49e4dc9a32c5c2faed` (recomputed, matches).
  URL https://files.pythonhosted.org/packages/4f/d6/e95834b29360092376fe4da9956ba41bb7b021869efe6ee9d4172d05cb15/pyarrow-26.0.0-cp313-cp313-manylinux_2_28_x86_64.whl
- Fit: 2.28 <= Debian 13's 2.41. Fresh ELF parse of all 38 ELF files:
  highest `GLIBC_2.28` (libarrow.so.2600), `GLIBCXX_3.4.22`, `CXXABI_1.3.11`, `GCC_7.0.0`. All within
  Debian's libstdc++ 14 (GLIBCXX_3.4.33 / CXXABI_1.3.15).
- External NEEDED: ld-linux-x86-64.so.2, libc.so.6, libdl.so.2, libgcc_s.so.1, libm.so.6,
  libpthread.so.0, librt.so.1, libstdc++.so.6. No libcurl, libssl, libcrypto, zlib or zstd NEEDED.
- Why: wheel build uses vcpkg static triplet: `VCPKG_TARGET_TRIPLET:=${VCPKG_DEFAULT_TRIPLET:-x64-linux-static-${CMAKE_BUILD_TYPE}}`,
  dh-arrow-src/ci_scripts_python_wheel_xlinux_build.sh line 85.
  URL https://raw.githubusercontent.com/apache/arrow/apache-arrow-26.0.0/ci/scripts/python_wheel_xlinux_build.sh
- CPU: compile-time baseline: `"DEFAULT" # default to SSE4_2 on x86, NEON on Arm, NONE otherwise`,
  dh-arrow-src/cpp_cmake_modules_DefineOptions.cmake line 164. Wheel script passes no `ARROW_SIMD_LEVEL`
  (grep of the script for SIMD finds only the xsimd line). Runtime level default `"MAX" # default to max supported by compiler`, line 177.
  AVX-512 code is present in the binaries (libparquet zmm lines 76,315) but runtime-dispatched per that option; a static
  check cannot prove every path is gated, so SIGILL-free import stays on the host list.
- **Zen 1 finding**: Arrow already avoids PEXT/PDEP on AMD.
  `// BMI2 (pext, pdep) is only efficient on Intel X86 processors.` and
  `return vendor() == Vendor::Intel && IsSupported(BMI2);`, dh-arrow-src/cpp_src_arrow_util_cpu_info.h lines 105-106.
  Guards at cpp/src/parquet/level_conversion.cc line 128 and arrow/compute/util.cc lines 94, 229, 285, 315,
  key_map_internal.cc lines 134, 305 (arrow-26-tree copy). Binary check: real PEXT/PDEP instructions:
  libparquet 1/0, libarrow_compute 5/4, libarrow 0/0. So Parquet decoding on the host will not hit the
  Zen 1 PEXT penalty through these paths. Applies to tag apache-arrow-26.0.0.

### arcticdb 6.28.2 (Mac pin exists for Linux: yes)

- Current version on PyPI: `6.28.2`, uploaded 2026-10-26; same as the Mac pin. URL https://pypi.org/pypi/arcticdb/json
- `requires_python`: not stated (null in JSON).
- manylinux x86-64 wheel tags: `manylinux2014_x86_64.manylinux_2_17_x86_64` (WHEEL lines 4-5).
- CPython: cp39, cp310, cp311, cp312, cp313, cp314.
- CPython 3.13 wheel: `arcticdb-6.28.2-cp313-cp313-manylinux2014_x86_64.manylinux_2_17_x86_64.whl`, 21,380,361 bytes,
  sha256 `d368fc869883aada995122853eb7a44243f7b67c8fbdf757746ab3dc3f8f8be7` (recomputed, matches).
  URL https://files.pythonhosted.org/packages/67/a1/c84f42a8b9a76a025f9354348bbe8d9bc6aacb4c2f58b108f073f89f928a/arcticdb-6.28.2-cp313-cp313-manylinux2014_x86_64.manylinux_2_17_x86_64.whl
- Fit: 2.17 <= 2.41. Fresh ELF parse of 11 ELF files: highest `GLIBC_2.17`, `GLIBCXX_3.4.19`, `CXXABI_1.3.7`, `GCC_3.4`.
- External NEEDED: ld-linux-x86-64.so.2, libc.so.6, libdl.so.2, libgcc_s.so.1, libm.so.6, libpthread.so.0,
  libresolv.so.2, librt.so.1, libstdc++.so.6. No libcurl or libssl NEEDED. Bundled in `arcticdb.libs/`:
  libsasl2, libkrb5, libgssapi_krb5, libk5crypto, libkrb5support, libcom_err, libcrypt, libkeyutils, libpcre, libselinux.
- Python deps from wheel METADATA lines 18-25: `pandas<3`, numpy, attrs, `protobuf<7,>=3.15.0`, `msgpack>=0.5.0`, pyyaml, packaging, pytz.
  Repo lock (`requirements.lock`, kept outside this repo) pins numpy 2.5.4, pandas 2.3.3, protobuf 6.33.6; all have
  cp313 glibc x86-64 wheels: `numpy-2.5.4-cp313-cp313-manylinux_2_27_x86_64.manylinux_2_28_x86_64.whl`,
  `pandas-2.3.3-cp313-cp313-manylinux_2_24_x86_64.manylinux_2_28_x86_64.whl`,
  `protobuf-6.33.6-cp39-abi3-manylinux2014_x86_64.whl` (dh2-pkg/pypi-*.json, https://pypi.org/pypi/numpy/2.5.4/json etc).
  QuestDB client `questdb-6.0.1-cp313-cp313-manylinux_2_28_x86_64.whl` also exists (https://pypi.org/pypi/questdb/6.0.1/json).
- CPU: no `-march`, `-mavx`, `-msse` or `-mbmi` flag in cpp/CMakeLists.txt, cpp/arcticdb/CMakeLists.txt or CMakePresets.json
  (grep over dh-arcticdb-src; tag v6.28.2). `CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG"` at cpp_CMakeLists.txt line 248.
  The `.so` holds zmm code (73,980 lines); origin not traced, likely dispatch in a bundled dependency, but that is inference:
  **not stated**. Real PEXT/PDEP in arcticdb_ext: 0/0. No published CPU requirement found: not stated.

### QuestDB 10.1.0

Release: `10.1.0`, published 2026-10-22, not a prerelease. https://github.com/questdb/questdb/releases/tag/10.1.0
(dh-questdb-rel/latest.json). Assets:

| Asset | Size (bytes) | GitHub digest | Notes |
|---|---|---|---|
| questdb-10.1.0-rt-linux-x86-64.tar.gz | 95,654,357 | sha256:567994861a032bd939588ed207e4f074db975c74ea5386cd2e01c8d68136b244 | Bundled runtime. `JAVA_VERSION="25.0.2"` in `release` |
| questdb-10.1.0-no-jre-bin.tar.gz | 34,276,936 | sha256:5d84ca94bb46eacf53fc0d27b30e50d82fa13324069e073a67a60c0cf0de5cff | Needs own JDK |
| questdb-10.1.0-rt-windows-x86-64.tar.gz | 86,488,172 | sha256:7abd62fa6189614bbc7198e28f7f4c31b063f3856e47b499aeb5a0afcca049d6 | Not relevant |

URLs: https://github.com/questdb/questdb/releases/download/10.1.0/<asset>. Digest values come from the GitHub API `assets[].digest`
field; both linux-relevant files recomputed here and match. The release body has no checksum text (digest's search for sha256/checksum: False).

- Host libraries needed (fresh ELF parse of rt/lib and rt/bin):
  - `lib/libjli.so` NEEDED `libz.so.1`. `bin/java` needs libjli, so the JRE cannot start without host zlib.
  - `lib/libquestdb.so` NEEDED `libstdc++.so.6`; `lib/libquestdbr.so` NEEDED `libgcc_s.so.1`.
  - Highest versions: `GLIBC_2.16`, `GLIBCXX_3.4.18`, `CXXABI_1.3`, `GCC_4.2.0`. bin/libjemalloc.so: no external NEEDED beyond libc.
  - X11/ALSA/freetype NEEDED only by desktop libs (libawt_xawt, libsplashscreen, libjsound, libfontmanager). Whether a headless server loads them: not stated, not expected.
  - Debian: `zlib1g` is a dpkg dependency (dh2-pkg/deb-dpkg.html line 405), `libstdc++6` an apt dependency (deb-apt.html line 555),
    `libgcc-s1` a libc6 dependency (deb-libc6.html line 275). Whether the host was installed with these: confirm with ldconfig below.
- CPU requirement, source tag 10.1.0:
  - C++ SIMD is compiled four ways and chosen at runtime. `Java will then call a dispatcher, which will check instruction set for the runtime`,
    dh-questdb-src/core_CMakeLists.txt line 280.
  - `if (iset == 10) return avx512; if (iset >= 8) return avx2;`, core_src_main_c_share_dispatcher.h, `dispatch_to_ptr`.
    Vect.java lines 136-139: `inst >= 10` is AVX512, `inst >= 8` is AVX2. Zen 1 lacks AVX-512 (Q2) so it should land on AVX2.
  - Fallbacks exist down to `Vanilla`, so AVX2 is not mandatory for the C++ layer.
  - Rust lib: `core/rust/qdbr/.cargo/config.toml` is only `rustflags = ["-D", "warnings"]` (no target-cpu); the workflow greps no RUSTFLAGS/target-cpu.
  - Real PEXT/PDEP: libquestdb 0/0, libquestdbr 0/0. Zen 1's slow PEXT does not touch QuestDB's own native code.
  - No published CPU requirement found beyond "x86_64": `QuestDB should be deployed on Intel/AMD architectures i.e. x86_64`,
    dh-docs/src_components_DRY__questdb_production_hardware-minimums.mdx (Architecture section).

### QuestDB OS settings: mandatory or advisory

**Advisory at startup. Runtime failure only if actually exhausted.**

- Startup code only logs. `Bootstrap.java` `verifyFileLimits()` (from line 614, called line 173) logs
  `vm.max_map_count limit is too low [limit=` ... `(SYSTEM COULD BE UNSTABLE)` at advisory level (line 637) and the same for files (line 624);
  no exception is thrown. The only hard fail nearby is NFS (`QuestDB cannot start because`, line 674). Tag 10.1.0.
- Threshold: `DEFAULT_FILE_LIMIT = 65536`, `DEFAULT_MAP_COUNT_LIMIT = 65536`, Files.java lines 45-46. Warns if limit `<=` that.
  Kernel default 65530 therefore triggers the warning.
- The "file" check is the soft per-process rlimit, not the sysctl: `getrlimit(RLIMIT_NOFILE, &limit)` ... `return (jlong) limit.rlim_cur;`,
  dh2-pkg/qdb-linux-files.c lines 476-484. Map count is read from `/proc/sys/vm/max_map_count`, line 491.
  https://raw.githubusercontent.com/questdb/questdb/10.1.0/core/src/main/c/linux/files.c
- Docs: `Raise both to 1048576 before running in production.` and `hitting a limit at runtime causes OS errors and can leave the database unstable.`
  dh-docs/documentation_getting-started_capacity-planning.md lines 322-325 (main branch of questdb/documentation, not tagged).
  https://questdb.com/docs/getting-started/capacity-planning/
- What hits them: `at least one per column per partition` (open files, line 335). `especially under out-of-order ingestion` (mappings, line 398).
  A formula for mapping count: not stated.
- Arithmetic from the doc's lower bound only (not a doc figure): for one trading day partitioned by DAY, one partition times C columns is C files.
  A 15-column table would need >= 4,369 partitions to reach 65,536 files at the stated one-per-column minimum. A one-day sample is nowhere near either limit,
  provided ingest is in timestamp order. `errno=24` and `errno=12` are the failure signatures (lines 337, 402).
- Soft nofile: `-XX:-MaxFDLimit Disables the attempt to set the soft limit for the number of open file descriptors to the hard limit.`
  and `By default, this option is enabled on all platforms` (dh-docs/java25.html, Java SE 25 `java` command). questdb.sh only passes `-XX:-MaxFDLimit` on Darwin
  (core_src_main_bin_questdb.sh lines 376-384), so on Linux the JVM lifts soft to hard itself. The doc says the same: `On Linux it is enough to raise the hard limit`, line 361.
- Debian: docs say 65530 for `RHEL 9, Ubuntu 22.04, and Debian 12` (line 399). Debian 13: not stated. Kernel: see host list.
- Workaround with no sudo:
  1. Accept the advisory warnings, record them in the run log, and keep the benchmark table to PARTITION BY DAY on sampled days, ingesting in timestamp order.
  2. `vm.max_map_count` cannot be changed without root. Nothing to work around unless the mapping count nears it; monitor with `wc -l /proc/$PID/maps` during ingest and query.
  3. Hard nofile cannot be raised without root, but the JVM raises soft to hard itself. A hard limit above 65536 also silences the warning.
  4. Keep QuestDB's root under ~/quant-projects with `-d`; default on Linux is `$HOME/.questdb` (questdb.sh line 81).

### uv under ~/quant-projects only

- Binary: uv `0.13.6`, published 2026-10-27. `uv-x86_64-unknown-linux-gnu.tar.gz`, 16,799,024 bytes, sha256 `f29c3e94ffabd7e6f3ba521dba169a9b7ec12100bf3257087f0f24f6123f3645`.
  Also `sha256.sum` asset. https://github.com/astral-sh/uv/releases/tag/0.13.6 (dh-docs/uv-latest.json). manylinux_2_17 per dh-docs/uv-platforms.md.
- `UV_UNMANAGED_INSTALL` `to install uv to a specific path while preventing the installer from modifying shell profiles or environment variables`
  (dh-docs/uv-env.html, https://docs.astral.sh/uv/reference/environment/). Also dh-docs/uv-installer-docs.md lines 41-48: `will also disable self-updates`.
- `UV_CACHE_DIR`: `uv will use this directory for caching instead of the default cache directory.`
- `UV_PYTHON_INSTALL_DIR`: `Specifies the directory for storing managed Python installations.`
- `UV_PYTHON_BIN_DIR`: `Specifies the directory to place links to installed, managed Python executables.`
- `UV_PYTHON_CACHE_DIR`: `Specifies the directory for caching the archives of managed Python installations before installation.`
- `UV_TOOL_DIR`, `UV_TOOL_BIN_DIR`: tool dirs. `UV_MANAGED_PYTHON`: `Require use of uv-managed Python versions.`
- Temp: `$TMPDIR` then `/tmp` (dh-docs/uv-storage.md, "Temporary directory"). Data defaults `$XDG_DATA_HOME/uv`, `$HOME/.local/share/uv`.
- uv would otherwise accept Debian's system 3.13; `--managed-python` forces its own (uv-python-install.md lines 388-393).
- python-build-standalone publishes `cpython-3.13.18+20261026-x86_64-unknown-linux-gnu-install_only.tar.gz` and v2/v3/v4 variants
  (vendor-meta/pbs-SHA256SUMS lines 327, 348, 369, 390). Mac's exact 3.13 patch: confirm from the Mac `.venv` before pinning; Mac `.venv` not read per rule 7.

### Verdicts

| System | Verdict | Blocking risk |
|---|---|---|
| pyarrow 26.0.0 | runs natively, smoke only (host check, kept outside this repo) | none known |
| arcticdb 6.28.2 | runs natively, smoke only (host check, kept outside this repo) | origin of zmm code still untraced; no SIGILL on the paths the smoke ran |
| QuestDB 10.1.0 | runs natively, smoke only, `-Xmx1g` (host check, kept outside this repo) | default heap untested |
| uv + CPython 3.13 | installed from pinned checksums (host check, kept outside this repo) | none known |

Install commands to run later (not run):

```bash
# All paths under ~/quant-projects. Assumes bash.
export QP=$HOME/quant-projects
export UV_CACHE_DIR=$QP/cache/uv UV_PYTHON_INSTALL_DIR=$QP/python UV_PYTHON_BIN_DIR=$QP/python/bin \
       UV_PYTHON_CACHE_DIR=$QP/cache/uv-python UV_TOOL_DIR=$QP/uv-tools UV_TOOL_BIN_DIR=$QP/uv-tools/bin \
       UV_NO_MODIFY_PATH=1 XDG_CACHE_HOME=$QP/cache/xdg XDG_DATA_HOME=$QP/cache/xdg-data TMPDIR=$QP/tmp
mkdir -p "$QP/bin" "$QP/tmp" "$QP/dl"

# uv
curl -fsSL -o "$QP/dl/uv.tgz" https://github.com/astral-sh/uv/releases/download/0.13.6/uv-x86_64-unknown-linux-gnu.tar.gz
echo "f29c3e94ffabd7e6f3ba521dba169a9b7ec12100bf3257087f0f24f6123f3645  $QP/dl/uv.tgz" | sha256sum -c -
tar -xzf "$QP/dl/uv.tgz" -C "$QP/bin" --strip-components=1

# Python and the two wheels; hashes come from the repo lock
export UV_MANAGED_PYTHON=1   # documented: "Require use of uv-managed Python versions."
"$QP/bin/uv" python install 3.13
"$QP/bin/uv" venv --python 3.13 "$QP/venv"
"$QP/bin/uv" pip install --python "$QP/venv/bin/python" --only-binary :all: --require-hashes -r requirements.lock

# QuestDB, bundled runtime
curl -fsSL -o "$QP/dl/qdb.tgz" https://github.com/questdb/questdb/releases/download/10.1.0/questdb-10.1.0-rt-linux-x86-64.tar.gz
echo "567994861a032bd939588ed207e4f074db975c74ea5386cd2e01c8d68136b244  $QP/dl/qdb.tgz" | sha256sum -c -
mkdir -p "$QP/questdb" && tar -xzf "$QP/dl/qdb.tgz" -C "$QP/questdb"
"$QP/questdb/questdb-10.1.0-rt-linux-x86-64/bin/questdb.sh" start -d "$QP/questdb-data"
```

`requirements.lock` (kept outside this repo) contains the Linux cp313 hashes for pyarrow (line 268) and arcticdb (line 13), so `--require-hashes` should pass for those two. The Linux hashes for numpy 2.5.4, pandas 2.3.3, protobuf 6.33.6 and the questdb 6.0.1 client (cp313 manylinux x86-64 wheels) also appear in the lock: each sha256 from CACHE/downloads/dh2-pkg/pypi-*.json was searched for in the file and found.

## Q2. Zen 1 and V1605B facts affecting SIMD design

Identity caveat. AMD's V1000 page says `4C/8T "Zen" Cores` and `14nm` but gives no ISA list. AMD's PPR for V1605B's exact
model was not found. The nearest AMD primary text opened is the PPR for **Family 17h Model 18h B1** (a sibling Zen-core APU,
not V1605B's stated part), so ISA lines below are "sibling, AMD primary" and must be confirmed on the host by `lscpu`.
Kernel 6.12 treats family 0x17 models `0x00 ... 0x2f` and `0x50 ... 0x5f` as `X86_FEATURE_ZEN1` (dh-kernel/arch_x86_kernel_cpu_amd.c lines 429-433,
https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/kernel/cpu/amd.c), which covers any model in that range.

PPR URL: https://www.amd.com/content/dam/amd/en/documents/processor-tech-docs/programmer-references/55570-B1-PUB.zip
Copy: CACHE/downloads/dh2-cpu/ppr/55570-B1-3.16_PUB_NRV.pdf (text in the .txt beside it). Revision 3.16, Apr 14 2021.

| Fact | Result | Quote | Exact location | URL | Applies to |
|---|---|---|---|---|---|
| AVX2 | yes | `5 AVX2. Read-only. Reset: Fixed,1.` | PPR PDF p.66, CPUID_Fn00000007_EBX_x00 | PPR URL | Fam 17h Model 18h B1 |
| BMI2 | yes | `8 BMI2. Read-only. Reset: Fixed,1.` | PPR PDF p.66, same table | PPR URL | Fam 17h Model 18h B1 |
| AVX-512F | no | EBX register reset `209C_01A9h`; `17:9 Reserved.`; the string `AVX512` occurs nowhere in the PPR text | PPR PDF p.66 | PPR URL | Fam 17h Model 18h B1 |
| AVX-512 VBMI2 | no | CPUID_Fn00000007_ECX_x00: `31:0 Reserved.` | PPR PDF p.67 | PPR URL | Fam 17h Model 18h B1 |
| Compiler ISA view | no AVX-512 | `znver1` list ends `... XSAVES, CLFLUSHOPT, POPCNT, and 64-bit instruction set extensions.` and has BMI2, AVX2, no AVX512 | dh-docs/gcc-x86.html, `-march=znver1` entry | https://gcc.gnu.org/onlinedocs/gcc/x86-Options.html | GCC znver1 target |
| Measured ISA | no AVX-512 rows | ZEN+: 286 AVX2, 32 BMI2, 0 AVX512* measured; ZEN4: 9,984 AVX512* | dh-cpu/uops-instructions.xml, count by `extension` per `architecture` | https://uops.info/instructions.xml | Zen+ (uops.info has no Zen 1 entry) |
| Agner Zen 1 table | no AVX-512 rows | zero `AVX-512`, `zmm` hits in Zen 1 section | agner-tables PDF pp.87-99 | https://www.agner.org/optimize/instruction_tables.pdf | Ryzen 7 1800X |
| 256-bit split | yes | `256-bit AVX and AVX2 instructions are split into two µops that do 128 bits each.` | Agner microarchitecture.pdf p.235, sec 22 (text line 13191) | https://www.agner.org/optimize/microarchitecture.pdf | Zen 1 |
| 256 still worthwhile | yes | `It is still more efficient to use 256-bit vector instructions than 128-bit instructions on Zen 1, due to efficient fetch and decoding.` (quote split, line 13191-13193) | same | same | Zen 1 |
| Throughput | | `most 256-bit instructions can execute with a throughput of at least one instruction per clock cycle on Zen 1` | same, p.235 | same | Zen 1 |
| Store width | | `There is one store unit of 128 bits on Zen 1` | same, p.235 | same | Zen 1 |
| Microcode rule | | `Instructions that generate more than two µops are using microcode.` | same, p.233 (line 13055) | same | Zen 1-2 |
| PDEP | 6 ops, lat 18, rtp 18 | `PDEP r,r,r 6 18 18 BMI2` | agner instruction_tables.pdf p.90 | Agner tables | Zen 1 (Ryzen 7 1800X) |
| PEXT | 7 ops, lat 18, rtp 18 | `PEXT r,r,r 7 18 18 BMI2` | same p.90 | same | Zen 1 |
| PEXT/PDEP 2nd source | PEXT 7 uops, PDEP 6 uops, TP 18.00; latency 3 and 17 by operand | XML: TP_unrolled `18.00`, `cycles` 3 (op2) and 17 (op3) | dh-cpu/uops-instructions.xml, `PEXT (R64, R64, R64)` `ZEN+` | https://uops.info/instructions.xml | Zen+ |
| Cheap alternatives | 1 op | `BZHI r,r,r 1 1 0.25 BMI2`; `SHRX r,r,r 1 1 0.25 BMI2`; `SHLX` same; `TZCNT r,r 2 2 0.5 BMI1`; `LZCNT r,r 1 1 0.25` | agner tables p.90 | same | Zen 1 |
| L1I | 64 KB per core | `64 kB, 4 way, 256 sets, 64 B line size, per core. Latency 4 clocks` | Agner uarch Table 22.2, p.237 | same | Zen 1 |
| L1I, AMD | 64 KB | `L1IcSize. Read-only. Reset: Fixed,64.` | PPR text line 3893 | PPR URL | Model 18h B1 |
| L1D | 32 KB per core | `32 kB, 8 way, 64 sets, 64 B line size, per core. Latency 4 clocks` | Agner Table 22.2 | same | Zen 1 |
| L1D, AMD | 32 KB | `L1DcSize. Read-only. Reset: Fixed,32.` | PPR text line 3875 | PPR URL | Model 18h B1 |
| L2 | 512 KB per core | `512 kB, 8 way, 1024 sets, 64 B line size, per core. Latency 17 clocks` | Agner Table 22.2 | same | Zen 1 |
| L2, AMD | 0200h KB (512) | `L2Size. Read-only. Reset: Fixed,0200h.` | PPR text line 3962 | PPR URL | Model 18h B1 |
| L3 | 4 MB total | `"totalL3Cache"` ... `"formatValue":"4 MB"` | dh-cpu/amd-v1000.html, JSON entry `AMDRyzenEmbeddedV1605BwithRadeonVega8Graphics` | https://www.amd.com/en/products/embedded/ryzen/ryzen-v1000-series.html | V1605B exactly |
| Boost | 3.6 GHz | `"cpuMaxFreq"` ... `"formatValue":"3.6 GHz"` | same | same | V1605B exactly |
| Base | 2 GHz | `"cpuBaseFreq"` ... `"formatValue":"2 GHz"` | same | same | V1605B exactly |
| Cross-check L1/L2/L3 | 4x64, 4x32, 4x512 KB, 4 MB | `L1 Instruction Cache: 4 x 64 KB L1 Data Cache: 4 x 32 KB L2 Cache: 4 x 512 KB L3 Cache: 4 MB` | dh2-cpu/cpubench-v1605b.html | https://www.cpubenchmark.net/cpu.php?id=3331 | V1605B (secondary) |

Conflict logged: cpubenchmark.net states `Turbo Speed: 3.2 GHz`; AMD and Wikipedia's list state 3.6. AMD is primary. Host check decides.
Wikipedia's Zen article also gives `L3 cache 8 MB per CCX (APU: 4 MB)`.

Not the same as Table 22.2: Agner's `16 MB` L3 is the desktop part. The host has 4 MB, per AMD. Do not copy 16 MB.

### Vector costs that bear on bit unpacking (Zen 1, Agner tables, columns Ops / Latency / Reciprocal throughput)

| Instruction | Ops | Lat | Rtp | Location |
|---|---|---|---|---|
| VPADD.. y,y,r/m | 2 | 1 | 0.67 | p.94 |
| VPSHUFB y,r/m | 2 | 1 | 1 | p.93 |
| VPSLLVD/Q VPSRAVD VPSRLVD/Q y,y,y | 2 | 3 | 4 | p.96 |
| VPSLLVD/Q VPSRAVD VPSRLVD/Q x,x,x | 1 | 3 | 2 | p.96 |
| VPERMD v,v,v | 3 | 5 | 4 | p.94 |
| VPERMQ v,v,i | 3 | 2 | 2 | p.94 |
| VPERM2I128 v,v,v,i | 8 | 4 | 3 | p.94 |
| VPMULLD y,y,r/m | 2 | 4 | 4 | p.95 |
| VPGATHERQQ y,[r+s*y],y | 35 | n/a | 12 | p.94 |
| VPGATHERDD y,[r+s*y],y | 66 | n/a | 20 | p.94 |
| VMOVDQA y,m256 | 2 | 3 | 1 | p.93 |
| VMOVDQA m256,y | 2 | 4 | 2 | p.93 |

uops.info ZEN+ agrees on the variable shift: `VPSLLVQ (YMM, YMM, YMM)` uops 2, TP 4.00, cycles 4.

What this changes:
- PEXT/PDEP throughput of 18 cycles is against 0.25 for BZHI/SHRX: 72x worse in reciprocal throughput (arithmetic from Agner p.90). PEXT-based unpacking is out on this host. Ship a non-PEXT path as the default for Zen 1, and keep any PEXT path as an opt-in dispatch, as Arrow's `HasEfficientBmi2()` does.
- AVX-512 and VBMI2 paths will never run on this host. A build that needs them has no route here.
- AVX2 is available and worth using, but variable shifts (rtp 4) and VPERMD (rtp 4) are the slow spots. Shift-by-constant and VPSHUFB-based designs avoid them.
- 4 MB L3 is a quarter of the desktop Zen 1 figure in Agner's table; decode benchmarks with blocks beyond it hit DRAM sooner than on the desktop parts.

## Q3. Cold cache on Linux without root

Sources: man-pages 6.19 (dh-man/*.txt, https://man7.org/linux/man-pages/), kernel 6.12.107 stable tree (dh-kernel/stable-6.12.107-*, https://raw.githubusercontent.com/gregkh/linux/v6.12.107/...).
Debian trixie carries 6.12.107-1 (dh-kernel/linux-madison.txt), with 6.12.113-1 in trixie-security. The evict path did not differ between v6.12 and 6.12.107 in the digest's diff; only fd-handling lines changed. Host's exact kernel is on the host list.

| Question | Finding | Quote | Exact location |
|---|---|---|---|
| Guarantee | None. It is a request | `POSIX_FADV_DONTNEED attempts to free cached pages associated with the specified region.` | posix_fadvise.2.txt lines 69-70 |
| Return value | 0 regardless of how much was dropped | final `return 0;`; `nr_failed` is only used to retry | stable-6.12.107-mm_fadvise.c lines 158-176 |
| Partial pages | Ignored; align or use whole file | `Requests to discard partial pages are ignored.` | posix_fadvise.2.txt line 76 |
| Dirty pages | Not freed | `Any unwritten dirty pages will not be freed.` | posix_fadvise.2.txt lines 82-83 |
| Dirty remedy | fsync first | `it should call fsync(2) or fdatasync(2) first.` | posix_fadvise.2.txt line 85 |
| Dirty, kernel | Async writeback only, then skip | `__filemap_fdatawrite_range(mapping, offset, endbyte,` with `WB_SYNC_NONE`; evict: `if (folio_test_dirty(folio) \|\| folio_test_writeback(folio))` `return 0;` | mm_fadvise.c lines 114-115; mm_truncate.c lines 293-294 |
| Mapped by another process | Not evicted | `/* The refcount will be elevated if any page in the folio is mapped */` | stable-6.12.107-mm_truncate.c line 295 |
| tmpfs / DAX | Returns 0, no effect | `/* no bad return value, but ignore advice */` | mm_fadvise.c lines 51, 58-60 |
| Whole file form | offset 0, len 0 | `Len == 0 means "as much as possible".` | mm_fadvise.c line 68 (comment) |
| Separate process | Works. Eviction acts on the inode's page cache, not on the caller's | `mapping = file->f_mapping;` | mm_fadvise.c `generic_fadvise`. Man page: not stated |
| Ownership needed for fadvise | No check in the syscall path; any readable fd reaches it | `SYSCALL_DEFINE4(fadvise64...` calls `vfs_fadvise` | stable-6.12.107-mm_fadvise.c lines 180 (`vfs_fadvise`), 191-198 (`ksys_fadvise64_64`), 208-210 (`SYSCALL_DEFINE4(fadvise64`). Man page: not stated |
| Prove with mincore | man page gives the recipe | `opening a file, mapping it with mmap(2), and then applying mincore(2) to the mapping.` | posix_fadvise.2.txt lines 160-161 |
| mincore is a snapshot | | `the contents of vec may already be stale by the time this call returns.` | mincore.2.txt |
| **mincore can lie** | Without write access it reports everything resident | `memset(vec, 1, pages);` under `if (!can_do_mincore(vma))` | stable-6.12.107-mm_mincore.c lines 196-199; rule at 158-171 |
| Same in fincore doc | | `returns fake data as if all pages were resident in memory` | fincore.1.txt, DESCRIPTION |
| cachestat | Errors rather than lying; since 6.5 | `Linux 6.5.`; `return -EPERM;` under `!can_do_cachestat` | cachestat.2.txt HISTORY; stable-6.12.107-mm_filemap.c lines 4419-4426, 4487-4489 |
| **fincore availability** | Not in `util-linux` on trixie. It is in `util-linux-extra` | `/usr/bin/ fincore util-linux-extra` | dh2-pkg/deb-search-fincore.html; dh2-pkg/deb-util-linux-extra-filelist.html line 60; https://packages.debian.org/trixie/amd64/util-linux-extra/filelist |

What fadvise does not reach: dentries, inodes and other slab. `To free reclaimable slab objects (includes dentries and inodes)` needs `drop_caches` value 2 (dh-kernel/Documentation_admin-guide_sysctl_vm.rst lines 240-242), which needs root. So filesystem metadata stays warm. Drive-internal cache is also untouched. The honest label is "page cache evicted", not "cold boot".

Method (works for QuestDB, ArcticDB, Parquet; for a third-party process the key step is stopping it):

1. Stop the reader and let it exit. QuestDB: `questdb.sh stop`. A live process holding a mapping defeats eviction, per the mm_truncate.c line above. Whether QuestDB keeps mappings between queries is not stated, so assume yes.
2. For each regular file under the data directory, in a separate process owned by the same user: `open`, `fsync`, `posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)`, `close`. Using fsync first clears the dirty and writeback cases. On the host, Python stdlib has `os.fsync` and `os.posix_fadvise`, so no compiler is needed.
3. Prove it. Sum resident pages over the same file list with `fincore --bytes --noheadings --output RES` if present, otherwise a small tool using `mmap` + `mincore` or `cachestat`. The tool runs as the file owner, otherwise the mincore result is fake. Require resident = 0; fail the run otherwise, because the syscall's 0 return proves nothing.
4. Start the reader, record resident bytes again (server start reads some files itself), then time the first query. Report the post-start resident figure with the result.
5. Run on a real disk filesystem, not tmpfs. Filesystem is on the host list.

Side effect: only the benchmark's own files are evicted. Other services keep their cache, which suits a shared always-on host better than `drop_caches` would even with root.

Optional second route (unverified, documented): cgroup v2 `memory.reclaim` is `A write-only nested-keyed file which exists for all cgroups.` (cgroup-v2.rst line 1314), with the warning `the kernel can over or under reclaim` (line 1323). systemd's user unit has `Delegate=pids memory cpu` (dh-docs/systemd-user-service.in line 28). Whether an SSH session has a user manager, and whether the delegation works here, can only be settled on the host. Not required by the method above.

## Q4. Durability on Linux vs macOS

| Fact | Quote | Exact location | URL | Applies to |
|---|---|---|---|---|
| Linux fsync covers device cache | `This includes writing through or flushing a disk cache if present.` | dh-man/fsync.2.txt, DESCRIPTION (grep line 37) | https://man7.org/linux/man-pages/man2/fsync.2.html | Linux, man-pages 6.19 |
| Blocks until device reports | `The call blocks until the device reports that the transfer has completed.` | same, lines 37-38 | same | Linux |
| Directory entry not covered | `an explicit fsync() on a file descriptor for the directory is also needed.` | same, DESCRIPTION | same | Linux |
| fdatasync | `does not flush modified metadata unless that metadata is needed` for later retrieval; `st_size` change does need a flush | same, DESCRIPTION | same | Linux |
| Caveat | `older kernels and lesser used filesystems do not know how to flush disk caches.` | same, lines 111-112 | same | Linux |
| ext4 issues the flush | `err = blkdev_issue_flush(inode->i_sb->s_bdev);` under `if (needs_barrier)` | dh-kernel/fs_ext4_fsync.c lines 170-171 | https://raw.githubusercontent.com/torvalds/linux/v6.12/fs/ext4/fsync.c | ext4, 6.12 |
| macOS fsync stops at the drive | `the drive itself may not physically write the data to the platters for quite some time` | dh-man/mac-fsync.txt, DESCRIPTION | local `man 2 fsync`, macOS 27.2 | macOS |
| Loss scenario | `the application may find that only some or none of their data was written.` | same | same | macOS |
| F_FULLFSYNC | `Does the same thing as fsync(2) then asks the drive to flush all buffered data to the permanent storage device` | dh-man/mac-fcntl.txt lines 137-139 | local `man 2 fcntl`, macOS 27.2 | macOS |
| Filesystems | `currently implemented on HFS, MS-DOS (FAT), Universal Disk Format (UDF) and APFS file systems.` | same, lines 143-145 | same | macOS |
| F_BARRIERFSYNC | orders, does not persist: `valid state on disk when ordering is a concern but durability is not.` | same, lines 112 (flag) and 124-125 (quote) | same | macOS |

So Linux `fsync` on ext4 and macOS `F_FULLFSYNC` are the matching pair. Linux `fsync` and macOS `fsync` are not.

What each compared system actually calls:

| System | Linux | macOS | Default | Location |
|---|---|---|---|---|
| QuestDB | `fsync` | plain `fsync` too; `osx/` has no files.c, so the shared file applies; no `F_FULLFSYNC` in it | `cairo.commit.mode` default `nosync`: `no explicit flush (relies on OS page cache)`; `sync` `waits for flush on appended column files to complete` | dh2-pkg/qdb-share-files.c lines 274-275; repo tree tag 10.1.0 (dh-questdb-src/tree.json, `core/src/main/c/osx/` holds accept.c, affinity.c, kqueue.c); dh-docs/documentation_configuration_cairo-engine.md lines 102-111 |
| LMDB 0.9.35 (ArcticDB 6.28.2 pin) | `fdatasync` | `fcntl(fd, F_FULLFSYNC)` | syncs on commit; ArcticDB extra flags `Default: 0 (no extra flags).`, `MDB_NOSYNC \| MDB_NOMETASYNC` `skips the fsync on every commit` | CACHE/downloads/lmdb-src/mdb.c (byte-identical to dh2-pkg/lmdb-pinned-mdb.c, https://raw.githubusercontent.com/LMDB/lmdb/69087ced3cb6082f7dcfb4fc2dcaa3b68a7e2e8c/libraries/liblmdb/mdb.c) line 132 (Apple), lines 455 (default), 2589, 2599; lmdb-src/lmdb.h lines 199-213 for version; arcticdb-docs/runtime_config.md lines 117-128 |
| pyarrow / Parquet | none | none | no fsync at all in the write path files checked | zero `fsync`/`fdatasync`/`FULLFSYNC` hits in arrow-26-src/cpp_src_arrow_io_file.cc, cpp_src_arrow_util_io_util.cc, cpp_src_arrow_filesystem_localfs.cc, cpp_src_parquet_file_writer.cc |

Correction to the cache: dh-docs/lmdb-mdb.c and dh-docs/lmdb.h are LMDB **0.9.70, dated "December 19, 2015"** (mdb.master), not the version ArcticDB links. Pin 69087ced at 0.9.35 comes from research/evidence/column-systems.md line 56; I re-downloaded mdb.c at that commit and confirmed it equals CACHE/downloads/lmdb-src/mdb.c. Do not cite the dh-docs copy for ArcticDB. The sync macros agree in both copies, only line numbers differ. On Linux the pinned file has `BROKEN_FDATASYNC` at line 92 and a runtime kernel check; a 6.12 kernel is new enough per the master copy's comment `Kernels after 3.6-rc6 are known good.`

Honest comparison across the two hosts:

1. Do not compare absolute durable-write numbers across hosts as a software result. Storage differs (host device unknown, see host list) and the calls differ. Report each host's durable numbers separately, with ratios within a host.
2. Pick one uniform end-of-run barrier applied by the harness, because the systems cannot be made to issue the same call themselves. After a system reports ingest done, the harness walks every file in its data directory and then every directory, calling `fsync` on Linux and `fcntl(F_FULLFSYNC)` on macOS. The barrier's time is included in "durable ingest time". Apply it to all four systems, tick-store included.
3. Report "buffered ingest" (no barrier) and "durable ingest" (with barrier) as two columns. Never label plain macOS `fsync` as durable.
4. Per-commit durability is a separate claim. Default QuestDB is `nosync`, LMDB syncs per commit, Parquet has no commit concept. State each system's mode. Do not imply the end-of-run barrier gives crash consistency during ingest.
5. For tick-store's own write path, use `fsync` on Linux and `F_FULLFSYNC` on macOS, and fsync the directory after create or rename on both.

## Q5. Timing and measurement without privileges

### perf_event_paranoid = 2

| Fact | Quote | Exact location | URL |
|---|---|---|---|
| Scope at 2 | `*scope* includes per-process performance monitoring only.` | dh-kernel/Documentation_admin-guide_perf-security.rst lines 248-249 (">=2") | https://docs.kernel.org/admin-guide/perf-security.html |
| User space only | `CPU and system events happened when executing in user space only can be monitored` | same, lines 249-251 | same |
| Sysctl table | `>=2  Disallow kernel profiling by users without CAP_PERFMON.` | Documentation_admin-guide_sysctl_kernel.rst line 954 | https://docs.kernel.org/admin-guide/sysctl/kernel.html |
| Kernel check | `if (!attr.exclude_kernel) {` `err = perf_allow_kernel(&attr);`; `sysctl_perf_event_paranoid > 1 && !perfmon_capable()` returns `-EACCES` | dh-kernel/kernel_events_core.c lines 12689-12693, 13544-13549 | https://raw.githubusercontent.com/torvalds/linux/v6.12/kernel/events/core.c |
| Debian default differs | `CONFIG_SECURITY_PERF_EVENTS_RESTRICT=y`; patched default `int sysctl_perf_event_paranoid __read_mostly = 3;`; `3 - disallow all unpriv perf event use` | dh-kernel/linux-debian-config.txt line 7725; dh-kernel/debian-perf-patch.patch lines 45-46 | https://salsa.debian.org/kernel-team/linux (debian/6.12.107-1) |

Consequences:
- Allowed: per-process counters with kernel excluded (`cycles:u`, `instructions:u`) on the user's own processes. System-wide and kernel counts are excluded at level 2.
- Forbidden: system-wide `-a`, per-CPU counting, and kernel-mode counts. Kernel time inside syscalls and page faults is not counted by `:u` events, so `:u` counters understate I/O-heavy paths. Cold-read benchmarks need wall clock alongside `:u` counts.
- Debian's boot default is 3. The host value of 2 was set by someone. It may not survive reboot; re-check in every run header.
- `perf` itself ships in Debian package `linux-perf` (6.12.113-1), which needs apt. Whether it is installed: host list. If absent, `perf_event_open(2)` can be called directly from the harness.

### Affinity and SMT

| Fact | Quote | Exact location |
|---|---|---|
| Unprivileged own process | `A user can change the CPU affinity of a process belonging to the same user.` | dh-man/taskset.1.txt lines 98-99 |
| Other users | `must possess CAP_SYS_NICE to change the CPU affinity of a process belonging to another user.` | same, lines 99-100 |
| Syscall rule | `The caller needs an effective user ID equal to the real user ID or effective user ID of the thread identified by pid` | dh-man/sched_setaffinity.2.txt lines 73-75 |
| Inheritance | `A child created via fork(2) inherits its parent's CPU affinity mask.` and `preserved across an execve(2).` | same, lines 129-130 |
| Isolation needs boot option | `isolcpus is the preferred mechanism of isolating CPUs` | same, lines 125-126. Needs a kernel parameter, so root. Not available |
| SMT sibling, current name | `core_cpus_list` `human-readable list of CPUs within the same core.` | dh-kernel/Documentation_ABI_stable_sysfs-devices-system-cpu lines 68-71 |
| Deprecated name | `(deprecated name: "thread_siblings_list").` | same, line 71 |
| Sharing under SMT | `each thread will get half of the resources when two threads are running in the same core.` | Agner microarchitecture.pdf p.240 |

Affinity limits a process to CPUs; it does not keep other services off those CPUs. Without isolcpus or root cgroups, an always-on host can still schedule its services onto the sibling or the pinned core. Pin to one CPU, leave its `core_cpus_list` sibling idle by convention, and record load (`/proc/loadavg`, involuntary context switches via `getrusage` or `/proc/PID/status`) during runs so disturbed runs can be discarded. Multi-threaded systems (QuestDB, Arrow) inherit the mask; pass a mask of the same CPUs to all systems.

### Governor and frequency

| Fact | Quote | Exact location |
|---|---|---|
| Debian default governor | `CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y` | dh-kernel/linux-debian-config-x86.txt line 430 (debian/6.12.107-1) |
| Driver fallback | `All Zen based processors support the legacy ACPI hardware P-States function` then `the kernel will fall back to initialize the acpi-cpufreq driver` | Documentation_admin-guide_pm_amd-pstate.rst lines 192-194 |
| amd-pstate reach | `Currently, some of the Zen2 and Zen3 processors support amd-pstate.` | same, line 204. Zen 1: not stated, so expect `acpi-cpufreq` |
| Read governor | `scaling_governor` `The scaling governor currently attached to this policy` | Documentation_admin-guide_pm_cpufreq.rst lines 301-304 |
| Read driver | `scaling_driver` `The scaling driver currently in use.` | same, lines 298-299 |
| Do not trust scaling_cur_freq | `which may or may not reflect the frequency the CPU is actually running at` | same, lines 287-291 |
| Hardware reading | `cpuinfo_cur_freq` `obtained from the hardware`; `should not be present` if undeterminable | same, lines 243-249 |
| Boost file | `/sys/devices/system/cpu/cpufreq/` `boost`; `It only means a permission to use the frequency boost mechanism` | same, lines 626-637 |
| Why boost matters | `to be able to run tests with and without boosting` | same, line 670-671 |

Governor is handled by measure.sh, but `performance` still allows boost, and boost depends on load. Writing `boost` needs root. So clock speed varies between 2.0 base and up to 3.6 GHz during a run unless it was disabled. Record `boost` value and derive effective frequency from `cycles:u / task-clock` per run. Do not report scaling_cur_freq as measured.

### rdtscp as a timer

| Fact | Quote | Exact location | Applies to |
|---|---|---|---|
| Invariant TSC bit | `TscInvariant: TSC invariant. Read-only. Reset: Fixed,1. The TSC rate is invariant.` | PPR 55570 PDF p.81 (text line 4082), CPUID_Fn80000007_EDX bit 8 | Fam 17h Model 18h B1 |
| Rate | `The TSC increments at the P0 frequency. The TSC counts at the same rate in all P-states, all C states, S0, or S1.` | PPR text lines 4584-4585, MSR0000_0010 | same |
| RDTSCP present | `27 RDTSCP. Read-only. Reset: Fixed,1. RDTSCP instruction.` | PPR text line 3623 | same |
| Linux flag | `c->x86_power is 8000_0007 edx. Bit 8 is TSC runs at constant rate`; sets `X86_FEATURE_CONSTANT_TSC`, `X86_FEATURE_NONSTOP_TSC` | dh-kernel/arch_x86_kernel_cpu_amd.c lines 546-551 | Linux 6.12 AMD |
| Not serializing (Intel text) | `The RDTSCP instruction is not a serializing instruction, but it does wait until all previous instructions have executed` | Intel SDM text via https://www.felixcloutier.com/x86/rdtscp | Intel. AMD's text not opened |
| Later code may run first | `subsequent instructions may begin execution before the read operation is performed.` | same | Intel |
| LFENCE on AMD | `/* A serializing LFENCE stops RDTSC speculation */`; `MSR_AMD64_DE_CFG_LFENCE_SERIALIZE_BIT` set by kernel | amd.c lines 1019-1028 | Linux on AMD |
| Cost | `RDTSCP 64 64 rdtscp`; `RDTSC 37 36` | Agner tables p.91 | Zen 1 |
| Cost, 2nd | RDTSCP uops 37, TP 65.00; RDTSC uops 37, TP 36.90 | uops-instructions.xml, ZEN+ | Zen+ |

- Measures: elapsed reference ticks at the fixed P0 rate, whatever the core clock does. It is wall time, not cycles: when the core boosts, one TSC tick spans more than one core cycle.
- Does not measure: core cycles, retired work, or anything a descheduled thread did not do. A context switch inside the interval is counted. Reads on different cores assume synchronized TSCs; RDTSCP's ECX lets the harness detect migration (TSC_AUX; contents are OS-defined).
- Resolution: the tick is 1/P0. AMD's PPR says the rate is P0; V1605B's P0 is not stated by AMD's page. Per-read cost is about 64 cycles on Zen 1, so timing anything under a few hundred cycles per sample is dominated by the timer. Time batches.
- Calibrate ticks per second on the host against `CLOCK_MONOTONIC_RAW`; do not assume 2.0 GHz.
- Pattern: `rdtscp; lfence` at both ends; AMD's own ordering text was not opened, so the Intel text is the reference, with kernel confirmation that LFENCE serializes on Linux on AMD.
- `clocksource` state is on the host list. The TSC watchdog is only disabled with `X86_FEATURE_TSC_ADJUST` (tsc.c lines 1259-1262); whether Zen 1 has it: not stated.

## Q6. Memory fit on 15 GB

Convention in this section: quotes are verbatim. "Arithmetic" means I computed it from quoted numbers. "Implication" is my reading, not a source statement. Cached files were matched by sha256 to the URL given.

### Input size

| Fact | Quote or value | Location | URL | Applies to |
|---|---|---|---|---|
| Nasdaq size guidance | `Version 5.0 files average 5-10 GB compressed and 15-25GB uncompressed.` | CACHE/downloads/itch-faq-wayback/ITCH_FAQ-20250206.pdf p. 3; faq.txt line 101 | https://web.archive.org/web/20250206122726/https://www.nasdaqtrader.com/Content/TechnicalSupport/FAQs/ITCH_FAQ.pdf | paid Historical product. Do not use downloads/itch-faq/ITCH_FAQ.pdf: it is a 3,184-byte HTML stub |
| Three known days | 10302019: 9,035,016,843; 01302020: 12,952,050,754; S061226: 41,662,444,846 decompressed bytes | research/evidence/itch-data.md line 176 (day columns: section 4.1 header; messages: line 175) | sources listed in itch-data.md; third-party parser output, not re-derived here | sample days |

So a full day runs from under the host's 15 GB to nearly three times it.

### QuestDB 10.1.0

| Fact | Quote | Location | URL | Applies to |
|---|---|---|---|---|
| RAM recommendation | `We recommend having at least 8GB of RAM for basic workloads, and 32GB for more advanced ones.` | dh-docs/documentation_getting-started_capacity-planning.md lines 173-174 | https://raw.githubusercontent.com/questdb/documentation/main/documentation/getting-started/capacity-planning.md | "basic" and "advanced" are not defined in bytes |
| Page cache | `For relatively small datasets i.e 4-40GB, and a read-heavy workload` | same, lines 176-178 | same | continues: performance can be improved by maximising use of the OS page cache |
| Production minimum | `at least 4 vCPUs and 8 GiB of memory` | dh-docs/src_components_DRY__questdb_production_hardware-minimums.mdx | https://raw.githubusercontent.com/questdb/documentation/main/src/components/DRY/_questdb_production_hardware-minimums.mdx | production |
| O3 memory | `16MB (2x8MB) RAM per column when it receives O3 writes` | capacity-planning.md lines 186-193 | as above | per column, out-of-order writes only |
| Start flags | `-XX:+AlwaysPreTouch`, `-XX:+UseParallelGC`, `${JVM_PREPEND}`. No `-Xmx` | dh-questdb-src/core_src_main_bin_questdb.sh lines 363, 364, 373 | https://raw.githubusercontent.com/questdb/questdb/10.1.0/core/src/main/bin/questdb.sh | `questdb.sh` start |
| JVM default max heap | `The default value is 25 percent.` | dh-docs/java25.html line 3120 (`-XX:MaxRAMPercentage`, entry from line 3115) | https://docs.oracle.com/en/java/javase/25/docs/specs/man/java.html | Java 25 |
| JVM default initial heap | `The default value is 1.5625 percent.` | same, line 2970 | same | Java 25 |
| Ergonomics page | `Maximum heap size of 1/4 of physical memory` | dh-docs/ergonomics.html line 104 (initial, 1/64: line 101) | https://docs.oracle.com/en/java/javase/25/gctuning/ergonomics.html | its G1 default does not apply: questdb.sh forces ParallelGC |
| Pre-touch | `touch every page on the Java heap` | java25.html line 2823 | as above | committed heap |
| Native limit | `Process-wide native memory limit as a percentage of the memory visible to the JVM` | dh-docs/documentation_configuration_cairo-engine.md lines 1426-1435, default 90 | https://raw.githubusercontent.com/questdb/documentation/main/documentation/configuration/cairo-engine.md | `ram.usage.limit.percent` |
| What it excludes | `The JVM heap, thread stacks, and memory-mapped files such as table column files do not count` | same, lines 1411-1413 | same | that limit |
| Writer page | `mmap sliding page size that the table writer uses to append data for each column.` | same, lines 164-170, default 16M | same | `cairo.writer.data.append.page.size` |
| Commit batch | `Maximum number of uncommitted rows per table.` | same, lines 113-118, default 500000 | same | `cairo.max.uncommitted.rows` |

Doc conflict, resolved by code. cairo-engine.md lines 256-262 gives `cairo.o3.column.memory.size` as `Default: 256k`, `512KB per column`. capacity-planning.md says 8M. Tag 10.1.0 code: PropServerConfiguration.java line 2098 `CAIRO_O3_COLUMN_MEMORY_SIZE, 8 * Numbers.SIZE_1MB`, DefaultCairoConfiguration.java lines 738-739. The 256K belongs to the separate system key (`CAIRO_SYSTEM_O3_COLUMN_MEMORY_SIZE`, PropServerConfiguration.java line 2100). Files in CACHE/downloads/questdb-10.1.0-src/ as core_src_main_java_io_questdb_*.java. **8 MB is the 10.1.0 default.**

Arithmetic: 25% of 15 GB is 3.75 GB max heap, 1.5625% is about 0.23 GB initial. Exact values depend on the host's real MemTotal. To set `-Xmx`, pass it through `JVM_PREPEND`.

Total RAM for ingesting one ITCH day into QuestDB: **not stated** in any source opened. No formula either. A QuestDB ingest figure must be measured.

### ArcticDB 6.28.2 on LMDB

| Fact | Quote | Location | URL | Applies to |
|---|---|---|---|---|
| Default map | `default_map_size = 400ULL * (1ULL << 30); /* 400 GiB */` | dh-docs/adb-storage-lmdb.cpp line 438 | https://raw.githubusercontent.com/man-group/ArcticDB/v6.28.2/cpp/arcticdb/storage/lmdb/lmdb_storage.cpp | Linux path |
| Why so large | `Linux just gets an arbitrarily large size that it probably won't ever reach.` | same, lines 432-433 (code comment) | same | non-Windows: the `#ifdef _WIN32` branch at line 434 is the only other case (2 GiB), so macOS and Linux share 400 GiB |
| Map is a ceiling | `on Linux the map size is an upper bound to the physical space that will be used` | CACHE/downloads/arcticdb-docs/index.md lines 470-483 | https://raw.githubusercontent.com/man-group/ArcticDB/v6.28.2/docs/mkdocs/docs/index.md | LMDB backend |
| Override | `lmdb://path/to/desired/database?map_size=2GB` | same | same | URI |
| Full map | `Errors with lmdb error code -30792 indicate that the map is getting full` | same | same | LMDB backend |
| LMDB | `The size of the memory map is also the maximum size of the database.` | CACHE/downloads/lmdb-src/lmdb.h, mdb_env_set_mapsize doc (lines 816-817) | https://raw.githubusercontent.com/LMDB/lmdb/69087ced3cb6082f7dcfb4fc2dcaa3b68a7e2e8c/libraries/liblmdb/lmdb.h | LMDB 0.9.35 |
| Segment defaults | `rows_per_segment` 100_000, `columns_per_segment` 127 | arcticdb-6.28.2-src/python_arcticdb_options.py lines 47-48 | https://raw.githubusercontent.com/man-group/ArcticDB/v6.28.2/python/arcticdb/options.py | library defaults |
| Incremental write path | `lib.append("my_data", df_to_append)` | arcticdb-docs/tutorials_fundamentals.md line 125 | https://raw.githubusercontent.com/man-group/ArcticDB/v6.28.2/docs/mkdocs/docs/tutorials/fundamentals.md | append |
| Staged writes exist | `ArcticDB does support staging multiple single-symbol concurrent writes.` | arcticdb-docs/faq.md line 159 | https://raw.githubusercontent.com/man-group/ArcticDB/v6.28.2/docs/mkdocs/docs/faq.md | single-symbol staging |

Map size is address space, not a RAM reservation, per the docs. 400 GiB on a 15 GB host is therefore not itself a blocker. The real ceiling is free disk.

RAM needed to ingest into ArcticDB: **not stated**. Searched api_library.md, api_processing.md, technical_on_disk_storage.md, tutorials_fundamentals.md, tutorials_library_sizes.md, faq.md, index.md. api_library.md is autodoc with no body text in the cache, so `staged` semantics were not read and the memory cost of `append` is unknown.

### pyarrow 26.0.0

| Fact | Quote | Location | URL | Applies to |
|---|---|---|---|---|
| Tracking | `precisely track amount of memory that has been allocated` | dh-docs/pyarrow-memory.rst lines 88-96 (`pa.total_allocated_bytes()` same block) | https://raw.githubusercontent.com/apache/arrow/apache-arrow-26.0.0/docs/source/python/memory.rst | Arrow pool |
| Zero copy read of IPC | `do not allocate any new memory on read` | dh-docs/pyarrow-ipc.rst lines 109-111 | https://raw.githubusercontent.com/apache/arrow/apache-arrow-26.0.0/docs/source/python/ipc.rst | memory-mapped IPC, not Parquet |
| Batch writing | `we in theory need to keep in memory only the current batch we are writing` | same, lines 196-202 | same | IPC stream/file writing |
| Parquet writing | `ParquetWriter` | arrow-26-tree/arrow-apache-arrow-26.0.0/docs/source/python/parquet/parquet.rst lines 236-240 | https://raw.githubusercontent.com/apache/arrow/apache-arrow-26.0.0/docs/source/python/parquet/parquet.rst | incremental row groups |
| Parquet reading | `For memory-efficient reads of large files, iter_batches` | same, line 247 | same | Parquet read |
| Parquet memory_map | `but won't help much with resident memory consumption.` | same, lines 102-103 | same | Parquet read: unlike IPC, a Parquet read decodes into memory |
| Table size | `nbytes` property | arrow-26-tree/.../python/pyarrow/table.pxi lines 234-247 | Arrow tag | measure after loading |
| Padding | `length that is a multiple of 8 or 64 bytes` | dh-docs/arrow-columnar.rst line 269 | https://raw.githubusercontent.com/apache/arrow/apache-arrow-26.0.0/docs/source/format/Columnar.rst | buffers |

Size of an in-memory Arrow table of one ITCH day: **not stated**. It depends on a schema that does not exist yet.

Arithmetic bound, not an estimate of real use: 01302020 has 423,285,709 messages. 15e9 / 423,285,709 is about 35.4 bytes per row. If one flat table holds every message, it cannot fit in 15 GB above that width, and that leaves nothing for Python, the OS or other services. For 1,304,894,064 messages the same bound is about 11.5 bytes per row.

### Implications for warm-cache and ingest (mine, not sourced)

1. Do not load a whole day into one in-memory table. Ingest through batches on every system: `ParquetWriter`, ArcticDB `append` or staging, QuestDB batched commits. The tick-store ingest should stream too.
2. A 41.7 GB day cannot be a warm-cache case on this host unless a query touches only part of it. Whether a 13 GB day can depends on stored size, unknown until ingest. Pick the benchmark day after measuring stored bytes per system.
3. Do not assume warm. A warm result counts only if `fincore` or `mincore` shows 100% of the files the query reads resident, after the warm-up pass and again after the measured run. Anything less is labelled partial with the percentage, or dropped.
4. For QuestDB, column files are mapped and sit in the page cache, outside `ram.usage.limit.percent`, so the JVM heap and the warm files compete for the same 15 GB. Record heap setting with every result.
5. Run one system at a time, with the others stopped. Other services on the host take RAM unpredictably, so log `MemAvailable` before each run and refuse the warm run if stored bytes exceed it.
6. For each ingest record peak RSS (`VmHWM` from /proc/<pid>/status), `pa.total_allocated_bytes()` where relevant, and stored bytes. These are the numbers the docs do not give.
7. Check for a cgroup `memory.max` on the session: it would cap page cache below 15 GB.
8. Confirm free disk before ArcticDB and QuestDB ingest.

## Not found

| Item | Where I looked | Result |
|---|---|---|
| AMD Software Optimization Guide 55723 (Family 17h) | amd.com URLs, kib.kiev.ua, searches | 404 or refused. Used Agner Fog and uops data instead, cited in Q2 |
| AMD APM 24594, 24593, 40332 | amd.com | 404. Only third-party copies found, not used as primary |
| AMD text on RDTSCP ordering | as above, PPR 55570 | not found. Q5 cites what was opened |
| PPR for the V1605B's exact model; its CPUID model | AMD PPR 55570 (Family 17h Model 18h B1) opened, it is the nearest sibling; openbenchmarking 403 | exact model unconfirmed, on host list |
| V1605B P0 frequency | AMD pages, PPR | not stated. On host list |
| Max boost, single value | AMD says 3.6 GHz, cpubenchmark 3.2 GHz | conflict logged in Q2, AMD preferred, host check decides |
| WikiChip, TechPowerUp | direct fetch | refused, 403 |
| Debian 13 `vm.max_map_count` as shipped | Debian sources | not stated; kernel default 65530 only. On host list |
| Origin of zmm instructions in ArcticDB wheel | binary scan only | runtime gating not established from source; smoke test on host list |
| QuestDB formula for mapping count per table | docs, source | not stated |
| Whether QuestDB keeps column mappings open between queries | docs | not stated. Matters for Q3: mapped pages resist eviction, so stop the process |
| Ingest RAM for QuestDB, ArcticDB; Arrow table size for ITCH | see Q6 | not stated |
| dh-docs/uv-env.md, dh-docs/pyarrow-parquet.rst | cached copies are 404 pages | used uv-env.html and arrow-26-tree parquet.rst |
| manpages.debian.org util-linux path for fincore | 404 | used trixie/fincore.1 page |
| downloads/itch-faq/ITCH_FAQ.pdf | opened | HTML stub. Wayback copy used |
| dh-docs/lmdb-mdb.c, lmdb.h | opened | wrong version (0.9.70, 2015). lmdb-src used, see Q4 |

## To verify on the host

Run later, once the toolchain is ready. All read-only, no sudo.

```sh
# OS and toolchain (Q1)
getconf GNU_LIBC_VERSION; python3 --version; uname -r
/usr/sbin/ldconfig -p | grep -E 'libz\.so\.1|libstdc\+\+\.so\.6|libgcc_s\.so\.1'
# CPU (Q2)
lscpu
grep -m2 -E 'cpu family|^model\s' /proc/cpuinfo
grep -m1 '^flags' /proc/cpuinfo | tr ' ' '\n' | grep -E '^(avx2|bmi2|avx512|constant_tsc|nonstop_tsc|rdtscp)'
grep . /sys/devices/system/cpu/cpu0/cache/index*/size
grep . /sys/devices/system/cpu/cpu*/topology/core_cpus_list
# OS limits (Q1)
cat /proc/sys/vm/max_map_count; ulimit -Sn; ulimit -Hn
# Cold cache tools and filesystem (Q3, Q4)
command -v perf fincore taskset
findmnt -no FSTYPE,OPTIONS -T ~/quant-projects
lsblk -d -o NAME,MODEL,ROTA,TRAN
df -h ~/quant-projects
# Timing (Q5)
cat /proc/sys/kernel/perf_event_paranoid
cat /sys/devices/system/cpu/cpu0/cpufreq/{scaling_driver,scaling_governor,cpuinfo_max_freq}
cat /sys/devices/system/cpu/cpufreq/boost
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
# Memory (Q6)
free -b; grep -E 'MemAvailable|^Cached' /proc/meminfo
cat /proc/self/cgroup; systemctl --user is-system-running
```

Behaviour checks, in the venv and install tree under ~/quant-projects:

- SIGILL smoke test: `python -c "import pyarrow, pyarrow.parquet, arcticdb; print(pyarrow.__version__, arcticdb.__version__)"`, then run one LMDB write/read.
- QuestDB start: grep the rolling log for `linux-x86-64 \[`, `fs.file-max checked` and `vm.max_map_count checked` to confirm which SIMD tier it chose and whether limit warnings appear. (Corrected on the host: the original pattern matched only `max_map`.)
- Cold cache proof: on a small test file, run fsync, `posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED)`, then `fincore <file>` from the owning user, with every process using it stopped. If `fincore` is missing, use a `mincore` helper.
- TSC: calibrate `rdtscp` against `CLOCK_MONOTONIC_RAW` under both boost states observed.
- perf: `perf stat -e cycles:u,instructions:u -- true`, confirm counters are supported, not just permitted.
- Memory: after each ingest, record `VmHWM` and stored bytes.
