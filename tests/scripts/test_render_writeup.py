#!/usr/bin/env python3
"""Tests for scripts/render_writeup.py. Run with:

    python3 -I tests/scripts/test_render_writeup.py

Each test builds a small tree of its own under the repo's gitignored build/ directory: a
template, a figure list, a targets file, a results file and a source file with a constant, and
runs the script on it as check.sh does. Each refusal is shown on a tree that differs from a good
one in one place, and so are the two rules the page lives by: a hand-edited figure fails --check,
and a tree whose results are not all committed is not judged, which check.sh reports as a skip and
fails under --no-skips.
"""

import copy
import importlib.util
import json
import os
import shutil
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

RESULT = "results/bench/macOS-demo-0123456789ab.json"
SOURCE = "include/demo.hpp"
CONSTANT = "inline constexpr std::size_t kHeaderSize = 16;\n"
# Words enough for the essay's range, with no number in them.
FILLER = " ".join(["the store keeps each stock's records together"] * 400) + "."

TEMPLATE = f"""# A demo essay

The demo decodes at {{{{speed}}}} ms, against a bar of {{{{bar:t1}}}} ms, so the target is
{{{{verdict:t1}}}}, over {{{{count}}}} runs on ITCH 5.0, with a {{{{code:header}}}}-byte header
([results]({{{{path:demo}}}})).
<!-- a note for whoever edits this, with a 99 in it -->

## Method

{FILLER}
"""

FIGURES = {
    "schema": RENDER.SCHEMA,
    "template": "docs/writeup/WRITEUP.template.md",
    "page": "WRITEUP.md",
    "targets": "scripts/targets.json",
    "not_results": [{"text": "ITCH 5.0", "why": "the feed's version"}],
    "kinds": {"demo": {"what": "the demo benchmark",
                       "files": {"macOS": "results/bench/macOS-demo-*.json"}}},
    "figures": {
        "speed": {"what": "the demo's median, ms", "kind": "demo", "host": "macOS",
                  "value": {"path": ["median_us"]}, "format": "{:.2f}", "scale": 0.001},
        "count": {"what": "the demo's runs", "kind": "demo", "host": "macOS",
                  "value": {"path": ["runs"]}, "format": "{:,}"},
        "path:demo": {"what": "the demo's file", "kind": "demo", "host": "macOS", "path": True},
        "code:header": {"what": "the header's bytes", "format": "{:d}", "value": {"constant": {
            "source": SOURCE,
            "pattern": r"^inline constexpr std::size_t kHeaderSize = (\d+);$"}}},
        "bar:t1": {"what": "T-1's target", "bar": {"id": "T-1", "host": "macOS"},
                   "format": "{:.1f}"},
        "verdict:t1": {"what": "T-1's verdict", "verdict": {"id": "T-1", "host": "macOS"},
                       "files": [{"kind": "demo", "host": "macOS"}], "words": "class"},
    },
}

TARGETS = {"rows": [{
    "id": "T-1", "host": "macOS", "target": {"at_most": 2.5}, "digits": 2, "unit": " ms",
    "judged": "the demo's median",
    "parts": [{"label": "demo", "file": "results/bench/macOS-demo-*.json",
               "value": {"div": [{"path": ["median_us"]}, 1000]}}]}]}

RESULTS = {"median_us": 1234.5, "runs": 30}


class Tree(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.root)
        self.figures = copy.deepcopy(FIGURES)
        self.write("docs/writeup/WRITEUP.template.md", TEMPLATE)
        self.write("scripts/targets.json", json.dumps(TARGETS))
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

    def edit(self, name, old, new):
        path = self.root / name
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def run_script(self, *arguments):
        return subprocess.run([sys.executable, "-I", str(SCRIPT), "--root", str(self.root),
                               *arguments], capture_output=True, text=True)

    def refused(self, *needles, arguments=("--check",)):
        done = self.run_script(*arguments)
        self.assertEqual(done.returncode, RENDER.EXIT_REFUSED, done.stdout + done.stderr)
        for needle in needles:
            self.assertIn(needle, done.stderr)
        return done

    def page(self):
        return (self.root / "WRITEUP.md").read_text(encoding="utf-8")


