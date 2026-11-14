#!/usr/bin/env python3
"""Write a G-3 host's host.json, the facts scripts/fuzz_results.py puts in the fuzzing file.

    python3 -I scripts/fuzz_host.py --build build/fuzz --host macOS|Linux \\
        [--level default|scalar|neon|avx2] --concurrency TEXT --peak-memory TEXT \\
        [--launch fuzz/launch.txt] [--out <logs>/host.json]

run on the host the targets ran on, after its scripts/fuzz_g3.py launch. Each fact is read:

    cpu_model             the processor's name, from sysctl on macOS and /proc/cpuinfo on Linux
    kernel                the running kernel, "Darwin <release>" or "Linux <release>"
    compiler              the first line of --version of the compiler the build used
    compile_flags         "<preset> preset, <build type>: " with the flags the build compiled
                          fuzz/fuzz_pfor.cpp with, the -DTICK_STORE_ definitions of any of its
                          compile commands, and "targets linked with" fuzz_pfor's link flags,
                          from compile_commands.json and build.ninja
    sanitizer_options     the ASAN_OPTIONS fuzz/launch.txt gives this system and the
                          UBSAN_OPTIONS scripts/fuzz_g3.py sets, with TICK_STORE_SIMD=<level>
                          when --level caps the kernels as fuzz_g3.py's --level did
    default_kernel_level  the level the build's dispatch runs with TICK_STORE_SIMD unset: neon
                          in a NEON build, avx2 in an AVX2 build on a processor with AVX2,
                          scalar otherwise
    concurrency, peak_memory  as given: how the launch ran the targets and what it read of
                          their memory, which no file of the build states

The preset is the one scripts/build_preset.py reads from the build's cache, and it must be
fuzz. A path under the build folder is written as under build/<preset>/, the preset's binary
folder, and one under the repository relative to it. It refuses, exit 1, a file that would
still hold an absolute path or this host's name, and a --host that is not this machine.

Standard library only.
"""

import argparse
import json
import platform
import re
import shlex
import socket
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import build_preset  # noqa: E402
import fuzz_g3  # noqa: E402

PRESET = "fuzz"
SOURCE = "fuzz/fuzz_pfor.cpp"
TARGET = "fuzz/fuzz_pfor"
SYSTEMS = {"macOS": "Darwin", "Linux": "Linux"}
LEVELS = ("default", "scalar", "neon", "avx2")
# Options whose next word is their value: a file, a folder or the target, none a build flag.
WITH_VALUE = ("-o", "-c", "-MF", "-MT", "-MQ", "-I", "-isystem", "-iquote", "-isysroot",
              "--sysroot", "-arch", "-target")
DROPPED = ("-I", "-o", "-MD", "-MMD", "-MF", "-MT", "-MQ", "--sysroot", "-isysroot",
           "-DTICK_STORE_")
# An absolute path: a slash that starts a word or follows = , ; or ( , not one inside a URL.
ABSOLUTE = re.compile(r"(?<![\w:/.~-])/[^\s,;:()'\"]+")


class Refused(Exception):
    pass


def words_of(entry):
    return shlex.split(entry["command"]) if "command" in entry else list(entry["arguments"])


def compile_facts(build):
    commands = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    entries = [entry for entry in commands if Path(entry["file"]).as_posix().endswith(SOURCE)]
    if len(entries) != 1:
        raise Refused(f"compile_commands.json has {len(entries)} commands for {SOURCE}, not one")
    words = words_of(entries[0])
    kept, skip = [], False
    for word in words[1:]:
        if skip:
            skip = False
        elif word in WITH_VALUE:
            skip = True
        elif word.startswith("-") and not word.startswith(DROPPED):
            kept.append(word)
    defines = sorted({word for entry in commands for word in words_of(entry)
                      if word.startswith("-DTICK_STORE_")})
    return words[0], kept, defines


def link_flags(build):
    text = (build / "build.ninja").read_text(encoding="utf-8", errors="replace")
    block = re.search(rf"^build {re.escape(TARGET)}: .*\n((?:  .*\n)*)", text, re.M)
    flags = block and re.search(r"^  LINK_FLAGS = (.*)$", block.group(1), re.M)
    if not flags:
        raise Refused(f"build.ninja has no link of {TARGET} with LINK_FLAGS")
    # The dependency file the linker writes is the build's bookkeeping, not a flag of the link.
    return re.sub(r"\s*-Xlinker --dependency-file=\S+", "", flags.group(1)).split()


