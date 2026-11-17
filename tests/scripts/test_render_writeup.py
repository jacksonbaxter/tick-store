#!/usr/bin/env python3
"""Tests for scripts/render_writeup.py. Run with:

    python3 -I tests/scripts/test_render_writeup.py

Each test builds a small tree of its own under the repo's gitignored build/ directory: a
template, a figure list, a targets file, a results file and a source file with a constant, and
runs the script on it as check.sh does. Each refusal is shown on a tree that differs from a good
one in one place, and so are the two rules the page lives by: a hand-edited figure fails --check,
and a tree whose results are not all committed is not judged, which check.sh reports as a skip and
fails under --no-skips. Forms computes each of the figure list's expressions on a document whose
answer is worked out here in plain Python, so an operator swapped, inverted or dropped changes a
value a test holds; Verdicts holds render_results.py's verdict code on hand-built parts, apart
from either renderer.
"""

import ast
import copy
import hashlib
import importlib.util
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "render_writeup.py"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_render_writeup"
sys.path.insert(0, str(REPO_ROOT / "scripts"))
import check_hygiene  # noqa: E402

_SPEC = importlib.util.spec_from_file_location("render_writeup", SCRIPT)
RENDER = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(RENDER)
RESULTS_CODE = RENDER.render_results

COMMIT = "0123456789ab" + "c" * 28
RESULT = "results/bench/macOS-demo-0123456789ab.json"
SOURCE = "include/demo.hpp"
CONSTANT = "inline constexpr std::size_t kHeaderSize = 16;\n"
# Words enough for the essay's range, with no number in them.
FILLER = " ".join(["the store keeps each stock's records together"] * 400) + "."
OPENING = """The demo decodes at {{speed}} on the M4, against a bar of {{bar:t1}}, and the target
is {{verdict:t1}}.
It ran {{count}} times on ITCH 5.0, at commit `{{measured_commit}}`{{commit_exceptions}}.
Its header takes {{code:header}} bytes ([results]({{path:demo}}))."""

TEMPLATE = f"""# A demo essay

{OPENING}
<!-- a note for whoever edits this, with a 99 in it -->

## Method

{FILLER}
"""

FIGURES = {
    "schema": RENDER.SCHEMA,
    "not_results": [{"text": "ITCH 5.0", "why": "the feed's version",
                     "in": ["times on ITCH 5.0, at"]}],
    "factors": [{"value": 0.001, "why": "microseconds to milliseconds"}],
    "kinds": {"demo": {"what": "the demo benchmark",
                       "files": {"macOS": "results/bench/macOS-demo-*.json"}}},
    "figures": {
        "speed": {"what": "the demo's median on the M4",
                  "target": {"id": "T-1", "host": "macOS", "part": "demo"}, "format": "{:.2f}"},
        "count": {"what": "the demo's runs on the M4", "kind": "demo", "host": "macOS",
                  "value": {"path": ["runs"]}, "format": "{:,}"},
        "path:demo": {"what": "the demo's file", "kind": "demo", "host": "macOS", "path": True},
        "code:header": {"what": "the header's bytes", "format": "{:d}", "value": {"constant": {
            "source": SOURCE,
            "pattern": r"^inline constexpr std::size_t kHeaderSize = (\d+);$"}}},
        "bar:t1": {"what": "T-1's target", "bar": {"id": "T-1", "host": "macOS"},
                   "format": "{:.1f}"},
        "verdict:t1": {"what": "T-1's verdict on the M4", "verdict": {"id": "T-1",
                                                                      "host": "macOS"}},
        "measured_commit": {"what": "the commit the figures were measured at",
                            "measured_commit": True},
        "commit_exceptions": {"what": "the files measured elsewhere", "commit_exceptions": True},
    },
}

TARGETS = {"rows": [{
    "id": "T-1", "host": "macOS", "target": {"at_most": 2.5}, "digits": 2, "unit": " ms",
    "judged": "the demo's median",
    "parts": [{"label": "demo", "file": "results/bench/macOS-demo-*.json",
               "value": {"div": [{"path": ["median_us"]}, 1000]}}]}]}

RESULTS = {"header": {"commit": COMMIT}, "median_us": 1234.5, "runs": 30}


class Tree(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.root)
        self.figures = copy.deepcopy(FIGURES)
        self.targets = copy.deepcopy(TARGETS)
        self.write("docs/writeup/WRITEUP.template.md", TEMPLATE)
        self.write(RESULT, json.dumps(RESULTS))
        self.write(SOURCE, "// demo\n" + CONSTANT)
        self.save()

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def save(self):
        self.write("docs/writeup/figures.json", json.dumps(self.figures, indent=1))
        self.write("scripts/targets.json", json.dumps(self.targets))

    def edit(self, name, old, new):
        path = self.root / name
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def insert(self, text):
        """Put a paragraph into the template before its Method section."""
        self.edit("docs/writeup/WRITEUP.template.md", "## Method", text + "\n\n## Method")

    def figure(self, name, entry):
        self.figures["figures"][name] = entry
        self.save()

    def run_script(self, *arguments):
        return subprocess.run([sys.executable, "-I", str(SCRIPT), "--root", str(self.root),
                               *arguments], capture_output=True, text=True)

    def refused(self, *needles, arguments=("--check",)):
        done = self.run_script(*arguments)
        self.assertEqual(done.returncode, RENDER.EXIT_REFUSED, done.stdout + done.stderr)
        for needle in needles:
            self.assertIn(needle, done.stderr)
        return done

    def renders(self):
        done = self.run_script()
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        return self.page()

    def page(self):
        return (self.root / "WRITEUP.md").read_text(encoding="utf-8")

    def rendered(self):
        return RENDER.render(self.root)


class Renders(Tree):
    def test_a_tree_whose_tokens_resolve_renders_a_page_check_calls_current(self):
        page = self.renders()
        self.assertIn("The demo decodes at 1.23 ms on the M4, against a bar of ≤ 2.5 ms, and "
                      "the target\nis met.", page)
        self.assertIn("It ran 30 times on ITCH 5.0, at commit `0123456789ab`.", page)
        self.assertIn(f"Its header takes 16 bytes ([results]({RESULT})).", page)
        self.assertNotIn("a note for whoever edits this", page)
        self.assertTrue(page.endswith(RENDER.MARKER + "\n"))
        done = self.run_script("--check")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("WRITEUP.md is current: 8 tokens", done.stdout)

    def test_a_hand_edited_number_in_the_page_fails_check(self):
        self.renders()
        self.edit("WRITEUP.md", "decodes at 1.23 ms", "decodes at 1.22 ms")
        done = self.run_script("--check")
        self.assertEqual(done.returncode, RENDER.EXIT_STALE)
        self.assertIn("WRITEUP.md differs from a fresh render", done.stderr)

    def test_a_page_left_behind_by_a_new_result_fails_check(self):
        self.renders()
        self.write(RESULT, json.dumps(dict(RESULTS, median_us=2000.0)))
        self.assertEqual(self.run_script("--check").returncode, RENDER.EXIT_STALE)

    def test_check_with_every_token_resolved_and_no_page_fails(self):
        done = self.run_script("--check")
        self.assertEqual(done.returncode, RENDER.EXIT_STALE)
        self.assertIn("every token resolves, and there is no WRITEUP.md", done.stderr)

    def test_a_file_measured_again_with_a_new_commit_in_its_name_still_resolves(self):
        (self.root / RESULT).unlink()
        self.write("results/bench/macOS-demo-fedcba987654.json",
                   json.dumps(dict(RESULTS, header={"commit": "fedcba987654" + "0" * 28})))
        page = self.renders()
        self.assertIn("(results/bench/macOS-demo-fedcba987654.json)", page)
        self.assertIn("at commit `fedcba987654`.", page)

    def test_the_verdict_is_render_results_own_code(self):
        # The token's word is what render_results.verdict_class says of the parts that
        # render_results.judged_parts judges, so the essay and the Targets block cannot differ.
        original = RESULTS_CODE.verdict_class
        seen = []

        def verdict_class(parts):
            seen.append([part[:3] for part in parts])
            return "the targets block's word"

        RESULTS_CODE.verdict_class = verdict_class
        try:
            rendered = self.rendered()
        finally:
            RESULTS_CODE.verdict_class = original
        self.assertEqual(rendered.values["verdict:t1"], "the targets block's word")
        self.assertEqual(seen[0], [("demo", "1.23 ms", True)])

    def test_a_missed_target_is_shown_missed(self):
        self.write(RESULT, json.dumps(dict(RESULTS, median_us=3000.0)))
        self.assertIn("the target\nis missed.", self.renders())

    def test_a_draft_is_never_the_page(self):
        done = self.run_script("--draft", str(self.root / "WRITEUP.md"))
        self.assertEqual(done.returncode, RENDER.EXIT_REFUSED)
        self.assertIn("--draft is never the page", done.stderr)

    def test_the_target_figure_follows_its_row(self):
        # Review WD-6: the measured figure is read through the row, at its scale and in its
        # unit, so a target restated in targets.json moves the figure beside its bar.
        self.targets["rows"][0]["parts"][0]["value"] = {"div": [{"path": ["median_us"]}, 1024]}
        self.targets["rows"][0]["unit"] = " ticks"
        self.save()
        page = self.renders()
        self.assertIn("decodes at 1.21 ticks on the M4, against a bar of ≤ 2.5 ticks", page)


