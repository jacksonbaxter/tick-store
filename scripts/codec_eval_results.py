#!/usr/bin/env python3
"""Write a published codec evaluation file from tick_codec_eval's output and its memory.

    /usr/bin/time -l -o eval.time build/bench/tools/tick_codec_eval <day>.NASDAQ_ITCH50 \\
        codec-eval.tool.json --day <day> --prove-all-price4
    python3 -I scripts/codec_eval_results.py --tool-output codec-eval.tool.json \\
        --time eval.time --build build/bench --out results/codec-eval-<day>.json

The file is the tool's own output, every key and value as it wrote them, with four keys
placed right after `sanitizers`, where the committed results/codec-eval-*.json have them:

    build_preset                 the CMakePresets.json preset that configured --build, read
                                 from its CMakeCache.txt by scripts/build_preset.py
    peak_rss_bytes               BSD time -l's maximum resident set size around the run
    peak_rss_note                what that figure counts
    peak_memory_footprint_bytes  BSD time -l's peak memory footprint

It refuses, exit 1, output that is not tick_codec_eval's or already holds these keys, a build
folder no preset configured or whose build type is not the tool's, and a time report without
both BSD figures. No other script reads the four keys; they are for the reader.

Standard library only.
"""

import argparse
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import build_preset  # noqa: E402
import store_results  # noqa: E402

TOOL = "tick_codec_eval"
AFTER = "sanitizers"
ADDED = ("build_preset", "peak_rss_bytes", "peak_rss_note", "peak_memory_footprint_bytes")
NOTE = ("Peak resident size counts the clean file-backed pages of the read-only mapping of the "
        "source. peak_memory_footprint_bytes is what the process held beyond that mapping. Both "
        "are from /usr/bin/time -l around the whole run, the two exhaustive Price(4) proofs "
        "included.")


class Refused(Exception):
    pass


def merged(tool, time_text, build):
    if not isinstance(tool, dict) or tool.get("tool") != TOOL or AFTER not in tool:
        raise Refused(f"the tool output is not {TOOL}'s output: no tool {TOOL!r} and "
                      f"{AFTER!r}")
    held = [key for key in ADDED if key in tool]
    if held:
        raise Refused(f"the tool output already holds {', '.join(held)}")
    preset = build_preset.preset_of(build)
    cached = build_preset.read_cache(build).get("CMAKE_BUILD_TYPE")
    if tool.get("build_type") != cached:
        raise Refused(f"the tool states build_type {tool.get('build_type')!r} and {build} was "
                      f"configured {cached!r}, so it is not the build that ran")
    figures = store_results.parse_time(time_text)
    if figures is None or figures["peak_memory_footprint_bytes"] is None:
        raise Refused("the time report holds no BSD time -l maximum resident set size and peak "
                      "memory footprint")
    out = {}
    for key, value in tool.items():
        out[key] = value
        if key == AFTER:
            out.update({"build_preset": preset, "peak_rss_bytes": figures["peak_rss_bytes"],
                        "peak_rss_note": NOTE,
                        "peak_memory_footprint_bytes": figures["peak_memory_footprint_bytes"]})
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--tool-output", type=Path, required=True)
    parser.add_argument("--time", type=Path, required=True,
                        help="what /usr/bin/time -l wrote around the tool's run")
    parser.add_argument("--build", type=Path, required=True,
                        help="the build folder the tool was built in")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        tool = json.loads(args.tool_output.read_text(encoding="utf-8"))
        document = merged(tool, args.time.read_text(encoding="utf-8"), args.build)
    except (Refused, build_preset.Refused, OSError, ValueError) as error:
        print(f"codec_eval_results: {error}", file=sys.stderr)
        return 1
    args.out.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(args.out, document["build_preset"], document["peak_rss_bytes"],
          document["peak_memory_footprint_bytes"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