class Renders(Tree):
    def test_a_tree_whose_tokens_resolve_renders_a_page_check_calls_current(self):
        done = self.run_script()
        self.assertEqual(done.returncode, 0, done.stderr)
        page = self.page()
        self.assertIn("decodes at 1.23 ms, against a bar of ≤ 2.5 ms, so the target is\nmet, "
                      "over 30 runs on ITCH 5.0, with a 16-byte header", page)
        self.assertIn(f"([results]({RESULT}))", page)
        self.assertNotIn("a note for whoever edits this", page)
        self.assertTrue(page.endswith(RENDER.MARKER + "\n"))
        done = self.run_script("--check")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("WRITEUP.md is current: 6 tokens", done.stdout)

    def test_a_hand_edited_number_in_the_page_fails_check(self):
        self.assertEqual(self.run_script().returncode, 0)
        self.edit("WRITEUP.md", "decodes at 1.23 ms", "decodes at 1.22 ms")
        done = self.run_script("--check")
        self.assertEqual(done.returncode, RENDER.EXIT_STALE)
        self.assertIn("WRITEUP.md differs from a fresh render", done.stderr)

    def test_a_page_left_behind_by_a_new_result_fails_check(self):
        self.assertEqual(self.run_script().returncode, 0)
        self.write(RESULT, json.dumps(dict(RESULTS, median_us=2000.0)))
        self.assertEqual(self.run_script("--check").returncode, RENDER.EXIT_STALE)

    def test_check_with_every_token_resolved_and_no_page_fails(self):
        done = self.run_script("--check")
        self.assertEqual(done.returncode, RENDER.EXIT_STALE)
        self.assertIn("every token resolves, and there is no WRITEUP.md", done.stderr)

    def test_a_file_measured_again_with_a_new_commit_in_its_name_still_resolves(self):
        (self.root / RESULT).rename(self.root / "results/bench/macOS-demo-fedcba987654.json")
        done = self.run_script()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("(results/bench/macOS-demo-fedcba987654.json)", self.page())

    def test_the_verdict_is_render_results_own_code(self):
        # The token's word is what render_results.verdict_class says of the parts that
        # render_results.judged_parts judges, so the essay and the Targets block cannot differ.
        original = RENDER.render_results.verdict_class
        seen = []

        def verdict_class(parts):
            seen.append([part[:3] for part in parts])
            return "the targets block's word"

        RENDER.render_results.verdict_class = verdict_class
        try:
            rendered = RENDER.render(self.root, RENDER.DEFAULT_FIGURES)
        finally:
            RENDER.render_results.verdict_class = original
        self.assertEqual(rendered.values["verdict:t1"], "the targets block's word")
        self.assertEqual(seen, [[("demo", "1.23 ms", True)]])

    def test_a_missed_target_is_shown_missed(self):
        self.write(RESULT, json.dumps(dict(RESULTS, median_us=3000.0)))
        self.assertEqual(self.run_script().returncode, 0)
        self.assertIn("so the target is\nmissed,", self.page())

    def test_a_draft_is_never_the_page(self):
        done = self.run_script("--draft", str(self.root / "WRITEUP.md"))
        self.assertEqual(done.returncode, RENDER.EXIT_REFUSED)
        self.assertIn("--draft is never the page", done.stderr)