class Refuses(Tree):
    def test_a_sentence_saying_a_write_is_faster_than_a_disk_read_clock(self):
        # Group 3, ruling 3: ArcticDB's input was not resident when its write's clock started,
        # so no sentence of the page may say tick-store writes faster than it.
        start = {"files": ["cols"], "pages": 10, "resident_pages": 1,
                 "made_ready": {"what": "nothing"}}
        system = {"headline_variant": "h", "variants": {"h": {"classes": {"B": {"write": {
            "detail": {"input_at_clock_start": start}}}}}}}
        self.write("results/compare/Linux-day-fedcba987654.json", json.dumps(
            {"header": {"host": "Linux"}, "systems": {"tickstore": {}, "arcticdb": system}}))
        self.edit("docs/writeup/WRITEUP.template.md", "## Method",
                  "It writes the day faster than ArcticDB.\n\n## Method")
        self.refused("says a write is 'faster' than ArcticDB's, whose clock holds its disk reads")

    def test_an_unknown_token(self):
        self.insert("{{nowhere}}")
        self.refused("the template's {{nowhere}} is not in the figure list")

    def test_a_stray_brace_pair(self):
        self.insert("{{not a token}}")
        self.refused("which is no token")

    def test_an_unused_entry(self):
        self.figure("spare", dict(self.figures["figures"]["count"]))
        self.refused("the figure list's 'spare' is used nowhere in the template")

    def test_a_helper_no_entry_uses(self):
        self.figure("spare", dict(self.figures["figures"]["count"], helper=True))
        self.refused("helper 'spare' is used by no entry")

    def test_a_helper_in_the_template(self):
        self.figures["figures"]["count"]["helper"] = True
        self.figure("twice_count", {"what": "the runs on the M4", "value": {"figure": "count"},
                                    "format": "{:,}", "helper": True})
        self.refused("helper 'count' is in the template")

    def test_a_kind_that_matches_two_files(self):
        self.write("results/bench/macOS-demo-fedcba987654.json", json.dumps(RESULTS))
        # The render's own lookup must refuse, naming the token: the verdict's read through
        # render_results refuses too, and alone would hide a render that took the first file.
        self.refused("results/bench/macOS-demo-*.json matches 2 files", ", and a token reads one")

    def test_a_kind_named_by_a_commit(self):
        self.figures["kinds"]["demo"]["files"]["macOS"] = RESULT
        self.save()
        self.refused("names the commit '0123456789ab'", "name the kind with * in its place")

    def test_a_kind_named_by_a_commit_prefix_or_folder_or_leaving_results(self):
        # Review WD-9: a prefix of a commit beside the *, a commit in a folder, a second glob, a
        # path out of results/: each could pick an old file over a new one, or no result.
        for pattern, needle in (
                ("results/bench/macOS-demo-012345*.json", "has a * other than one in place"),
                ("results/bench/macOS-demo-0123456*.json", "has a * other than one in place"),
                ("results/0123456789ab/macOS-demo-*.json", "names a commit or a glob in a folder"),
                ("results/bench/*/macOS-demo-*.json", "names a commit or a glob in a folder"),
                ("results/../WRITEUP.md", "leaves results/"),
                ("results/bench/macOS-demo-?.json", "leaves results/ or uses a glob"),
                ("docs/macOS-demo-*.json", "is not a pattern under results/"),
                ("results/bench/macOS-*-demo-*.json", "has a * other than one in place")):
            with self.subTest(pattern=pattern):
                self.figures["kinds"]["demo"]["files"]["macOS"] = pattern
                self.save()
                self.refused(needle)

    def test_a_day_in_a_name_is_no_commit(self):
        RENDER.check_pattern("day", "results/roundtrip-12302019-*.json")
        RENDER.check_pattern("day", "results/store-size-12302019.json")

    def test_the_list_naming_its_page_template_or_targets(self):
        # Review WD-10: the page, the template and the targets are fixed in the script.
        for key, value in (("page", "docs/OTHER.md"), ("template", "docs/other.template.md"),
                           ("targets", "scripts/other.json")):
            with self.subTest(key=key):
                figures = dict(self.figures, **{key: value})
                self.write("docs/writeup/figures.json", json.dumps(figures))
                self.refused(f"the figure list names its {key}")

    def test_a_kind_with_no_file_on_the_host(self):
        self.figures["figures"]["count"]["host"] = "Linux"
        self.save()
        self.refused("kind 'demo' has no file on the host 'Linux'")

    def test_an_unknown_kind(self):
        self.figures["figures"]["count"]["kind"] = "nowhere"
        self.save()
        self.refused("{{count}}: no kind 'nowhere' in the figure list")

    def test_a_pending_kind_with_no_reason(self):
        self.figures["kinds"]["later"] = {"what": "a file to come", "pending": ""}
        self.figure("count", dict(self.figures["figures"]["count"], kind="later", host="any"))
        self.refused("kind 'later': pending with no reason")

    def test_a_kind_that_says_nothing_of_its_files(self):
        del self.figures["kinds"]["demo"]["what"]
        self.save()
        self.refused("kind 'demo' says nothing of what its files are")

    def test_an_entry_that_says_nothing_of_itself(self):
        del self.figures["figures"]["count"]["what"]
        self.save()
        self.refused("{{count}}: its entry says nothing of what it is")

    def test_an_entry_with_no_value(self):
        del self.figures["figures"]["count"]["value"]
        self.save()
        self.refused("{{count}}: its entry gives no value, bar, target, verdict or path")

    def test_a_figure_computed_from_itself(self):
        self.figure("count", dict(self.figures["figures"]["count"],
                                  value={"add": [{"figure": "count"}, {"path": ["runs"]}]}))
        self.refused("{{count}} is computed from itself")

    def test_an_unreadable_figure_list_or_results_file(self):
        self.write(RESULT, "{not json")
        self.refused("cannot read results/bench/macOS-demo-0123456789ab.json")
        self.write("docs/writeup/figures.json", "{not json")
        self.refused("cannot read docs/writeup/figures.json")

    def test_a_list_of_another_schema(self):
        self.figures["schema"] = "tick-store-writeup-figures-1"
        self.save()
        self.refused("is not a tick-store-writeup-figures-2 figure list")

    def test_a_value_outside_its_format(self):
        self.write(RESULT, json.dumps(dict(RESULTS, runs=30.5)))
        self.refused("{{count}}: 30.5 is not a whole number, and {:,} writes one")

    def test_text_where_a_number_goes(self):
        self.write(RESULT, json.dumps(dict(RESULTS, runs="many")))
        self.refused("{{count}}: 'many' is not a number to write as {:,}")

    def test_an_unknown_format_and_a_scale_on_a_whole_number(self):
        self.figures["figures"]["count"]["format"] = "{:x}"
        self.save()
        self.refused("{{count}}: '{:x}' is no format")
        self.figures["figures"]["count"].update(format="{:,}", scale=0.001)
        self.save()
        self.refused("a whole-number format {:,} takes no scale")

    def test_a_number_that_is_not_finite(self):
        self.figure("count", dict(self.figures["figures"]["count"], format="{:.1f}",
                                  value={"div": [{"path": ["runs"]}, {"path": ["zero"]}]}))
        self.write(RESULT, json.dumps(dict(RESULTS, zero=0.0)))
        self.refused("division by zero")
        self.write(RESULT, json.dumps(dict(RESULTS, runs=float("inf"))))
        self.figure("count", dict(self.figures["figures"]["count"], value={"path": ["runs"]}))
        self.refused("is not a finite number")

    def test_a_field_the_file_does_not_have(self):
        self.write(RESULT, json.dumps({"header": {"commit": COMMIT}, "median_us": 1.0}))
        self.refused("{{count}}: no 'runs' on the path")

    def test_a_typed_number_in_the_template(self):
        self.insert("It ran 12 times.")
        self.refused("the template types the number '12' by hand")

    def test_a_typed_number_in_a_table(self):
        self.insert("| What | Bar |\n|---|---|\n| demo | 2.5 ms |")
        self.refused("the template types the number '2.5' by hand")

    def test_a_number_in_words(self):
        # Review WD-3: a result written in words is a typed number.
        for words in ("It ran four times as long.", "It took half the time.",
                      "It ran twice.", "It read a million values.", "It ran one pass."):
            with self.subTest(words=words):
                self.setUp()
                self.insert(words)
                self.refused("counts with")

    def test_digits_in_a_name_are_no_number(self):
        self.insert("On the M4 with AVX2 off, Q1 ran.")
        self.renders()

    def test_a_non_result_with_no_reason_or_no_place(self):
        self.figures["not_results"][0]["why"] = ""
        self.save()
        self.refused("'ITCH 5.0' is kept out of the number check with no reason")
        self.figures["not_results"][0].update(why="the feed's version", **{"in": []})
        self.save()
        self.refused("'ITCH 5.0' names no place it may appear")

    def test_a_non_result_the_template_does_not_hold_where_declared(self):
        self.figures["not_results"].append({"text": "2019", "why": "a year",
                                            "in": ["the year 2019"]})
        self.save()
        self.refused("'2019' is declared in 'the year 2019', which the template holds 0 times")

    def test_a_non_result_used_outside_its_place(self):
        # Review WD-14: a declared text is taken out only where it is declared.
        self.figures["not_results"].append({"text": "2018", "why": "the processor's year",
                                            "in": ["made in 2018"]})
        self.save()
        self.insert("The processor was made in 2018. Its slowest query took 2018 seconds.")
        self.refused("'2018' appears outside the places declared for it")

    def test_a_non_result_shaped_like_a_result(self):
        for text in ("15 GB of memory", "within 5% of the best", "2.5 ms", "3 times"):
            with self.subTest(text=text):
                self.setUp()
                self.figures["not_results"].append({"text": text, "why": "a fact",
                                                    "in": [f"has {text} here"]})
                self.save()
                self.insert(f"It has {text} here.")
                self.refused(f"not_results: {text!r} holds a number with a unit")

    def test_a_non_result_declared_where_it_occurs_twice(self):
        self.figures["not_results"].append({"text": "2018", "why": "the processor's year",
                                            "in": ["in 2018"]})
        self.save()
        self.insert("Made in 2018. Sold in 2018.")
        self.refused("'2018' is declared in 'in 2018', which the template holds 2 times")

    def test_tokens_side_by_side_or_against_a_digit(self):
        # Review WD-17: two tokens or a token and a digit print as one number.
        for text in ("It ran {{count}}{{count}} times.", "It ran {{count}}0 times."):
            with self.subTest(text=text):
                self.setUp()
                self.insert(text)
                self.refused("which prints as one number")

    def test_a_character_reference(self):
        for text in ("It took &#x33; s.", "It took &#51; s.", "It took &frac12; s."):
            with self.subTest(text=text):
                self.setUp()
                self.insert(text)
                self.refused("the character reference")

    def test_a_changed_constant_fails_check(self):
        self.renders()
        self.edit(SOURCE, "kHeaderSize = 16;", "kHeaderSize = 20;")
        self.assertEqual(self.run_script("--check").returncode, RENDER.EXIT_STALE)

    def test_a_constant_that_changed_form(self):
        self.edit(SOURCE, CONSTANT, "inline constexpr std::size_t kHeaderSize{16};\n")
        self.refused("matches 0 lines of code in include/demo.hpp, not one: the constant moved "
                     "or changed form")

    def test_a_constant_that_moved(self):
        (self.root / SOURCE).unlink()
        self.write("include/other.hpp", CONSTANT)
        self.refused("{{code:header}}: cannot read include/demo.hpp")

    def test_a_constant_found_twice(self):
        self.write(SOURCE, CONSTANT + "// and\n" + CONSTANT)
        self.refused("matches 2 lines of code in include/demo.hpp")

    def test_a_constant_read_from_a_comment(self):
        # Review WD-13: a size is read from code, never from a comment that says it.
        self.write(SOURCE, "// " + CONSTANT + "/*\n" + CONSTANT + "*/\n")
        self.refused("matches 0 lines of code in include/demo.hpp")

    def test_a_constant_still_to_be_merged_waits_and_then_must_drop_its_wait(self):
        self.figures["figures"]["code:header"]["value"]["constant"]["until"] = (
            "the header's change is merged")
        self.save()
        self.write(SOURCE, "// demo\n")
        done = self.run_script("--check")
        self.assertEqual(done.returncode, RENDER.EXIT_UNJUDGED, done.stdout + done.stderr)
        self.assertIn("code (a merge) 1", done.stdout)
        self.write(SOURCE, CONSTANT)
        self.refused("now matches in include/demo.hpp, and its entry still waits")

    def test_a_verdict_part_the_row_does_not_have(self):
        self.figures["figures"]["verdict:t1"]["verdict"]["parts"] = ["elsewhere"]
        self.save()
        self.refused("{{verdict:t1}}: no part ['elsewhere'] in ['demo']")

    def test_a_target_row_that_is_not_one(self):
        self.targets["rows"].append(copy.deepcopy(self.targets["rows"][0]))
        self.save()
        self.refused("no one target row T-1 on macOS")

    def test_a_path_its_file_lists_as_not_established(self):
        # Review DX-6, DX-7: a diagnostic summary lists what its raw output could not show; a
        # token that reads it is refused, one that reads around it is not.
        self.figures["figures"]["count"]["value"] = {"path": ["items", {"name": "a"}, "runs"]}
        self.save()
        items = {"items": [{"name": "a", "runs": 30}, {"name": "b", "runs": 99}]}
        reason = {"what": "the kernels each level ran", "why": "an answer gives no kernel_runs"}
        self.write(RESULT, json.dumps(dict(RESULTS, **items, not_established=[
            dict(reason, path=["items", {"name": "b"}])])))
        self.renders()
        self.write(RESULT, json.dumps(dict(RESULTS, **items, not_established=[
            dict(reason, path=["items", {"name": "a"}])])))
        self.refused("{{count}} reads ['items', {'name': 'a'}, 'runs'], and its file says the "
                     "kernels each level ran is not established there: an answer gives no "
                     "kernel_runs")
        # A file-wide entry covers every token of the file.
        self.write(RESULT, json.dumps(dict(RESULTS, **items, not_established=[
            dict(reason, path=[])])))
        self.refused("{{count}} reads", "is not established there")
        self.write(RESULT, json.dumps(dict(RESULTS, **items, not_established={"x": 1})))
        self.refused("its file's not_established is no list of entries each with a path")

    def test_an_essay_too_short(self):
        self.edit("docs/writeup/WRITEUP.template.md", FILLER, "Short.")
        self.refused("outside its 2,500 to 4,000")

    def test_an_essay_too_long(self):
        self.edit("docs/writeup/WRITEUP.template.md", FILLER, FILLER + "\n\n" + FILLER)
        self.refused("outside its 2,500 to 4,000")

    def test_the_word_count_leaves_out_comments_and_table_rules(self):
        self.assertEqual(RENDER.word_count("A b.\n<!-- c d e -->\n| x | y |\n|---|---|\n"), 4)

    def test_an_essay_with_no_title(self):
        self.edit("docs/writeup/WRITEUP.template.md", "# A demo essay\n\n", "")
        self.refused("the page does not open with its title")

    def test_an_essay_that_opens_with_a_heading(self):
        self.edit("docs/writeup/WRITEUP.template.md", "# A demo essay\n\n",
                  "# A demo essay\n\n## Summary\n\n")
        self.refused("a section heading comes before any prose")

    def test_an_essay_that_opens_with_a_list(self):
        self.edit("docs/writeup/WRITEUP.template.md", "The demo decodes", "- The demo decodes")
        self.refused("the essay opens with a list")

    def test_an_opening_that_is_not_three_sentences_each_with_a_token(self):
        # Review WD-16: the opening is the conclusion, three sentences, each a result.
        self.edit("docs/writeup/WRITEUP.template.md", OPENING, "Read on.\n\n" + OPENING)
        self.refused("the essay's opening paragraph holds 1 sentences")
        self.setUp()
        self.edit("docs/writeup/WRITEUP.template.md", "bytes ([results]({{path:demo}})).",
                  "bytes ([results]({{path:demo}})). That is all.")
        self.refused("the essay's opening paragraph holds 4 sentences")
        self.setUp()
        self.edit("docs/writeup/WRITEUP.template.md",
                  "Its header takes {{code:header}} bytes ([results]({{path:demo}})).",
                  "Read the rest.\n\nIts header takes {{code:header}} bytes "
                  "([results]({{path:demo}})).")
        self.refused("the opening sentence 'Read the rest.' holds no token")

    def test_a_cut_or_cause_note_left_in(self):
        self.insert("<!-- CUT-OR-CAUSE: replace the last sentence -->")
        self.refused("the template still holds 1 CUT-OR-CAUSE notes")