def relative(text, build, preset):
    """Paths under the build folder as under build/<preset>/, and the repository's relative."""
    def one(match):
        path = Path(match.group(0))
        for top, prefix in ((build.resolve(), f"build/{preset}"), (REPO_ROOT, None)):
            try:
                rest = path.resolve().relative_to(top).as_posix()
            except ValueError:
                continue
            return f"{prefix}/{rest}" if prefix else rest
        return match.group(0)
    return ABSOLUTE.sub(one, text)


def run_text(command):
    done = subprocess.run(command, capture_output=True, text=True, check=False)
    return done.stdout.strip() if done.returncode == 0 else ""


def cpuinfo():
    path = Path("/proc/cpuinfo")
    return path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""


def cpu_model(system):
    if system == "Darwin":
        model = run_text(["sysctl", "-n", "machdep.cpu.brand_string"])
    else:
        found = re.search(r"^model name\s*:\s*(.+)$", cpuinfo(), re.M)
        model = found.group(1).strip() if found else ""
    if not model:
        raise Refused("this host does not state its processor's name")
    return model


def has_avx2(system):
    if system == "Darwin":
        return run_text(["sysctl", "-n", "hw.optional.avx2_0"]) == "1"
    flags = re.search(r"^flags\s*:(.*)$", cpuinfo(), re.M)
    return bool(flags) and "avx2" in flags.group(1).split()


def default_level(defines, system):
    if "-DTICK_STORE_SIMD_NEON" in defines:
        return "neon"
    if "-DTICK_STORE_SIMD_AVX2" in defines and has_avx2(system):
        return "avx2"
    return "scalar"


def facts_of(args):
    system = platform.system()
    if SYSTEMS[args.host] != system:
        raise Refused(f"this machine is not {args.host}: it runs {system}, and host.json "
                      "states the host the targets ran on")
    build = args.build
    preset = build_preset.preset_of(build)
    if preset != PRESET:
        raise Refused(f"{build} is the {preset} preset, not the {PRESET} preset the targets "
                      "are built with")
    build_type = build_preset.read_cache(build).get("CMAKE_BUILD_TYPE")
    compiler, flags, defines = compile_facts(build)
    version = run_text([compiler, "--version"]).splitlines()
    if not version:
        raise Refused(f"the build's compiler did not print its version: {compiler}")
    launch = fuzz_g3.read_launch(args.launch)
    asan = launch["asan_options"].get(system)
    if not asan:
        raise Refused(f"{args.launch} gives no asan_options for {system}")
    options = f"ASAN_OPTIONS={asan} UBSAN_OPTIONS={fuzz_g3.UBSAN_OPTIONS}"
    if args.level != "default":
        options += f" TICK_STORE_SIMD={args.level}"
    compile_flags = (f"{preset} preset, {build_type}: {' '.join(flags)}, {' '.join(defines)}; "
                     f"targets linked with {' '.join(link_flags(build))}")
    return {"cpu_model": cpu_model(system), "kernel": f"{system} {platform.release()}",
            "compiler": version[0],
            "compile_flags": relative(compile_flags, build, preset),
            "sanitizer_options": f"{options}, from fuzz/launch.txt, which scripts/fuzz_g3.py "
                                 "applies",
            "concurrency": args.concurrency, "peak_memory": args.peak_memory,
            "default_kernel_level": default_level(defines, system)}


def refuse_leaks(facts):
    """No absolute path and no name of this host anywhere in the file: it is published."""
    problems = []
    names = {socket.gethostname().strip().lower()}
    names |= {name.split(".")[0] for name in names}
    for key, value in facts.items():
        for path in ABSOLUTE.findall(str(value)):
            problems.append(f"{key} holds the absolute path {path}")
        if any(name and len(name) > 1 and name in str(value).lower() for name in names):
            problems.append(f"{key} holds the host's name")
    if problems:
        raise Refused("; ".join(problems))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--build", type=Path, required=True, help="the fuzz build folder")
    parser.add_argument("--host", choices=sorted(SYSTEMS), required=True)
    parser.add_argument("--level", choices=LEVELS, default="default",
                        help="the TICK_STORE_SIMD cap fuzz_g3.py's --level set, if any")
    parser.add_argument("--concurrency", required=True)
    parser.add_argument("--peak-memory", required=True)
    parser.add_argument("--launch", type=Path, default=fuzz_g3.LAUNCH)
    parser.add_argument("--out", type=Path, help="host.json to write; stdout if not given")
    args = parser.parse_args(argv)
    try:
        facts = facts_of(args)
        refuse_leaks(facts)
    except (Refused, build_preset.Refused, OSError, ValueError, KeyError) as error:
        print(f"fuzz_host: {error}", file=sys.stderr)
        return 1
    text = json.dumps(facts, indent=1) + "\n"
    if args.out:
        args.out.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
