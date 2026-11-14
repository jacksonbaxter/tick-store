#!/usr/bin/env python3
"""Name the CMakePresets.json configure preset a build folder was configured with.

    python3 -I scripts/build_preset.py BUILD_DIR

prints the preset's name, read from BUILD_DIR/CMakeCache.txt. A preset matches when every
cache variable it sets (inherited ones included) has that value in the cache, and every option
CMakeLists.txt declares that it does not set keeps its default there. A value naming a preset
macro, such as base's FETCHCONTENT_BASE_DIR, depends on where the source is and is not
compared. Exactly one preset must match: a folder configured with other values is refused,
exit 1, since no preset is what built it. scripts/codec_eval_results.py and
scripts/fuzz_host.py read the preset this way rather than assuming it.

Standard library only.
"""

import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
PRESETS = REPO_ROOT / "CMakePresets.json"
LISTS = REPO_ROOT / "CMakeLists.txt"

CACHE_LINE = re.compile(r"^([A-Za-z_][A-Za-z0-9_.+-]*)(?::[A-Z]+)?=(.*)$")
OPTION = re.compile(r'^\s*option\(\s*(\w+)\s+"[^"]*"\s+(ON|OFF)\s*\)', re.M)
CACHED_SET = re.compile(r'^\s*set\(\s*(\w+)\s+"([^"$]*)"\s+CACHE\b', re.M)
TRUE_WORDS = {"ON", "TRUE", "YES", "Y", "1"}
FALSE_WORDS = {"OFF", "FALSE", "NO", "N", "0", ""}


class Refused(Exception):
    pass


def read_cache(build):
    path = Path(build) / "CMakeCache.txt"
    if not path.is_file():
        raise Refused(f"{build} holds no CMakeCache.txt, so it is no configured build folder")
    cache = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(("#", "//")):
            continue
        match = CACHE_LINE.match(line)
        if match:
            cache[match.group(1)] = match.group(2)
    return cache


def same(cached, wanted):
    """CMake's own reading of a cache value against a preset's: booleans by truth."""
    if cached is None:
        return False
    words = (cached.upper(), wanted.upper())
    if all(word in TRUE_WORDS | FALSE_WORDS for word in words) and wanted.upper() != "":
        return (words[0] in TRUE_WORDS) == (words[1] in TRUE_WORDS)
    return cached == wanted


def configure_presets(presets=PRESETS):
    """{name: {variable: value}} for every preset a user can configure, inheritance resolved."""
    listed = {entry["name"]: entry for entry in
              json.loads(Path(presets).read_text(encoding="utf-8"))["configurePresets"]}

    def variables(name):
        entry = listed[name]
        inherits = entry.get("inherits", [])
        resolved = {}
        for parent in [inherits] if isinstance(inherits, str) else inherits:
            for key, value in variables(parent).items():
                resolved.setdefault(key, value)
        for key, value in entry.get("cacheVariables", {}).items():
            resolved[key] = value["value"] if isinstance(value, dict) else value
        return resolved

    return {name: variables(name) for name, entry in listed.items() if not entry.get("hidden")}


def option_defaults(lists=LISTS):
    text = Path(lists).read_text(encoding="utf-8")
    defaults = dict(OPTION.findall(text))
    defaults.update(dict(CACHED_SET.findall(text)))
    return defaults


def matches(cache, variables, defaults):
    for key, value in variables.items():
        if isinstance(value, bool):
            value = "ON" if value else "OFF"
        if "$" in str(value):
            continue
        if not same(cache.get(key), str(value)):
            return False
    for key, value in defaults.items():
        if key not in variables and not same(cache.get(key), value):
            return False
    return True


def preset_of(build, presets=PRESETS, lists=LISTS):
    cache = read_cache(build)
    defaults = option_defaults(lists)
    found = [name for name, variables in configure_presets(presets).items()
             if matches(cache, variables, defaults)]
    if len(found) != 1:
        shown = {key: cache.get(key) for key in sorted(
            {key for variables in configure_presets(presets).values() for key in variables}
            | set(defaults)) if key in cache}
        if not found:
            raise Refused(f"{build} was configured by no preset in CMakePresets.json: {shown}")
        raise Refused(f"{build} matches more than one preset, {', '.join(found)}: {shown}")
    return found[0]


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print("usage: build_preset.py BUILD_DIR", file=sys.stderr)
        return 2
    try:
        print(preset_of(argv[0]))
    except Refused as error:
        print(f"build_preset: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