class Commits(Tree):
    """Review WD-1: the essay's one commit is derived from every file it reads."""

    def other(self, commit, exception=None, name="results/bench/macOS-other-fedcba987654.json"):
        kind = {"what": "another benchmark", "files": {"macOS": "results/bench/macOS-other-*.json"}}
        if exception:
            kind["exception"] = exception
        self.figures["kinds"]["other"] = kind
        self.figure("other", {"what": "the other's runs on the M4", "kind": "other",
                              "host": "macOS", "value": {"path": ["runs"]}, "format": "{:,}"})
        self.write(name, json.dumps({"header": {"commit": commit}, "runs": 7}))
        self.insert("The other ran {{other}} times.")

    def test_a_file_at_another_commit_is_refused(self):
        self.other("fedcba987654" + "0" * 28)
        self.refused("the results files read name 0123456789ab (results/bench/macOS-demo-",
                     "fedcba987654 (results/bench/macOS-other-",
                     "declare the kind an exception with its reason")

    def test_a_file_at_the_same_commit_resolves(self):
        self.other(COMMIT)
        self.assertIn("at commit `0123456789ab`.", self.renders())

    def test_an_exception_is_named_in_the_essay_s_own_sentence(self):
        self.other("fedcba987654" + "0" * 28,
                   {"why": "measured at its own commit", "says": "the other benchmark"})
        self.assertIn("at commit `0123456789ab`, except the other benchmark, at `fedcba987654`.",
                      self.renders())

    def test_an_exception_at_the_measured_commit_is_not_named(self):
        self.other(COMMIT, {"why": "measured at its own commit", "says": "the other benchmark"})
        self.assertIn("at commit `0123456789ab`.", self.renders())

    def test_an_exception_with_no_reason(self):
        self.other("fedcba987654" + "0" * 28, {"says": "the other benchmark"})
        self.refused("kind 'other': an exception says what it is and why")

    def test_a_file_with_no_commit(self):
        self.write(RESULT, json.dumps({k: v for k, v in RESULTS.items() if k != "header"}))
        self.refused("names no commit at ['header', 'commit']")

    def test_a_record_that_names_no_commit_is_said_to_be_what_it_is(self):
        self.other(None, {"why": "a record of a prover run", "says": "the prover's record, a run "
                          "of its own", "no_commit": True})
        self.assertIn("at commit `0123456789ab`, except the prover's record, a run of its own.",
                      self.renders())

    def differing(self):
        return {"why": "the harness changed after the freeze", "says": "the other benchmark",
                "differs_only_in": {"paths": ["fuzz/"], "says": "the fuzz harness"}}

    def test_a_claim_of_differing_only_in_some_paths_waits_with_no_history(self):
        self.other("fedcba987654" + "0" * 28, self.differing())
        done = self.run_script("--check")
        self.assertEqual(done.returncode, RENDER.EXIT_UNJUDGED, done.stdout + done.stderr)
        self.assertIn("history (git)", done.stdout)

    def git(self, *arguments):
        return subprocess.run(["git", "-C", str(self.root), "-c", "user.name=t",
                               "-c", "user.email=t@t", "-c", "commit.gpgsign=false",
                               *arguments], capture_output=True, text=True, check=True).stdout

    def history(self, second_path):
        self.git("init", "-q")
        self.git("add", "-A")
        self.git("commit", "-qm", "first")
        first = self.git("rev-parse", "HEAD").strip()
        self.write(second_path, "changed\n")
        self.git("add", "-A")
        self.git("commit", "-qm", "second")
        second = self.git("rev-parse", "HEAD").strip()
        self.write(RESULT, json.dumps(dict(RESULTS, header={"commit": first})))
        self.write("results/bench/macOS-other-fedcba987654.json",
                   json.dumps({"header": {"commit": second}, "runs": 7}))
        return first, second

    def test_a_claim_of_differing_only_in_some_paths_is_checked_with_git(self):
        self.other("fedcba987654" + "0" * 28, self.differing())
        first, second = self.history("fuzz/harness.cpp")
        self.assertIn(f"at commit `{first[:12]}`, except the other benchmark, at "
                      f"`{second[:12]}`, which differs from it only in the fuzz harness.",
                      self.renders())

    def test_a_claim_of_differing_only_in_some_paths_that_git_contradicts(self):
        self.other("fedcba987654" + "0" * 28, self.differing())
        self.history("src/store.cpp")
        self.refused("differ in ['src/store.cpp'], outside ['fuzz/']")