class Refuses(Tree):
    def test_an_unknown_token(self):
        self.edit("docs/writeup/WRITEUP.template.md", "## Method", "{{nowhere}}\n\n## Method")
        self.refused("the template's {{nowhere}} is not in the figure list")

    def test_a_stray_brace_pair(self):
        self.edit("docs/writeup/WRITEUP.template.md", "## Method", "{{not a token}}\n\n## Method")
        self.refused("which is no token")

    def test_an_unused_entry(self):
        self.figures["figures"]["spare"] = dict(self.figures["figures"]["count"])
        self.save()
        self.refused("the figure list's 'spare' is used nowhere in the template")

    def test_a_helper_no_entry_uses(self):
        self.figures["figures"]["spare"] = dict(self.figures["figures"]["count"], helper=True)
        self.save()
        self.refused("helper 'spare' is used by no entry")

    def test_a_kind_that_matches_two_files(self):
        self.write("results/bench/macOS-demo-fedcba987654.json", json.dumps(RESULTS))
        self.refused("results/bench/macOS-demo-*.json matches 2 files")

    def test_a_kind_named_by_a_commit(self):
        self.figures["kinds"]["demo"]["files"]["macOS"] = RESULT
        self.save()
        self.refused("names the commit '0123456789ab'", "name the kind with * in its place")

    def test_a_kind_with_no_file_on_the_host(self):
        self.figures["figures"]["count"]["host"] = "Linux"
        self.save()
        self.refused("kind 'demo' has no file on the host 'Linux'")

    def test_a_kind_that_says_nothing_of_its_files(self):
        del self.figures["kinds"]["demo"]["what"]
        self.save()
        self.refused("kind 'demo' says nothing of what its files are")

    def test_a_value_outside_its_format(self):
        self.write(RESULT, json.dumps(dict(RESULTS, runs=30.5)))
        self.refused("{{count}}: 30.5 is not a whole number, and {:,} writes one")

    def test_text_where_a_number_goes(self):
        self.write(RESULT, json.dumps(dict(RESULTS, median_us="fast")))
        self.refused("{{speed}}: 'fast' is not a number to write as {:.2f}")

    def test_a_field_the_file_does_not_have(self):
        self.write(RESULT, json.dumps({"runs": 30}))
        self.refused("{{speed}}: no 'median_us' on the path")

    def test_a_typed_number_in_the_template(self):
        self.edit("docs/writeup/WRITEUP.template.md", "## Method", "It ran 12 times.\n\n## Method")
        self.refused("the template types the number '12' by hand")

    def test_a_typed_number_in_a_table(self):
        self.edit("docs/writeup/WRITEUP.template.md", "## Method",
                  "| What | Bar |\n|---|---|\n| demo | ≤ 2.5 ms |\n\n## Method")
        self.refused("the template types the number '2.5' by hand")

    def test_digits_in_a_name_are_no_number(self):
        self.edit("docs/writeup/WRITEUP.template.md", "## Method",
                  "On the M4 with AVX2 off, Q1 ran.\n\n## Method")
        self.assertEqual(self.run_script().returncode, 0)

    def test_a_non_result_with_no_reason(self):
        self.figures["not_results"][0]["why"] = ""
        self.save()
        self.refused("'ITCH 5.0' is kept out of the number check with no reason")

    def test_a_non_result_the_template_does_not_hold(self):
        self.figures["not_results"].append({"text": "2019", "why": "a year"})
        self.save()
        self.refused("'2019' is not in the template")

    def test_a_changed_constant_fails_check(self):
        self.assertEqual(self.run_script().returncode, 0)
        self.edit(SOURCE, "kHeaderSize = 16;", "kHeaderSize = 20;")
        self.assertEqual(self.run_script("--check").returncode, RENDER.EXIT_STALE)

    def test_a_constant_that_changed_form(self):
        self.edit(SOURCE, CONSTANT, "inline constexpr std::size_t kHeaderSize{16};\n")
        self.refused("matches 0 lines of include/demo.hpp, not one: the constant moved or "
                     "changed form")

    def test_a_constant_that_moved(self):
        (self.root / SOURCE).unlink()
        self.write("include/other.hpp", CONSTANT)
        self.refused("{{code:header}}: cannot read include/demo.hpp")

    def test_a_constant_found_twice(self):
        self.write(SOURCE, CONSTANT + "// and\n" + CONSTANT)
        self.refused("matches 2 lines of include/demo.hpp")

    def test_a_verdict_whose_files_are_not_the_row_s(self):
        self.figures["kinds"]["other"] = {"what": "another file",
                                          "files": {"macOS": "results/other-*.json"}}
        self.figures["figures"]["verdict:t1"]["files"] = [{"kind": "other", "host": "macOS"}]
        self.save()
        self.refused("T-1 on macOS reads ['results/bench/macOS-demo-*.json'], and its entry "
                     "names ['results/other-*.json']")

    def test_a_verdict_part_the_row_does_not_have(self):
        self.figures["figures"]["verdict:t1"]["verdict"]["parts"] = ["elsewhere"]
        self.save()
        self.refused("{{verdict:t1}}: no part ['elsewhere'] in ['demo']")

    def test_a_condition_the_sentence_does_not_hold_on(self):
        self.figures["figures"]["count"] = {
            "what": "whether the demo ran more than ten times", "kind": "demo", "host": "macOS",
            "value": {"below": [{"path": ["runs"]}, 10]}, "words": {"true": "thirty"}}
        self.save()
        self.refused("{{count}} is false on these results, and the template's sentence holds "
                     "only if it is true")

    def test_an_essay_too_short(self):
        self.edit("docs/writeup/WRITEUP.template.md", FILLER, "Short.")
        self.refused("outside its 2,500 to 4,000")

    def test_an_essay_too_long(self):
        self.edit("docs/writeup/WRITEUP.template.md", FILLER, FILLER + "\n\n" + FILLER)
        self.refused("outside its 2,500 to 4,000")

    def test_an_essay_that_opens_with_a_heading(self):
        self.edit("docs/writeup/WRITEUP.template.md", "# A demo essay\n\n",
                  "# A demo essay\n\n## Summary\n\n")
        self.refused("a section heading comes before any prose")

    def test_an_essay_that_opens_with_a_list(self):
        self.edit("docs/writeup/WRITEUP.template.md", "The demo decodes", "- The demo decodes")
        self.refused("the essay opens with a list")

    def test_a_cut_or_cause_note_left_in(self):
        self.edit("docs/writeup/WRITEUP.template.md", "## Method",
                  "<!-- CUT-OR-CAUSE: replace the last sentence -->\n\n## Method")
        self.refused("the template still holds 1 CUT-OR-CAUSE notes")