class Pins(Tree):
    """Review WD-15: a file under a pin, and a kind with a note, are said so where shown."""

    def pin(self, declared=True):
        digest = hashlib.sha256((self.root / RESULT).read_bytes()).hexdigest()
        self.write("scripts/data_pins.json", json.dumps(
            {"what": "pins", "old_files": {Path(RESULT).name: digest}}))
        if declared:
            self.figures["kinds"]["demo"]["pin"] = {
                "name": "old_files", "says": "a file kept under its pin, measured under an "
                "older protocol"}
            self.figure("pin:demo", {"what": "the demo's pin", "pin_of": "demo"})

    def test_a_pinned_file_read_by_a_kind_that_does_not_declare_it(self):
        self.pin(declared=False)
        self.refused("which is held under the pin 'old_files' of scripts/data_pins.json")

    def test_a_declared_pin_said_in_each_paragraph(self):
        self.pin()
        self.edit("docs/writeup/WRITEUP.template.md", "## Method",
                  "Each figure here is from ({{pin:demo}}).\n\n## Method")
        self.refused("a block shows figures of the kind 'demo', and does not hold {{pin:demo}}")
        self.edit("docs/writeup/WRITEUP.template.md", "The demo decodes",
                  "The demo, {{pin:demo}}, decodes")
        self.edit("docs/writeup/WRITEUP.template.md",
                  "Each figure here is from ({{pin:demo}}).\n\n", "")
        self.assertIn("The demo, a file kept under its pin, measured under an older protocol, "
                      "decodes", self.renders())

    def test_a_declared_pin_whose_file_is_not_held_by_it(self):
        self.pin()
        self.write(RESULT, json.dumps(dict(RESULTS, runs=31)))
        self.edit("docs/writeup/WRITEUP.template.md", "The demo decodes",
                  "The demo, {{pin:demo}}, decodes")
        self.refused("declares the pin 'old_files', and macOS-demo-0123456789ab.json is not held")

    def test_a_kind_s_note_said_in_each_paragraph(self):
        self.figures["kinds"]["demo"]["note"] = "summaries assembled by a script"
        self.figure("note:demo", {"what": "the demo's note", "note_of": "demo"})
        self.insert("These are {{note:demo}}.")
        self.refused("does not hold {{note:demo}}")


class Hosts(Tree):
    """Review WD-5: a figure is described as its own host's."""

    def test_a_figure_described_as_the_other_host_s(self):
        self.figures["figures"]["count"]["what"] = "the demo's runs on the Zen 1"
        self.save()
        self.refused("{{count}} is read on macOS, and its description",
                     "does not call it the M4's")

    def test_a_figure_with_no_host_word(self):
        self.figures["figures"]["count"]["what"] = "the demo's runs"
        self.save()
        self.refused("does not call it the M4's")

    def across(self, declared):
        self.figures["kinds"]["linux"] = {"what": "the Zen 1's demo",
                                          "files": {"Linux": "results/bench/Linux-demo-*.json"}}
        entry = {"what": "the M4's runs over the Zen 1's", "kind": "demo", "host": "macOS",
                 "value": {"div": [{"path": ["runs"]}, {"figure": "linux_runs"}]},
                 "format": "{:.2f}"}
        if declared:
            entry["across_hosts"] = "a ratio of the two hosts' runs"
        self.figure("ratio", entry)
        self.figure("linux_runs", {"what": "the Zen 1's runs", "kind": "linux", "host": "Linux",
                                   "value": {"path": ["runs"]}, "helper": True})
        self.write("results/bench/Linux-demo-0123456789ab.json",
                   json.dumps(dict(RESULTS, runs=10)))
        self.insert("The ratio is {{ratio}}.")

    def test_a_figure_across_hosts_not_declared(self):
        self.across(False)
        self.refused("{{ratio}} is computed from files of ['Linux', 'macOS']")

    def test_a_figure_across_hosts_declared(self):
        self.across(True)
        self.assertIn("The ratio is 3.00.", self.renders())


class Literals(Tree):
    """Review WD-4: the list itself types no number."""

    def test_a_literal_value(self):
        self.figure("count", {"what": "a figure on the M4", "value": 3.2, "format": "{:.1f}"})
        self.refused("{{count}}: the literal 3.2 is no declared factor")

    def test_a_value_of_literals_only(self):
        self.figures["factors"].append({"value": 2, "why": "a pair"})
        self.figure("count", {"what": "a figure on the M4", "value": {"mul": [2, 2]},
                              "format": "{:d}"})
        self.refused("{{count}}: its value reads no results file")

    def test_a_literal_text_with_a_number(self):
        self.figure("count", {"what": "a figure on the M4", "kind": "demo", "host": "macOS",
                              "value": {"concat": [{"path": ["header", "commit"]}, " 3.2x"]},
                              "format": "text"})
        self.refused("the text ' 3.2x' types a number")

    def test_words_with_a_number(self):
        self.figure("count", {"what": "whether it ran on the M4", "kind": "demo",
                              "host": "macOS", "value": {"above": [{"path": ["runs"]},
                                                                    {"path": ["median_us"]}]},
                              "words": {"false": "ten"}})
        self.refused("{{count}}: its words 'ten' type a number")

    def test_a_scale_that_is_no_factor(self):
        self.figures["figures"]["count"].update(format="{:.2f}", scale=1.1)
        self.save()
        self.refused("{{count}}: its scale 1.1 is no declared factor")

    def test_a_factor_with_no_reason(self):
        self.figures["factors"].append({"value": 3})
        self.save()
        self.refused("factors: {'value': 3} is not a number with its reason")

    def test_a_comparison_with_something_other_than_a_verdict_word(self):
        self.figure("count", {"what": "whether it is fast on the M4", "kind": "demo",
                              "host": "macOS", "value": {"is": [{"path": ["runs"]}, "fast"]},
                              "words": {"true": "so", "false": "not so"}})
        self.refused("is compares with 'fast', which is no verdict word")


class Gaps(Tree):
    """Review WD-7: no bar, figure or gap is printed so it says what the results do not."""

    def test_a_bar_shown_with_fewer_digits_than_it_has(self):
        self.targets["rows"][0]["target"] = {"at_most": 2.55}
        self.save()
        self.refused("{{bar:t1}} shows T-1's bar 2.55 as 2.5, which is not its value")

    def test_a_figure_that_rounds_to_the_other_side_of_its_bar(self):
        self.targets["rows"][0]["target"] = {"at_most": 1.23}
        self.figures["figures"]["bar:t1"]["format"] = "{:.2f}"
        self.write(RESULT, json.dumps(dict(RESULTS, median_us=1220.0)))
        self.save()
        self.renders()
        self.write(RESULT, json.dumps(dict(RESULTS, median_us=1234.5)))
        self.figures["figures"]["speed"]["format"] = "{:.1f}"
        self.save()
        self.refused("{{speed}} prints 1.2 ms against the bar 1.23, which reads met where the part "
                     "is missed")

    def test_a_gap_printed_as_none(self):
        self.figure("gap", {"what": "the gap on the M4", "kind": "demo", "host": "macOS",
                            "value": {"div": [{"path": ["median_us"]}, {"path": ["base"]}]},
                            "format": "{:.2f}", "gap": "ratio"})
        self.write(RESULT, json.dumps(dict(RESULTS, base=1234.0)))
        self.insert("The ratio is {{gap}}.")
        self.refused("{{gap}} prints the gap 1.00, which reads as none")
        self.figures["figures"]["gap"]["format"] = "{:.4f}"
        self.save()
        self.assertIn("The ratio is 1.0004.", self.renders())


class Claims(Tree):
    """Review WD-2: every sentence that ranks, compares or presumes a verdict holds a claim."""

    def test_a_judging_word_with_no_claim(self):
        for sentence in ("It decodes faster than its bar.", "The target is met.",
                         "It is the smallest store.", "It ran fewer times."):
            with self.subTest(sentence=sentence):
                self.setUp()
                self.insert(sentence)
                self.refused("no claim of the figure list or token ties those words to the results")

    def claim(self, text, holds, sentence):
        self.figures.setdefault("claims", []).append({"text": text, "holds": holds})
        self.save()
        self.insert(sentence)

    def test_a_claim_that_holds(self):
        self.claim("fewer runs than its median", {"below": [{"figure": "count"},
                                                            {"figure": "median"}]},
                   "It ran {{count}} times, fewer runs than its median of {{median}}.")
        self.figure("median", {"what": "the median on the M4", "kind": "demo", "host": "macOS",
                               "value": {"path": ["median_us"]}, "format": "{:,.1f}"})
        self.assertIn("It ran 30 times, fewer runs than its median of 1,234.5.", self.renders())

    def test_a_claim_the_results_do_not_bear_out(self):
        self.claim("more runs than its median", {"above": [{"figure": "count"},
                                                           {"figure": "median"}]},
                   "It ran {{count}} times, more runs than its median of {{median}}.")
        self.figure("median", {"what": "the median on the M4", "kind": "demo", "host": "macOS",
                               "value": {"path": ["median_us"]}, "format": "{:,.1f}"})
        self.refused("the template says 'more runs than its median', and the results do not "
                     "bear it out")

    def test_a_claim_on_figures_its_sentence_does_not_print(self):
        self.claim("fewer runs than its median", {"below": [{"figure": "count"},
                                                            {"figure": "median"}]},
                   "It ran fewer runs than its median of {{median}}.")
        self.figure("median", {"what": "the median on the M4", "kind": "demo", "host": "macOS",
                               "value": {"path": ["median_us"]}, "format": "{:,.1f}"})
        self.refused("its condition reads ['count'], which its sentence does not print")

    def test_a_claim_that_reads_none_of_its_sentence_s_figures(self):
        self.figures["claims"] = [{"text": "fewer runs than its median", "kind": "demo",
                                   "host": "macOS", "holds": {"below": [
                                       {"path": ["runs"]}, {"path": ["median_us"]}]}}]
        self.save()
        self.insert("It ran {{count}} times, fewer runs than its median.")
        self.refused("its sentence prints ['count'], and its condition reads none of them")

    def test_a_claim_of_literals(self):
        self.figures["factors"].append({"value": 2, "why": "a pair"})
        self.claim("ran fewer times", {"below": [2, 3]}, "It ran fewer times.")
        self.refused("the literal 3 is no declared factor")

    def test_a_claim_on_a_verdict(self):
        self.claim("the target is met", {"verdict_is": {"id": "T-1", "host": "macOS",
                                                        "class": "met"}},
                   "So the target is met.")
        self.renders()
        self.write(RESULT, json.dumps(dict(RESULTS, median_us=3000.0)))
        self.refused("the template says 'the target is met', and the results do not bear it out")

    def test_a_claim_not_in_the_template(self):
        self.figures["claims"] = [{"text": "nowhere at all", "holds": {"figure": "count"}}]
        self.save()
        self.refused("claims: 'nowhere at all' is not in the template exactly once")