class NotJudged(Tree):
    def pend(self):
        self.figures["kinds"]["later"] = {"what": "a file still to be measured",
                                          "pending": "it is measured at the freeze"}
        self.figures["figures"]["later_figure"] = {"what": "a figure still to come",
                                                   "kind": "later", "host": "any",
                                                   "format": "{:d}"}
        self.save()
        self.edit("docs/writeup/WRITEUP.template.md", "## Method",
                  "It took {{later_figure}} s.\n\n## Method")

    def test_a_token_whose_kind_is_still_to_come_is_not_judged(self):
        self.pend()
        done = self.run_script("--check")
        self.assertEqual(done.returncode, RENDER.EXIT_UNJUDGED, done.stderr)
        self.assertIn("not judged: 1 of 7 tokens have no results file yet", done.stdout)
        self.assertIn("waiting, by kind of file: later (any) 1", done.stdout)
        self.assertFalse((self.root / "WRITEUP.md").exists())

    def test_a_missing_file_is_not_judged_until_the_page_exists(self):
        (self.root / RESULT).unlink()
        done = self.run_script("--check")
        self.assertEqual(done.returncode, RENDER.EXIT_UNJUDGED, done.stderr)
        self.assertIn("not judged: 4 of 6 tokens have no results file yet", done.stdout)
        self.assertIn("demo (macOS) 4", done.stdout)

    def test_a_missing_file_fails_once_the_page_exists(self):
        self.assertEqual(self.run_script().returncode, 0)
        (self.root / RESULT).unlink()
        done = self.run_script("--check")
        self.assertEqual(done.returncode, RENDER.EXIT_STALE)
        self.assertIn("WRITEUP.md exists, and 4 of its tokens have no results file", done.stderr)

    def test_not_judged_still_holds_every_rule_on_what_resolves(self):
        self.pend()
        self.edit("docs/writeup/WRITEUP.template.md", "## Method", "It ran 12 times.\n\n## Method")
        self.refused("the template types the number '12' by hand")

    def test_a_draft_holds_what_resolves_and_the_waiting_tokens(self):
        self.pend()
        draft = self.root / "draft.md"
        self.assertEqual(self.run_script("--draft", str(draft)).returncode, RENDER.EXIT_UNJUDGED)
        text = draft.read_text(encoding="utf-8")
        self.assertIn("decodes at 1.23 ms", text)
        self.assertIn("It took {{later_figure}} s.", text)

    def test_check_sh_reports_not_judged_as_a_skip_and_a_failure_under_no_skips(self):
        tree = self.exported_copy()
        for name in ("docs/writeup", "results", "scripts/targets.json", "include"):
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
        self.assertIn("not judged: 1 of 7 tokens have no results file yet", done.stdout)
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


class TheList(unittest.TestCase):
    """The committed figure list and template, as the render reads them."""

    def test_no_kind_names_a_commit_and_every_entry_says_what_it_is(self):
        figures = json.loads((REPO_ROOT / RENDER.DEFAULT_FIGURES).read_text(encoding="utf-8"))
        for kind, spec in figures["kinds"].items():
            for pattern in spec.get("files", {}).values():
                RENDER.check_pattern(kind, pattern)
        for name, entry in figures["figures"].items():
            self.assertTrue(entry.get("what"), name)

    def test_every_verdict_token_is_a_verdict_read_through_render_results(self):
        figures = json.loads((REPO_ROOT / RENDER.DEFAULT_FIGURES).read_text(encoding="utf-8"))
        for name, entry in figures["figures"].items():
            if name.startswith("verdict:"):
                self.assertTrue("verdict" in entry or "words" in entry or
                                entry.get("format") == "text", name)


if __name__ == "__main__":
    unittest.main()