class Census(Tree):
    """Review WD-2, WD-3: the missed table holds exactly the missed targets."""

    def setUp(self):
        super().setUp()
        self.targets["rows"].append({
            "id": "T-2", "host": "macOS", "target": {"at_least": 40}, "digits": 0, "unit": "",
            "judged": "the runs", "parts": [{"label": "runs",
                                             "file": "results/bench/macOS-demo-*.json",
                                             "value": {"path": ["runs"]}}]})
        self.figures["census"] = [{"block": "## Missed targets", "classes": ["missed",
                                                                             "missed in part"]}]
        self.figures["figures"]["verdict:t2"] = {"what": "T-2's verdict on the M4",
                                                 "verdict": {"id": "T-2", "host": "macOS"}}
        self.save()

    def missed(self, *rows):
        table = "\n".join(f"| a row | {{{{verdict:{row}}}}} |" for row in rows)
        self.edit("docs/writeup/WRITEUP.template.md", "## Method",
                  f"## Method\n\nThe rows.\n\n## Missed targets\n\n| Row | Verdict |\n|---|---|\n"
                  f"{table}\n\n## End\n")

    def test_a_missed_target_left_out(self):
        self.missed()
        self.insert("The runs target is {{verdict:t2}}.")
        self.refused("census '## Missed targets' leaves out [('T-2', 'macOS')]")

    def test_a_met_target_kept_in(self):
        self.missed("t1", "t2")
        self.refused("census '## Missed targets' holds [('T-1', 'macOS')], whose verdict is not")

    def test_the_missed_targets_exactly(self):
        self.missed("t2")
        self.renders()


class Changed(Tree):
    """A verdict whose outcome differs on the replaced run shows both, wherever it is quoted."""

    def setUp(self):
        super().setUp()
        self.figures["kinds"]["replaced"] = {
            "what": "the replaced demo", "files": {"macOS": "results/bench/replaced/macOS-demo-*.json"},
            "exception": {"why": "the run this one replaced", "says": "the replaced demo"}}
        self.figures["replaced"] = {"of": "demo", "kind": "replaced"}
        self.write("results/bench/replaced/macOS-demo-fedcba987654.json", json.dumps(
            dict(RESULTS, median_us=3000.0, header={"commit": "fedcba987654" + "0" * 28})))
        self.save()

    def test_a_changed_outcome_with_no_token(self):
        self.refused("T-1 on macOS, part 'demo', has another outcome on the replaced run")

    def test_a_changed_outcome_shown_beside_each_quote(self):
        self.figures["figures"]["changed:t1"] = {"what": "T-1's outcome on both runs",
                                                 "changed": {"id": "T-1", "host": "macOS",
                                                             "part": "demo"}}
        self.save()
        self.insert("Both runs: {{changed:t1}}.")
        self.refused("{{verdict:t1}} quotes T-1 on macOS, whose part 'demo' changed",
                     "does not hold {{changed:t1}}")
        self.edit("docs/writeup/WRITEUP.template.md", "Both runs: {{changed:t1}}.\n\n", "")
        self.edit("docs/writeup/WRITEUP.template.md", "is {{verdict:t1}}.",
                  "is {{verdict:t1}} ({{changed:t1}}).")
        page = self.renders()
        self.assertIn("is met (met on this run's 1.23 ms, missed on the replaced run's 3.00 ms, "
                      "the new outcome in tick-store's favour).", page)
        self.assertIn("except the replaced demo, at `fedcba987654`.", page)

    def test_a_claim_that_rests_on_a_changed_outcome(self):
        self.figures["figures"]["changed:t1"] = {"what": "T-1's outcome on both runs",
                                                 "changed": {"id": "T-1", "host": "macOS",
                                                             "part": "demo"}}
        self.figures["claims"] = [{"text": "so it leads", "holds": {"verdict_is": {
            "id": "T-1", "host": "macOS", "class": "met"}}}]
        self.save()
        self.edit("docs/writeup/WRITEUP.template.md", "is {{verdict:t1}}.",
                  "is {{verdict:t1}} ({{changed:t1}}).")
        self.insert("And so it leads.")
        self.refused("rests on T-1 on macOS, whose outcome changed with the replaced run")


class Prover(Tree):
    """Review WD-8: faults counted without the control, the record in its own terms."""

    PROVER = ('FAULTS = (\n    Fault("a-fault", "x"),\n    Fault("b-fault", "x"),\n'
              '    Fault("c-fault", "x"),\n    Fault("control", "x", control=True),\n)\n')

    def setUp(self):
        super().setUp()
        self.write("scripts/prove_checks.py", self.PROVER)
        self.record([("a-fault", "PROVEN"), ("b-fault", "UNDETECTED"), ("control", "UNDETECTED")])
        self.figures["kinds"]["proofs"] = {
            "what": "the prover's record", "files": {"any": "results/check-proofs.json"},
            "exception": {"why": "a run of its own", "says": "the prover's record",
                          "no_commit": True}}
        for count in ("listed", "covered", "proven", "controls_passed"):
            self.figures["figures"][f"proofs_{count}"] = {
                "what": f"the prover's {count}", "kind": "proofs", "host": "any",
                "value": {"prover": {"source": "scripts/prove_checks.py", "count": count}},
                "format": "{:d}"}
        self.figures["figures"]["proofs_controls_passed"].update(
            format=None, words={"true": "its control passed", "false": "its control failed"})
        del self.figures["figures"]["proofs_controls_passed"]["format"]
        self.save()
        self.insert("Of {{proofs_listed}} faults the record covers {{proofs_covered}}, "
                    "{{proofs_proven}} proven, and {{proofs_controls_passed}}.")

    def record(self, entries):
        self.write("results/check-proofs.json", json.dumps(
            {"faults": [{"fault_id": name, "verdict": verdict} for name, verdict in entries]}))

    def test_the_counts(self):
        self.assertIn("Of 3 faults the record covers 2, 1 proven, and its control passed.",
                      self.renders())

    def test_a_control_caught(self):
        self.record([("a-fault", "PROVEN"), ("control", "PROVEN")])
        self.assertIn("Of 3 faults the record covers 1, 1 proven, and its control failed.",
                      self.renders())

    def test_a_record_of_faults_no_longer_listed(self):
        # The record covers what it covers of the list as it stands.
        self.record([("gone", "PROVEN"), ("a-fault", "PROVEN"), ("control", "UNDETECTED")])
        self.assertIn("Of 3 faults the record covers 1, 1 proven, and its control passed.",
                      self.renders())

    def test_a_record_with_no_control(self):
        self.record([("a-fault", "PROVEN")])
        self.refused("the record holds no control")

    def test_the_committed_prover(self):
        faults, controls = RENDER.listed_faults(
            (REPO_ROOT / "scripts" / "prove_checks.py").read_text(encoding="utf-8"), "t")
        tree = ast.parse((REPO_ROOT / "scripts" / "prove_checks.py").read_text(encoding="utf-8"))
        names = [node.args[0].value for node in ast.walk(tree) if isinstance(node, ast.Call)
                 and getattr(node.func, "id", "") == "Fault" and node.args
                 and isinstance(node.args[0], ast.Constant)]
        self.assertEqual(sorted(faults + controls), sorted(names))
        self.assertEqual(controls, ["control-comment-only"])


class NotJudged(Tree):
    def pend(self):
        self.figures["kinds"]["later"] = {"what": "a file still to be measured",
                                          "pending": "it is measured at the freeze"}
        self.figures["figures"]["later_figure"] = {"what": "a figure still to come",
                                                   "kind": "later", "host": "any",
                                                   "format": "{:d}", "value": {"path": ["x"]}}
        self.save()
        self.insert("It took {{later_figure}} s.")

    def test_a_token_whose_kind_is_still_to_come_is_not_judged(self):
        self.pend()
        done = self.run_script("--check")
        self.assertEqual(done.returncode, RENDER.EXIT_UNJUDGED, done.stderr)
        self.assertIn("not judged: 1 of 9 tokens have no results file yet", done.stdout)
        self.assertIn("waiting, by kind of file: later (any) 1", done.stdout)
        self.assertFalse((self.root / "WRITEUP.md").exists())

    def test_a_missing_file_is_not_judged_until_the_page_exists(self):
        (self.root / RESULT).unlink()
        done = self.run_script("--check")
        self.assertEqual(done.returncode, RENDER.EXIT_UNJUDGED, done.stderr)
        self.assertIn("not judged: 5 of 8 tokens have no results file yet", done.stdout)
        self.assertIn("demo (macOS) 4", done.stdout)

    def test_a_missing_file_fails_once_the_page_exists(self):
        self.renders()
        (self.root / RESULT).unlink()
        done = self.run_script("--check")
        self.assertEqual(done.returncode, RENDER.EXIT_STALE)
        self.assertIn("WRITEUP.md exists, and 5 of its tokens have no results file", done.stderr)

    def test_not_judged_still_holds_every_rule_on_what_resolves(self):
        self.pend()
        self.insert("It ran 12 times.")
        self.refused("the template types the number '12' by hand")

    def test_list_exits_not_judged_with_tokens_waiting(self):
        self.pend()
        done = self.run_script("--list")
        self.assertEqual(done.returncode, RENDER.EXIT_UNJUDGED, done.stderr)
        self.assertIn("later_figure\twaits on later (any): it is measured at the freeze",
                      done.stdout)

    def test_a_draft_holds_what_resolves_and_the_waiting_tokens(self):
        self.pend()
        draft = self.root / "draft.md"
        self.assertEqual(self.run_script("--draft", str(draft)).returncode, RENDER.EXIT_UNJUDGED)
        text = draft.read_text(encoding="utf-8")
        self.assertIn("decodes at 1.23 ms", text)
        self.assertIn("It took {{later_figure}} s.", text)

    def test_check_sh_reports_not_judged_as_a_skip_and_a_failure_under_no_skips(self):
        tree = self.exported_copy()
        for name in ("docs/writeup", "results", "scripts/targets.json", "include",
                     "scripts/data_pins.json"):
            target = tree / name
            if target.is_dir():
                shutil.rmtree(target)
            elif target.exists():
                target.unlink()
        self.pend()
        for name in ("docs/writeup", "results", "include"):
            shutil.copytree(self.root / name, tree / name)
        shutil.copy2(self.root / "scripts/targets.json", tree / "scripts/targets.json")
        (tree / "WRITEUP.md").unlink(missing_ok=True)
        logs = self.root / "logs"
        env = dict(os.environ, CXX=os.environ.get("CXX", "c++"))
        script = str(tree / "scripts" / "check.sh")
        done = subprocess.run([script, "--log-dir", str(logs), "--only", "writeup-check"],
                              capture_output=True, text=True, env=env)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual(summary(logs), {"writeup-check": "SKIP"})
        self.assertIn("not judged: 1 of 9 tokens have no results file yet", done.stdout)
        done = subprocess.run([script, "--no-skips", "--log-dir", str(logs), "--only",
                               "writeup-check"], capture_output=True, text=True, env=env)
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        self.assertEqual(summary(logs), {"writeup-check": "FAIL"})

    def exported_copy(self):
        """This tree's tracked files, as test_check_sh.py copies them, under build/."""
        if check_hygiene.is_work_tree_top(REPO_ROOT):
            names = check_hygiene.tracked_files(REPO_ROOT)
        else:
            names = check_hygiene.exported_files(REPO_ROOT)
        tree = self.root / "tree"
        for name in names:
            source = REPO_ROOT / name
            if source.is_file():
                (tree / name).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, tree / name)
        (tree / "COMMIT").write_text("0" * 40 + "\n")
        return tree


def summary(logs):
    lines = (logs / "summary.tsv").read_text().splitlines()
    return {line.split("\t")[0]: line.split("\t")[1] for line in lines}


# A document of known answers for each form of the figure list (review WD-11). Each expected value
# is worked out in plain Python beside its expression, so the two compute it apart.
DOCUMENT = {
    "header": {"commit": COMMIT},
    "a": 7.5, "b": 2.0, "power": 4096, "exponent": 12, "word": "abcdefgh",
    "list": [3.0, 9.0, 1.0, 4.0], "flags": [True, False],
    "items": [{"name": "x", "v": 1.5, "kind": "p"}, {"name": "y", "v": 2.5, "kind": "q"},
              {"name": "z", "v": 6.0, "kind": "p"}],
    "columns": [
        {"name": "price", "codecs": [
            {"family": "pfor", "codec": "pfor_a", "exact": True, "bits": 9.0},
            {"family": "pfor", "codec": "pfor_b", "exact": True, "bits": 7.0},
            {"family": "pfor", "codec": "pfor_c", "exact": False, "bits": 1.0},
            {"family": "gorilla", "codec": "gorilla", "exact": True, "bits": 30.0}],
         "rows": 10},
        {"name": "size", "codecs": [
            {"family": "gorilla", "codec": "gorilla", "exact": True, "bits": 20.0}],
         "rows": 20}],
}
FORMS = [
    ("sub", {"sub": [{"path": ["a"]}, {"path": ["b"]}]}, 7.5 - 2.0),
    ("add", {"add": [{"path": ["a"]}, {"path": ["b"]}]}, 7.5 + 2.0),
    ("mul", {"mul": [{"path": ["a"]}, {"path": ["b"]}]}, 7.5 * 2.0),
    ("div", {"div": [{"path": ["a"]}, {"path": ["b"]}]}, 7.5 / 2.0),
    ("min", {"min": [{"path": ["a"]}, {"path": ["b"]}]}, 2.0),
    ("max", {"max": [{"path": ["a"]}, {"path": ["b"]}]}, 7.5),
    ("median", {"median": {"path": ["list"]}}, statistics.median([3.0, 9.0, 1.0, 4.0])),
    ("min only_above", {"min": {"path": ["list"]}, "only_above": {"path": ["b"]}}, 3.0),
    ("max only_below", {"max": {"path": ["list"]}, "only_below": {"path": ["a"]}}, 4.0),
    ("each", {"each": ["x", "z"], "of": {"path": ["items", {"name": "$"}, "v"]}}, [1.5, 6.0]),
    ("all", {"all": [{"above": [{"path": ["a"]}, {"path": ["b"]}]},
                     {"below": [{"path": ["b"]}, {"path": ["a"]}]}]}, True),
    ("all false", {"all": {"path": ["flags"]}}, False),
    ("any", {"any": {"path": ["flags"]}}, True),
    ("not", {"not": {"above": [{"path": ["a"]}, {"path": ["b"]}]}}, False),
    ("above", {"above": [{"path": ["b"]}, {"path": ["a"]}]}, False),
    ("below", {"below": [{"path": ["b"]}, {"path": ["a"]}]}, True),
    ("same", {"same": [{"path": ["b"]}, {"div": [{"mul": [{"path": ["b"]}, {"path": ["b"]}]},
                                                 {"path": ["b"]}]}]}, 2.0),
    ("log2", {"log2": {"path": ["power"]}}, 12),
    ("pow2", {"pow2": {"path": ["exponent"]}}, 4096),
    ("concat", {"concat": [{"path": ["word"]}, "-", {"path": ["word"]}]}, "abcdefgh-abcdefgh"),
    ("prefix", {"prefix": {"path": ["word"]}, "length": 3}, "abc"),
    ("is", {"is": [{"path": ["word"]}, "met"]}, False),
    ("name pick", {"path": ["items", {"name": "y"}, "v"]}, 2.5),
    ("items", {"items": ["items"], "field": ["v"]}, [1.5, 2.5, 6.0]),
    ("where", {"where": ["items"], "match": {"kind": "p"}, "field": ["v"]}, [1.5, 6.0]),
    ("count", {"count": ["items"]}, 3),
    ("count where", {"count": ["items"], "where": {"field": ["kind"], "equals": "q"}}, 1),
    ("codec_column", {"codec_column": "size", "field": ["rows"]}, 20),
    ("codec_columns", {"codec_columns": {"with_family": "gorilla", "except": ["size"]}},
     ["price"]),
    ("least", {"least": {"column": "price", "family": "pfor", "field": "bits"}}, 7.0),
    ("least not_family", {"least": {"column": "price", "not_family": "pfor", "field": "bits"}},
     30.0),
    ("codec", {"codec": "pfor_a", "column": "price", "field": ["bits"]}, 9.0),
]


class Forms(unittest.TestCase):
    """Each form of the figure list on DOCUMENT, against an answer computed here."""

    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.root)
        self.figures = {"schema": RENDER.SCHEMA, "kinds": {"doc": {
            "what": "a document", "files": {"any": "results/doc.json"}}}, "figures": {}}
        (self.root / "results").mkdir()
        (self.root / "results" / "doc.json").write_text(json.dumps(DOCUMENT))
        (self.root / "src").mkdir()
        (self.root / "src" / "a.hpp").write_text("x\ny\n")
        (self.root / "src" / "b.hpp").write_text("z\n")
        (self.root / "src" / "c.txt").write_text(
            "BEGIN\nFault(1)\nFault(2)\nEND\nFault(3)\n")

    def value(self, expression):
        resolver = RENDER.Resolver(self.root, self.figures)
        entry = {"what": "a form", "kind": "doc", "host": "any", "value": expression}
        context = resolver.context("form", entry)
        return RENDER.evaluate(expression, context)

    def test_each_form(self):
        for name, expression, expected in FORMS:
            with self.subTest(form=name):
                self.assertEqual(self.value(expression), expected)

    def test_tree_forms(self):
        self.assertEqual(self.value({"lines": ["src/*.hpp", "src/a.hpp"]}), 3)
        self.assertEqual(self.value({"count_matches": r"^Fault\(", "source": "src/c.txt"}), 3)
        self.assertEqual(self.value({"count_matches": r"^Fault\(", "source": "src/c.txt",
                                     "between": ["^BEGIN$", "^END$"]}), 2)

    def test_figure_reference(self):
        self.figures["figures"]["base"] = {"what": "a", "kind": "doc", "host": "any",
                                           "value": {"path": ["a"]}}
        self.assertEqual(self.value({"sub": [{"figure": "base"}, {"path": ["b"]}]}), 5.5)

    def test_each_refusal_of_a_form(self):
        for expression, needle in (
                ({"same": [{"path": ["a"]}, {"path": ["b"]}]}, "and they differ"),
                ({"log2": {"path": ["a"]}}, "is not a power of two"),
                ({"pow2": {"path": ["a"]}}, "is no whole power"),
                ({"min": {"path": ["list"]}, "only_above": {"path": ["power"]}},
                 "no value to take the min of"),
                ({"each": [], "of": {"path": ["a"]}}, "which gives no items"),
                ({"all": {"path": ["list"]}}, "which are not true or false"),
                ({"not": {"path": ["a"]}}, "not of 7.5"),
                ({"concat": [{"path": ["a"]}]}, "is not text to join"),
                ({"prefix": {"path": ["a"]}, "length": 2}, "is not text"),
                ({"path": ["items", {"name": "nowhere"}, "v"]}, "0 items match"),
                ({"items": ["a"], "field": ["v"]}, "is no list of items"),
                ({"count": ["a"]}, "is no list to count"),
                ({"where": ["items"], "match": {"kind": "r"}, "field": ["v"]},
                 "no item of ['items'] matches"),
                ({"codec_columns": {"with_family": "gorilla", "except": ["volume"]}},
                 "are no gorilla columns"),
                ({"least": {"column": "size", "family": "pfor", "field": "bits"}},
                 "no exact codec of that family"),
                ({"codec": "nowhere", "column": "price", "field": ["bits"]}, "no one codec"),
                ({"lines": ["src/*.cpp"]}, "matches no file"),
                ({"count_matches": "x", "source": "src/c.txt", "between": ["^NO$", "^END$"]},
                 "to count between"),
                ({"mystery": 1}, "unknown expression"),
                ({"sub": [{"path": ["word"]}, {"path": ["a"]}]}, "is not a number")):
            with self.subTest(expression=expression):
                with self.assertRaises((RENDER.Refused, RESULTS_CODE.TargetError)) as caught:
                    self.value(expression)
                self.assertIn(needle, str(caught.exception))

    def test_formats(self):
        for value, entry, expected in (
                (1234.5, {"format": "{:,.2f}"}, "1,234.50"),
                (1234.0, {"format": "{:,}"}, "1,234"),
                (1234, {"format": "{:d}"}, "1234"),
                (1234.5, {"format": "{:.1f}", "scale": 0.001}, "1.2"),
                ("parquet", {"format": "system"}, RESULTS_CODE.SYSTEM_LABEL["parquet"]),
                (["a", "b"], {"format": "list"}, "a, b"),
                ([], {"format": "list", "empty": "none"}, "none"),
                ("words", {"format": "text"}, "words"),
                (True, {"words": {"true": "yes"}}, "yes")):
            with self.subTest(entry=entry):
                self.assertEqual(RENDER.formatted(value, entry, "t"), expected)
        for value, entry, needle in (
                ("nowhere", {"format": "system"}, "is no system"),
                ([], {"format": "list"}, "an empty list, and no words for one"),
                ("two\nlines", {"format": "text"}, "is not one line of text"),
                (1, {"words": {"true": "yes"}}, "words for 1"),
                (False, {"words": {"true": "yes"}}, "is false on these results")):
            with self.subTest(entry=entry, value=value):
                with self.assertRaises(RENDER.Refused) as caught:
                    RENDER.formatted(value, entry, "t")
                self.assertIn(needle, str(caught.exception))

    def test_a_target_s_figure_is_the_targets_block_s(self):
        # The essay's figure for a part and render_results.py's are one computation.
        definitions = {"rows": [{"id": "T-9", "host": "any", "target": {"at_most": 8.0},
                                 "digits": 2, "unit": " bits", "parts": [
                                     {"label": "p", "file": "results/doc.json",
                                      "value": {"div": [{"path": ["a"]}, {"path": ["b"]}]}}]}]}
        (self.root / "scripts").mkdir()
        (self.root / "scripts" / "targets.json").write_text(json.dumps(definitions))
        resolver = RENDER.Resolver(self.root, self.figures)
        value = resolver.target_figure("t", {"target": {"id": "T-9", "host": "any",
                                                        "part": "p"}})
        self.assertEqual(value, 7.5 / 2.0)
        parts, _ = RESULTS_CODE.judged_parts(definitions["rows"][0], self.root)
        self.assertEqual(parts[0][1], "3.75 bits")


class Verdicts(unittest.TestCase):
    """Review WD-12: render_results.py's verdict code on hand-built parts, apart from both
    renderers."""

    def parts(self, *verdicts):
        return [(f"p{index}", "x", met, None, None) for index, met in enumerate(verdicts)]

    def test_verdict_class(self):
        unjudged = RESULTS_CODE.Unjudged("this code")
        for verdicts, expected in (
                ((True,), "met"), ((True, True), "met"), ((False,), "missed"),
                ((False, False), "missed"), ((True, False), "missed in part"),
                ((False, True), "missed in part"), ((unjudged,), "not judged"),
                ((True, unjudged), "not judged in part"),
                ((False, unjudged), "missed in part"), ((True, False, unjudged), "missed in part")):
            with self.subTest(verdicts=verdicts):
                self.assertEqual(RESULTS_CODE.verdict_class(self.parts(*verdicts)), expected)
        self.assertEqual(RESULTS_CODE.verdict_class(None), "not judged")

    def test_verdict_of(self):
        unjudged = RESULTS_CODE.Unjudged("this code")
        for verdicts, expected in (
                ((True, True), "met"), ((False, False), "missed"),
                ((True, False), "met on (p0); missed on (p1)"),
                ((False, True), "met on (p1); missed on (p0)"),
                ((unjudged,), "not judged on this code"),
                ((True, unjudged), "met on (p0); not judged on this code for (p1)"),
                ((None, None), "not judged: no published figure, reported as measured")):
            with self.subTest(verdicts=verdicts):
                self.assertEqual(RESULTS_CODE.verdict_of(self.parts(*verdicts)), expected)
        with self.assertRaises(RESULTS_CODE.TargetError):
            RESULTS_CODE.verdict_of(self.parts(True, None))


class TheList(unittest.TestCase):
    """The committed figure list and template, as the render reads them."""

    def setUp(self):
        self.figures = json.loads((REPO_ROOT / RENDER.FIGURES_PATH).read_text(encoding="utf-8"))
        self.template = (REPO_ROOT / RENDER.TEMPLATE_PATH).read_text(encoding="utf-8")

    def test_the_list_and_the_template_hold_every_rule_that_needs_no_results(self):
        RENDER.check_list(self.figures)
        body, _ = RENDER.body_of(self.template)
        names = RENDER.tokens_of(body)
        self.assertIsNone(RENDER.opening_problem(body))
        resolver = RENDER.Resolver(REPO_ROOT, self.figures)
        RENDER.check_entries(resolver, names, resolver.factors)
        self.assertEqual(RENDER.typed_numbers(body, self.figures["not_results"]), [])
        self.assertEqual(sorted(set(names) - set(self.figures["figures"])), [])
        self.assertLessEqual(RENDER.word_count(body), RENDER.WORDS_AT_MOST)

    def test_every_kind_has_a_commit_or_is_a_declared_exception(self):
        for kind, spec in self.figures["kinds"].items():
            exception = spec.get("exception")
            if exception:
                self.assertTrue(exception.get("why") and exception.get("says"), kind)

    def test_every_targets_pattern_is_one_kind(self):
        definitions = json.loads((REPO_ROOT / RENDER.TARGETS_PATH).read_text(encoding="utf-8"))
        resolver = RENDER.Resolver(REPO_ROOT, self.figures)
        for row in definitions["rows"]:
            for spec in ([row["lowest_of"]] if "lowest_of" in row else row.get("parts", [])):
                resolver.kind_of_pattern(spec["file"], row["id"])


if __name__ == "__main__":
    unittest.main()
