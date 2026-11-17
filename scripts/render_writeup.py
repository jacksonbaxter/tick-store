#!/usr/bin/env python3
"""Render WRITEUP.md, the project's essay, from its template and its figure list, and check it.

    python3 -I scripts/render_writeup.py           write WRITEUP.md, once every token resolves
    python3 -I scripts/render_writeup.py --check   exit 1 if WRITEUP.md is not a fresh render
    python3 -I scripts/render_writeup.py --list    each token, its state and its value

docs/writeup/WRITEUP.template.md is the essay with a {{name}} token where each figure, verdict,
bar, constant and results path goes, and docs/writeup/figures.json, the figure list, says for each
token what it is, the kind of results file and the host it is read from, the field or the
computation, and its format. The template, the list, the page and scripts/targets.json are fixed
paths here, not chosen by the list. A results file is named by its kind and host, as a pattern
whose one * stands where the commit goes, so a file measured again still resolves and exactly one
file must match. A verdict is the Targets block's own, from scripts/render_results.py's code on
the row and parts scripts/targets.json gives, and a target's measured figure is read through its
row, so the bar beside it follows the same row. A size or limit the essay takes from the code is
read from the source by a pattern that must match one line of code.

What the render holds, each a refusal (exit 2) when it fails:
- every token is in the list, every entry is used, every value reaches a results file, a bar, a
  constant or a count of the tree at its leaves, and a literal operand or scale is one of the
  list's declared factors;
- every results file a token reads names the one measured commit, but for the kinds the list
  declares exceptions with their reasons, and the essay's own sentence names each exception it
  reads through a token; an exception that names another commit holds only where nothing the
  files read at the measured commit measure, by scripts/check_ties.py's kinds, differs between
  the two commits, as git tells where the history is present;
- a file under a pin of scripts/data_pins.json is read only by a kind that declares the pin,
  and every paragraph that shows one of its figures says so through the pin's token; a kind with
  a note, as the diagnostics' summaries have, puts the note in each paragraph that shows them;
- a figure read from one host is described as that host's, and one computed across hosts
  declares it;
- a bar is shown exactly, a measured figure beside it rounds the same way the verdict does, and
  a gap never prints as no gap;
- no number is typed in the template's prose, tables or the list's own words, digits or number
  words alike, but a declared non-result, each with its reason and the place it may appear;
- every sentence that ranks, compares or presumes a verdict is borne out by a claim of the list,
  whose condition reads the figures the sentence prints;
- the Missed targets table holds a row for exactly the targets missed in whole or in part, and
  the not-judged line exactly those not judged;
- every verdict carries beside its word the marks RESULTS.md gives it, from render_results.py's
  own functions: an outcome the replaced comparison run judges otherwise, a disturbed phase it
  rests on, and whose write input was where;
- no sentence says tick-store writes faster than a system whose write read its input from disk
  inside its clock, by render_results.py's rule for every page;
- the page opens, after its title, with one paragraph of exactly three sentences, each with a
  token, and runs to 2,500 to 4,000 words.

Until the measurements are committed, some kinds of results file do not exist yet, and WRITEUP.md
does not either. Then --check renders what resolves, applies every rule above to it, and exits
EXIT_UNJUDGED, naming how many tokens wait and their kinds: scripts/check.sh reports that as a
skip, a failure under --no-skips, never a pass. Once WRITEUP.md exists, a token with no file is a
failure, and so are a page that differs from a fresh render and a template that still holds a
CUT-OR-CAUSE note. What none of this checks is that a sentence describes its figure rightly, that
a verdict token sits beside the words for its own part outside a table, or that the files
themselves are sound: the validators of scripts/check.sh hold the results files, and none
re-reads the diagnostics' summaries.

Standard library only. tests/scripts/test_render_writeup.py shows each rule failing on a small
tree of its own.
"""

import argparse
import ast
import glob
import hashlib
import json
import math
import re
import unicodedata
import statistics
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import check_ties  # noqa: E402
import render_results  # noqa: E402

FIGURES_PATH = Path("docs") / "writeup" / "figures.json"
TEMPLATE_PATH = Path("docs") / "writeup" / "WRITEUP.template.md"
PAGE_PATH = Path("WRITEUP.md")
TARGETS_PATH = Path("scripts") / "targets.json"
PINS_PATH = Path("scripts") / "data_pins.json"
# The list names none of these: a key that tries to is refused.
FIXED_PATHS = {"template": TEMPLATE_PATH, "page": PAGE_PATH, "targets": TARGETS_PATH}
SCHEMA = "tick-store-writeup-figures-2"

EXIT_OK = 0
EXIT_STALE = 1
EXIT_REFUSED = 2
# Every rule held on what resolves, and some token has no results file yet: render_results.py's
# own code for a check that could not judge.
EXIT_UNJUDGED = render_results.EXIT_UNJUDGED

TOKEN = re.compile(r"\{\{([A-Za-z0-9_:-]+)\}\}")
COMMENT = re.compile(r"[ \t]*<!--.*?-->[ \t]*\n?", re.DOTALL)
# A number as prose writes it, render_results.py's NUMBER, not part of a name: digits that follow
# a letter, as in AVX2, M4, L3 or Q1, name something.
TYPED = re.compile(r"(?<![A-Za-z0-9])" + render_results.NUMBER.pattern)
# A multiplier glued to its number, "x3", which TYPED's start passes over as a name.
MULTIPLIER = re.compile(r"(?<![A-Za-z0-9])x[0-9]+")
# A number written in words. "One" names a unit far more often than it counts, so it is a number
# word like the rest: each use is a declared non-result or sits in a claim. "Double" is left out:
# here it names a floating-point type.
NUMBER_WORDS = re.compile(
    r"\b(?:one|two|three|four|five|six|seven|eight|nine|ten|eleven|twelve|thirteen|fourteen|"
    r"fifteen|sixteen|seventeen|eighteen|nineteen|twenty|thirty|forty|fifty|sixty|seventy|"
    r"eighty|ninety|hundreds?|thousands?|millions?|billions?|dozens?|half|halved|halves|twice|"
    r"thrice|doubled|tripled?|quarters?|once|orders? of magnitude)\b", re.IGNORECASE)
# Words that rank, compare or state an outcome: render_results.py's RANKING, and more.
JUDGING = re.compile(
    r"\b(?:faster|fastest|fast|slower|slowest|slows?|slowed|quicker|quickest|least|most|more|"
    r"less|fewer|fewest|smaller|smallest|larger|largest|lower|lowest|higher|highest|longer|"
    r"shorter|cheaper|costlier|better|best|worse|worst|below|above|beats?|beating|beaten|"
    r"outperforms?|wins?|won|loses|lost|lose|gave up|met|meets|missed|misses|miss|exceeds?|"
    r"exact|binds?|(?<=-)bound)\b", re.IGNORECASE)
# A number with a unit: what a measurement looks like, so never a declared non-result.
RESULT_SHAPED = re.compile(r"\d[\d,.]*\s*(?:%|×|x\b|[KMGT]i?B\b|B\b|bytes?\b|bits\b|ns\b|µs\b|"
                           r"ms\b|s\b|seconds?\b|cycles?\b|times\b)")
CHARACTER_REFERENCE = re.compile(r"&(?:#\d+|#[xX][0-9A-Fa-f]+|[A-Za-z][A-Za-z0-9]*);")
# A run of hexadecimal digits a results file's name carries as its commit. A day, eight digits, is
# not one.
COMMIT_IN_NAME = re.compile(r"[0-9a-f]{7,40}")
DAY_IN_NAME = re.compile(r"\d{8}")
HEX_RUN = re.compile(r"[0-9a-f]{6,}")
NUMBER_FORMAT = re.compile(r"\{:(,?)(?:\.(\d+)f|d)?\}")
COMPARATOR_SIGN = {"at_most": "≤", "at_least": "≥", "above": ">", "below": "<", "equals": "="}
CUT_NOTE = "CUT-OR-CAUSE"
MARKER = ("<!-- generated by scripts/render_writeup.py from docs/writeup/WRITEUP.template.md and "
          "docs/writeup/figures.json: edit those, never this file -->")
TABLE_RULE = re.compile(r"\s*\|?(?:\s*:?-+:?\s*\|)+\s*:?-*:?\s*")
# The essay's length, in words as word_count counts them: here, not in the figure list, so the
# list cannot widen it.
WORDS_AT_LEAST = 2500
WORDS_AT_MOST = 4000
# What the essay calls each host, so a figure read on one is never described as the other's.
HOST_WORDS = {"Linux": "Zen 1", "macOS": "M4"}
# The values a comparison of text may name: verdict words of render_results.py and the prover.
VERDICT_CODES = ("met", "missed", "not judged", "missed in part", "not judged in part",
                 "PROVEN", "UNDETECTED")
# The commit a results file names, shown as its files' names give it.
COMMIT_SHOWN = 12


class Refused(Exception):
    """A template or a figure list that cannot give a page the results back."""


class Waiting(Exception):
    """A token whose kind of results file is not committed yet, or that needs what this tree
    cannot give (a git history)."""

    def __init__(self, kind, host, why=None):
        super().__init__(f"{kind} on {host}")
        self.label = f"{kind} ({host})"
        self.why = why


def read_json(path, what):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise Refused(f"cannot read {what}: {error}") from error


def show(name):
    return f"{{{{{name}}}}}"


# --- The template -----------------------------------------------------------------------------

def body_of(template):
    """The template as the page shows it, its HTML comments taken out, and the comments."""
    return COMMENT.sub("", template), re.findall(r"<!--.*?-->", template, re.DOTALL)


def tokens_of(body):
    """Each token's name, in the order the body uses them. A {{ that opens no token refuses, and
    so do two tokens side by side, which print as one number."""
    names = TOKEN.findall(body)
    stray = TOKEN.sub("", body)
    if "{{" in stray or "}}" in stray:
        at = stray.find("{{") if "{{" in stray else stray.find("}}")
        raise Refused(f"the template holds {stray[at:at + 30]!r}, which is no token: a token is "
                      "{{name}}, a name of letters, digits, _, - and :")
    glued = re.search(r"\}\}\{\{|\}\}[0-9]|[0-9]\{\{", body)
    if glued:
        raise Refused(f"the template puts a token against another or a digit, "
                      f"{body[max(glued.start() - 20, 0):glued.end() + 20]!r}, which prints as "
                      "one number")
    reference = CHARACTER_REFERENCE.search(body)
    if reference:
        raise Refused(f"the template holds the character reference {reference.group(0)!r}, which "
                      "the page shows as a character no rule here sees")
    return names


def blocks_of(body):
    """The body's blocks, as the page shows them: paragraphs, list items and table rows. A
    table's rows are each a block, and its rule is none."""
    blocks = []
    for chunk in re.split(r"\n\s*\n", body):
        lines = [line for line in chunk.strip().splitlines() if line.strip()]
        if not lines:
            continue
        if lines[0].lstrip().startswith("|"):
            blocks += [line for line in lines if not TABLE_RULE.fullmatch(line)]
        elif lines[0].lstrip().startswith("- "):
            items, current = [], []
            for line in lines:
                if line.lstrip().startswith("- ") and current:
                    items.append("\n".join(current))
                    current = []
                current.append(line)
            blocks += items + ["\n".join(current)]
        else:
            blocks.append("\n".join(lines))
    return blocks


def sentences_of(block):
    """A block's sentences: a table row is one, and prose splits after ., ! or ? before a
    capital, a token or a bold lead-in."""
    if block.lstrip().startswith("|"):
        return [block]
    text = " ".join(block.split())
    return [part for part in re.split(r"(?<=[.!?])\s+(?=[A-Z*`{(\[])", text) if part]


def scoped(not_results, body):
    """The body with each declared non-result taken out where, and only where, it is declared to
    appear: each item gives the text and the words around it ("in"), each of which must occur
    exactly once, and the text must occur nowhere else."""
    text = body
    for item in not_results:
        if not isinstance(item, dict) or not item.get("text"):
            raise Refused(f"not_results: {item!r} gives no text")
        if not item.get("why"):
            raise Refused(f"not_results: {item['text']!r} is kept out of the number check with "
                          "no reason")
        if RESULT_SHAPED.search(item["text"]):
            raise Refused(f"not_results: {item['text']!r} holds a number with a unit, which is a "
                          "measurement: read it from a results file")
        places = item.get("in")
        if not isinstance(places, list) or not places or \
                any(not isinstance(place, str) or item["text"] not in place for place in places):
            raise Refused(f"not_results: {item['text']!r} names no place it may appear: give the "
                          "words around each use under \"in\"")
        for place in places:
            # Words as the page shows them: a line break is a space.
            pattern = r"\s+".join(re.escape(word) for word in place.split())
            found = list(re.finditer(pattern, text))
            if len(found) != 1:
                raise Refused(f"not_results: {item['text']!r} is declared in {place!r}, which the "
                              f"template holds {len(found)} times, not once")
            inner = r"\s+".join(re.escape(word) for word in item["text"].split())
            kept = re.sub(inner, " ", found[0].group(0))
            text = text[:found[0].start()] + kept + text[found[0].end():]
        left = re.search(rf"(?<![A-Za-z0-9]){re.escape(item['text'])}(?![A-Za-z0-9])", text)
        if left:
            raise Refused(f"not_results: {item['text']!r} appears outside the places declared "
                          f"for it, at {text[max(left.start() - 30, 0):left.end() + 30]!r}")
    return text


def typed_numbers(body, not_results):
    """Every number in digits in the body that is not a token, once each declared text is taken
    out where it is declared. A number in words is held sentence by sentence, with the words that
    rank or judge: claims_hold."""
    text = TOKEN.sub(" ", scoped(not_results, body))
    # A host's name is the script's own, never a result.
    for word in HOST_WORDS.values():
        text = re.sub(rf"\b{re.escape(word)}\b", " ", text)
    # A numeral other than an ASCII digit, "½", "²" or "Ⅻ", is a number too.
    numerals = [char for char in text if not char.isascii()
                and unicodedata.numeric(char, None) is not None]
    return TYPED.findall(text) + MULTIPLIER.findall(text) + numerals


def word_count(text):
    """Words of the page: its comments and its tables' rules and pipes left out. A word holds a
    letter or a digit; a token not yet resolved counts as one."""
    text = body_of(text)[0]
    lines = [line for line in text.splitlines() if not TABLE_RULE.fullmatch(line)]
    return sum(1 for word in "\n".join(lines).replace("|", " ").split()
               if re.search(r"[A-Za-z0-9]", word))


def opening_problem(body):
    """Why the template does not open with its conclusion, or None: the title, one # heading,
    then one paragraph of exactly three sentences, each holding a token, before any section."""
    blocks = [block.strip() for block in re.split(r"\n\s*\n", body) if block.strip()]
    if not blocks or not re.fullmatch(r"# \S[^\n]*", blocks[0]):
        return "the page does not open with its title, one # heading"
    if len(blocks) < 2 or blocks[1].startswith("#"):
        return "a section heading comes before any prose: the essay opens with its conclusion"
    first = blocks[1]
    if re.match(r"([-*+>|]|\d+\.|```)", first):
        return "the essay opens with a list, a table, a quote or code, not with its conclusion"
    sentences = sentences_of(first)
    if len(sentences) != 3 or not sentences[-1].rstrip().endswith((".", "!", "?")):
        return (f"the essay's opening paragraph holds {len(sentences)} sentences; it is its "
                "conclusion in exactly three")
    bare = [sentence for sentence in sentences if not TOKEN.search(sentence)]
    if bare:
        return (f"the opening sentence {bare[0][:60]!r} holds no token: each of the three states "
                "a result the files give")
    return None


# --- Results files by kind --------------------------------------------------------------------

class Context:
    """What an expression reads: the resolver, its entry and its one document."""

    def __init__(self, resolver, name, entry, document=None, pattern=None):
        self.resolver = resolver
        self.name = name
        self.entry = entry
        self.document = document
        self.pattern = pattern

    def need_document(self, form):
        if self.document is None:
            raise Refused(f"{show(self.name)}: {form} reads a results file, and its entry "
                          "names no kind")
        return self.document


def check_pattern(kind, pattern):
    """A kind's file pattern: inside results/, one file name, and at most one *, standing between
    a - and .json where the commit goes, so no prefix of a commit and no commit in a folder can
    pick an old file over a new one."""
    if not isinstance(pattern, str) or not pattern.startswith("results/"):
        raise Refused(f"kind {kind!r}: {pattern!r} is not a pattern under results/")
    parts = Path(pattern).parts
    if ".." in parts or any(set(part) & set("?[]") for part in parts):
        raise Refused(f"kind {kind!r}: {pattern!r} leaves results/ or uses a glob other than *")
    for folder in parts[:-1]:
        if "*" in folder or HEX_RUN.search(folder):
            raise Refused(f"kind {kind!r}: {pattern!r} names a commit or a glob in a folder")
    name = parts[-1]
    if name.count("*") > 1 or ("*" in name and not name.endswith("-*.json")):
        raise Refused(f"kind {kind!r}: {pattern!r} has a * other than one in place of the "
                      "commit, between - and .json")
    for run in COMMIT_IN_NAME.findall(name):
        if not DAY_IN_NAME.fullmatch(run):
            raise Refused(f"kind {kind!r}: {pattern!r} names the commit {run!r}; a file measured "
                          "again carries a new one, so name the kind with * in its place")


def pins_of(root):
    """Each pin of scripts/data_pins.json that names files, as {pin: {file name: sha256}}."""
    path = Path(root) / PINS_PATH
    if not path.exists():
        return {}
    document = read_json(path, PINS_PATH.as_posix())
    return {name: value for name, value in document.items()
            if isinstance(value, dict) and value
            and all(isinstance(key, str) and key.endswith(".json") and isinstance(sha, str)
                    and re.fullmatch(r"[0-9a-f]{64}", sha) for key, sha in value.items())}


def pin_of(root, path, pins):
    """The pin a results file is held under, by its name and bytes, or None."""
    digest = None
    for pin, files in pins.items():
        if path.name in files:
            digest = digest or hashlib.sha256(path.read_bytes()).hexdigest()
            if files[path.name] == digest:
                return pin
    return None


class Resolver:
    def __init__(self, root, figures):
        self.root = Path(root)
        self.figures = figures
        self.entries = figures.get("figures") or {}
        self.kinds = figures.get("kinds") or {}
        self.factors = factors_of(figures)
        self.raw_values = {}
        self.busy = []
        self.targets = None
        self.documents = {}
        self.pins = pins_of(root)
        # Every results file read, with its kind, and what each token's value reads.
        self.read = {}
        self.deps = {}
        self.reading = []

    # Kinds and files.
    def pattern_of(self, kind, host, name):
        spec = self.kinds.get(kind)
        if spec is None:
            raise Refused(f"{show(name)}: no kind {kind!r} in the figure list")
        if "pending" in spec:
            if not spec.get("pending"):
                raise Refused(f"kind {kind!r}: pending with no reason")
            raise Waiting(kind, host, spec["pending"])
        files = spec.get("files") or {}
        if host not in files:
            raise Refused(f"{show(name)}: kind {kind!r} has no file on the host {host!r}")
        pattern = files[host]
        check_pattern(kind, pattern)
        return pattern

    def file_of(self, kind, host, name):
        """The one results file of a kind on a host, or Waiting if there is none yet. A kind
        whose files are one a run, such as the disturbance records, names under record_of the
        kind whose run it records: its file is the one its pattern gives at that kind's file's
        commit, so no commit is named and a run measured again carries its own."""
        pattern = self.pattern_of(kind, host, name)
        spec = self.kinds[kind]
        try:
            if "record_of" in spec:
                other = spec["record_of"]
                other_host = host if host in (self.kinds.get(other, {}).get("files") or {}) \
                    else "any"
                run, _ = self.file_of(other, other_host, name)
                commit = commit_of(self, run, other)
                if commit is None:
                    raise Refused(f"kind {kind!r}: the kind {other!r} it records names no commit")
                pattern = pattern.replace("*", commit[:COMMIT_SHOWN])
            path = render_results.target_file(self.root, pattern)
        except render_results.TargetError as error:
            raise Refused(f"{show(name)}: {error}, and a token reads one") from error
        if path is None:
            raise Waiting(kind, host)
        self.note_read(path, kind, host, name)
        return path, pattern

    def note_read(self, path, kind, host, name):
        """A file read: its pin must be declared by its kind, and the token reading it must
        say so."""
        pin = pin_of(self.root, path, self.pins)
        declared = (self.kinds[kind].get("pin") or {}).get("name")
        if pin and pin != declared:
            raise Refused(f"{show(name)} reads {path.name}, which is held under the pin {pin!r} "
                          f"of {PINS_PATH.as_posix()}; its kind {kind!r} must declare the pin, and "
                          "the paragraph must say so")
        if declared and not pin:
            raise Refused(f"kind {kind!r} declares the pin {declared!r}, and {path.name} is not "
                          "held under it")
        self.read[path] = (kind, host)
        for found in self.reading:
            found.add((path, kind, host))

    def kind_of_pattern(self, pattern, what):
        """The kind and host of the figure list whose file pattern scripts/targets.json uses."""
        found = [(kind, host) for kind, spec in self.kinds.items()
                 for host, own in (spec.get("files") or {}).items() if own == pattern]
        if len(found) != 1:
            raise Refused(f"{what} reads {pattern}, and {len(found)} kinds of the figure list "
                          "name that pattern, not one")
        return found[0]

    def document_of(self, path):
        if path not in self.documents:
            self.documents[path] = read_json(path, path.relative_to(self.root).as_posix())
        return self.documents[path]

    def targets_file(self):
        if self.targets is None:
            self.targets = read_json(self.root / TARGETS_PATH, TARGETS_PATH.as_posix())
        return self.targets

    def target_row(self, spec, name):
        rows = [row for row in self.targets_file().get("rows", [])
                if (row.get("id"), row.get("host")) == (spec.get("id"), spec.get("host"))]
        if len(rows) != 1:
            raise Refused(f"{show(name)}: no one target row {spec.get('id')} on "
                          f"{spec.get('host')} in scripts/targets.json")
        return rows[0]

    def judged(self, row, name):
        """A target row's parts, judged by render_results.py on the files of the kinds its
        patterns name, which are read as any token's; render_results.py judges a comparison row
        on the replaced run's pinned file too, and notes each part's marks."""
        specs = [] if "no_figure" in row else [row["lowest_of"]] if "lowest_of" in row \
            else row.get("parts", [])
        own = json.loads(json.dumps(row))
        for spec in specs:
            kind, host = self.kind_of_pattern(spec["file"], f"{row['id']} on {row['host']}")
            try:
                path, _ = self.file_of(kind, host, name)
                self.document_of(path)
            except Waiting:
                if "if_missing" not in row:
                    raise
        try:
            return render_results.judged_parts(own, self.root)
        except (render_results.TargetError, KeyError, TypeError, ZeroDivisionError) as error:
            raise Refused(f"{show(name)}: {error}") from error

    # Values.
    def entry(self, name):
        if name not in self.entries:
            raise Refused(f"{show(name)} is not in the figure list")
        entry = self.entries[name]
        if not isinstance(entry, dict) or not entry.get("what"):
            raise Refused(f"{show(name)}: its entry says nothing of what it is")
        return entry

    def context(self, name, entry):
        if "kind" not in entry:
            return Context(self, name, entry)
        path, pattern = self.file_of(entry["kind"], entry.get("host", "any"), name)
        return Context(self, name, entry, self.document_of(path), pattern)

    def raw(self, name):
        """A token's value before its format: a number, a string, a list or true or false."""
        if name in self.raw_values:
            value = self.raw_values[name]
            for found in self.reading:
                found.update(self.deps.get(name, set()))
            if isinstance(value, Waiting):
                raise value
            return value
        if name in self.busy:
            raise Refused(f"{show(name)} is computed from itself: " + " -> ".join(self.busy))
        entry = self.entry(name)
        self.busy.append(name)
        reads = set()
        self.reading.append(reads)
        try:
            value = self.value_of(name, entry)
        except Waiting as waiting:
            self.raw_values[name] = waiting
            raise
        except (render_results.TargetError, render_results.NoFigure, KeyError, TypeError,
                ValueError, ZeroDivisionError, IndexError) as error:
            raise Refused(f"{show(name)}: {error}") from error
        finally:
            self.busy.remove(name)
            self.reading.pop()
            self.deps[name] = reads
            for found in self.reading:
                found.update(reads)
        self.raw_values[name] = value
        check_hosts(name, entry, reads)
        return value

    def value_of(self, name, entry):
        if "bar" in entry:
            return bar_value(self.target_row(entry["bar"], name), name)
        if "target" in entry:
            return self.target_figure(name, entry)
        if "verdict" in entry:
            return verdict_value(self, name, entry)
        # The file first: a kind still to come has no value to give yet.
        context = self.context(name, entry)
        if "value" not in entry:
            raise Refused(f"{show(name)}: its entry gives no value, bar, target, verdict or "
                          "path")
        refuse_not_established(context, entry["value"])
        return evaluate(entry["value"], context)

    def target_figure(self, name, entry):
        """A target's measured figure, read through its row's part, so it is the Targets
        block's own."""
        spec = entry["target"]
        row = self.target_row(spec, name)
        parts, reason = self.judged(row, name)
        if parts is None:
            raise Refused(f"{show(name)}: {row['id']} on {row['host']} is {reason}")
        found = [part for part in parts if part[0] == spec.get("part")]
        if len(found) != 1:
            raise Refused(f"{show(name)}: no part {spec.get('part')!r} in "
                          f"{[part[0] for part in parts]}")
        items = [item for item in ([row["lowest_of"]] if "lowest_of" in row else row["parts"])
                 if item.get("label") == spec["part"]]
        if len(items) != 1 or "value" not in items[0]:
            raise Refused(f"{show(name)}: {row['id']}'s part {spec['part']!r} has no one value")
        return render_results.evaluate(items[0]["value"], found[0][4])

    def shown(self, name):
        """A token as the page shows it."""
        entry = self.entry(name)
        if entry.get("path") is True:
            path, _ = self.file_of(entry.get("kind"), entry.get("host", "any"), name)
            return path.relative_to(self.root).as_posix()
        if "repo_file" in entry:
            path = self.root / entry["repo_file"]
            if not path.is_file():
                raise Refused(f"{show(name)}: {entry['repo_file']} is not in the tree")
            return entry["repo_file"]
        for special in ("measured_commit", "commit_exceptions", "pin_of", "note_of"):
            if special in entry:
                raise Refused(f"{show(name)}: {special} is shown once the page is read")
        value = self.raw(name)
        if "verdict" in entry:
            return value
        if "bar" in entry:
            row = self.target_row(entry["bar"], name)
            return shown_bar(name, entry, row, value)
        if "target" in entry:
            row = self.target_row(entry["target"], name)
            return shown_target(self, name, entry, row, value)
        text = formatted(value, entry, name)
        check_gap(name, entry, text)
        return text


def factors_of(figures):
    """The literal numbers the list may use, each with its reason: unit factors, mostly."""
    found = {}
    for item in figures.get("factors") or []:
        if not isinstance(item, dict) or not item.get("why") or \
                isinstance(item.get("value"), bool) or \
                not isinstance(item.get("value"), (int, float)):
            raise Refused(f"factors: {item!r} is not a number with its reason")
        found[item["value"]] = item["why"]
    return found


def check_hosts(name, entry, reads):
    """A value read on one host is described as that host's, and one read on two declares it."""
    hosts = {host for _, _, host in reads if host in HOST_WORDS}
    what = entry.get("what", "")
    if len(hosts) > 1:
        if not entry.get("across_hosts"):
            raise Refused(f"{show(name)} is computed from files of {sorted(hosts)}; an entry "
                          "across hosts says so under across_hosts, with its reason")
        missing = [HOST_WORDS[host] for host in hosts if HOST_WORDS[host] not in what]
        if missing:
            raise Refused(f"{show(name)} reads {sorted(hosts)}, and its description does not "
                          f"name {missing}")
        return
    for host in hosts:
        other = [word for key, word in HOST_WORDS.items() if key != host and word in what]
        if other or HOST_WORDS[host] not in what:
            raise Refused(f"{show(name)} is read on {host}, and its description "
                          f"{what!r} does not call it the {HOST_WORDS[host]}'s")


# The keys of the figure list's forms whose list is a path into the entry's document.
PATH_KEYS = ("path", "where", "items", "count")


def read_paths(expression):
    """Every path into the document an expression reads."""
    if isinstance(expression, dict):
        found = [value for key, value in expression.items()
                 if key in PATH_KEYS and isinstance(value, list)]
        return found + [path for value in expression.values() for path in read_paths(value)]
    if isinstance(expression, list):
        return [path for value in expression for path in read_paths(value)]
    return []


def covers(listed, path):
    """Whether a path the file lists as not established and a path a token reads overlap: one
    is the start of the other."""
    return all(one == two for one, two in zip(listed, path))


def refuse_not_established(context, expression):
    """A results file lists, under not_established, what its raw output could not show, with
    the path it covers. A token that reads such a path would state as fact what the run does
    not establish: it is refused, with the file's reason."""
    document = context.document
    if not isinstance(document, dict) or "not_established" not in document:
        return
    listed = document["not_established"]
    if not isinstance(listed, list) or any(not isinstance(item, dict)
                                           or not isinstance(item.get("path"), list)
                                           for item in listed):
        raise Refused(f"{show(context.name)}: its file's not_established is no list of "
                      "entries each with a path")
    for item in listed:
        for path in read_paths(expression):
            if covers(item["path"], path):
                raise Refused(f"{show(context.name)} reads {path}, and its file says "
                              f"{item.get('what')} is not established there: {item.get('why')}")


def bar_comparator(row, name):
    target = row.get("target") or {}
    found = [key for key in target if key in COMPARATOR_SIGN]
    if len(found) != 1:
        raise Refused(f"{show(name)}: {row.get('id')} on {row.get('host')} has no one "
                      "comparator and value for a target")
    return found[0]


def bar_value(row, name):
    value = row["target"][bar_comparator(row, name)]
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise Refused(f"{show(name)}: {row.get('id')}'s target {value!r} is not a number")
    return value


def printed_number(text):
    found = render_results.NUMBER.search(text)
    return float(found.group(0).replace(",", "")) if found else None


def shown_bar(name, entry, row, value):
    """A bar as the page shows it: its sign, the row's value at the row's scale, exactly, and the
    row's unit."""
    scaled = value * row.get("scale", 1)
    if "format" not in entry:
        # The fewest digits that show the bar as it is.
        digits = next((digits for digits in range(7)
                       if math.isclose(round(scaled, digits), scaled, rel_tol=1e-12, abs_tol=0)),
                      6)
        entry = dict(entry, format=f"{{:,.{digits}f}}")
    text = number_text(scaled, entry, name)
    if not math.isclose(printed_number(text), scaled, rel_tol=1e-12, abs_tol=0):
        raise Refused(f"{show(name)} shows {row['id']}'s bar {scaled!r} as {text}, which is not "
                      "its value: give its format the digits the bar has")
    return f"{COMPARATOR_SIGN[bar_comparator(row, name)]} {text}{row.get('unit', '')}"


def shown_target(resolver, name, entry, row, value):
    """A target's measured figure at the row's scale with the row's unit. As printed, it must
    stand on the same side of the bar as the verdict says, so a rounding never shows a miss as
    met or a met as missed."""
    if "format" in entry:
        text = number_text(value * row.get("scale", 1), entry, name) + row.get("unit", "")
    else:
        # The Targets block's own: the row's scale, digits and unit.
        text = render_results.shown(value, row)
    comparator = (row.get("target") or {}) and bar_comparator(row, name)
    if comparator:
        parts, _ = resolver.judged(row, name)
        met = [part[2] for part in parts or [] if part[0] == entry["target"]["part"]]
        bar = bar_value(row, name) * row.get("scale", 1)
        printed = render_results.COMPARATORS[comparator][0](printed_number(text), bar)
        if met and isinstance(met[0], bool) and printed != met[0]:
            raise Refused(f"{show(name)} prints {text} against the bar {bar}, which reads "
                          f"{'met' if printed else 'missed'} where the part is "
                          f"{'met' if met[0] else 'missed'}: give it more digits")
    return text


def check_gap(name, entry, text):
    """A gap is never printed as none: a ratio as 1, a difference as 0."""
    gap = entry.get("gap")
    if gap is None:
        return
    if gap not in ("ratio", "difference"):
        raise Refused(f"{show(name)}: gap is ratio or difference, not {gap!r}")
    printed = printed_number(text)
    if printed == (1.0 if gap == "ratio" else 0.0):
        raise Refused(f"{show(name)} prints the gap {text}, which reads as none: give it more "
                      "digits")


# --- Expressions ------------------------------------------------------------------------------

# Forms read from scripts/targets.json's expressions, evaluated by render_results.py itself.
SHARED_FORMS = ("path", "case", "cases_median", "best", "codec", "store_bits", "min_variant",
                "lowest_system", "lowest_value", "engine_only")
# The figure list's own forms that read the entry's results file.
DOCUMENT_FORMS = ("codec_columns", "codec_column", "least", "items", "where", "count",
                  "count_cases", "flagged_queries", "target_values", "flagged")
# Forms that read the tree, the code or the targets, not a results file.
TREE_FORMS = ("bar", "constant", "lines", "count_matches", "prover", "verdict_is")
OPERATORS = ("sub", "add", "mul", "div", "same", "log2", "pow2", "min", "max", "median",
             "each", "all", "any", "above", "below", "is", "concat", "prefix", "not")


def grounding(expression, name, factors, kind):
    """Whether an expression reaches a results file, the tree or another figure at a leaf:
    True or False, refusing a literal that is not a declared factor and a text with a number in
    it."""
    if isinstance(expression, bool):
        raise Refused(f"{show(name)}: the literal {expression!r} is no expression")
    if isinstance(expression, (int, float)):
        if expression not in factors:
            raise Refused(f"{show(name)}: the literal {expression!r} is no declared factor; a "
                          "figure comes from a file, and a factor is listed with its reason")
        return False
    if isinstance(expression, str):
        return False
    if isinstance(expression, list):
        found = [grounding(item, name, factors, kind) for item in expression]
        return any(found)
    if not isinstance(expression, dict):
        raise Refused(f"{show(name)}: {expression!r} is no expression")
    if "figure" in expression or any(form in expression for form in TREE_FORMS):
        return True
    if any(form in expression for form in (*SHARED_FORMS, *DOCUMENT_FORMS)):
        if kind is None:
            raise Refused(f"{show(name)}: {next(iter(expression))} reads a results file, and its "
                          "entry names no kind")
        return True
    known = [key for key in expression if key in OPERATORS]
    if not known:
        raise Refused(f"{show(name)}: unknown expression {sorted(expression)}")
    if "concat" in expression:
        for item in expression["concat"]:
            if isinstance(item, str) and (TYPED.search(item) or NUMBER_WORDS.search(item)):
                raise Refused(f"{show(name)}: the text {item!r} types a number")
    if "is" in expression:
        literal = expression["is"][1] if isinstance(expression["is"], list) and \
            len(expression["is"]) == 2 else None
        if literal not in VERDICT_CODES:
            raise Refused(f"{show(name)}: is compares with {literal!r}, which is no verdict "
                          f"word of {VERDICT_CODES}")
        return grounding(expression["is"][0], name, factors, kind)
    operands = [value for key, value in expression.items()
                if key not in ("length", "of_kind")]
    return any(grounding(item, name, factors, kind) for item in operands)


def substitute(expression, item):
    if isinstance(expression, str):
        return item if expression == "$" else expression.replace("$", str(item))
    if isinstance(expression, list):
        return [substitute(value, item) for value in expression]
    if isinstance(expression, dict):
        return {key: substitute(value, item) for key, value in expression.items()}
    return expression


def number(value, what):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise Refused(f"{what}: {value!r} is not a number")
    return value


def values_of(operand, context):
    """A list of values: a list of expressions, each evaluated, or one that gives a list."""
    if isinstance(operand, list):
        found = []
        for item in operand:
            value = evaluate(item, context)
            found += value if isinstance(value, list) else [value]
        return found
    value = evaluate(operand, context)
    if not isinstance(value, list):
        raise Refused(f"{show(context.name)}: {operand!r} gives no list")
    return value


def walk(document, path):
    """render_results.walk, with {"name": n} picking the one item of a list so named and a whole
    number the item at that place."""
    here, keys = document, []
    for key in path:
        if isinstance(key, int) and not isinstance(key, bool):
            here = render_results.walk(here, keys) if keys else here
            keys = []
            if not isinstance(here, list) or not -len(here) <= key < len(here):
                raise render_results.TargetError(f"no item {key} on {path}")
            here = here[key]
        elif isinstance(key, dict):
            here = render_results.walk(here, keys) if keys else here
            keys = []
            if not isinstance(here, list):
                raise render_results.TargetError(f"no list to pick {key} from on {path}")
            found = [item for item in here if isinstance(item, dict)
                     and all(item.get(field) == value for field, value in key.items())]
            if len(found) != 1:
                raise render_results.TargetError(f"{len(found)} items match {key} on {path}")
            here = found[0]
        else:
            keys.append(key)
    return render_results.walk(here, keys) if keys else here


def source_text(context, source):
    path = context.resolver.root / source
    try:
        return path.read_text(encoding="utf-8")
    except OSError as error:
        raise Refused(f"{show(context.name)}: cannot read {source}: {error}") from error


def code_lines(text):
    """The source with its comments blanked, line for line, so a pattern matches code only."""
    text = re.sub(r"/\*.*?\*/", lambda found: re.sub(r"[^\n]", " ", found.group(0)), text,
                  flags=re.DOTALL)
    return "\n".join(re.sub(r"//.*$|^\s*#(?!\s*(?:define|include)).*$", "", line)
                     for line in text.splitlines())


def constant(spec, context):
    """A constant from a source file: the one line of code, comments left out, its pattern
    matches, group 1 its value."""
    until = spec.get("until")
    path = context.resolver.root / spec["source"]
    if until and not path.exists():
        raise Waiting("code", "a merge", until)
    text = code_lines(source_text(context, spec["source"]))
    found = list(re.finditer(spec["pattern"], text, re.MULTILINE))
    if until and not found:
        raise Waiting("code", "a merge", until)
    if until:
        raise Refused(f"{show(context.name)}: {spec['pattern']!r} now matches in "
                      f"{spec['source']}, and its entry still waits {until}: drop until")
    if len(found) != 1:
        raise Refused(f"{show(context.name)}: {spec['pattern']!r} matches {len(found)} lines "
                      f"of code in {spec['source']}, not one: the constant moved or changed "
                      "form, so the figure list must say again where it is")
    digits = found[0].group(1)
    return float(digits) if "." in digits else int(digits)


def exact_codecs(column, expression):
    return [codec for codec in column["codecs"]
            if codec.get("family") == expression.get("family", codec.get("family"))
            and codec.get("family") != expression.get("not_family")
            and codec.get("exact") is True]


def listed_faults(text, name):
    """The faults scripts/prove_checks.py lists, and which are controls, read from its FAULTS
    tuple as Python reads it."""
    try:
        tree = ast.parse(text)
    except SyntaxError as error:
        raise Refused(f"{show(name)}: the prover does not parse: {error}") from error
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(isinstance(target, ast.Name) and
                                                target.id == "FAULTS" for target in node.targets):
            faults, controls = [], []
            for call in getattr(node.value, "elts", []):
                if not (isinstance(call, ast.Call) and getattr(call.func, "id", "") == "Fault"
                        and call.args and isinstance(call.args[0], ast.Constant)):
                    raise Refused(f"{show(name)}: FAULTS holds something other than a Fault")
                control = any(keyword.arg == "control" and
                              getattr(keyword.value, "value", False) is True
                              for keyword in call.keywords)
                (controls if control else faults).append(call.args[0].value)
            if not faults:
                raise Refused(f"{show(name)}: FAULTS lists no fault")
            return faults, controls
    raise Refused(f"{show(name)}: the prover has no FAULTS")


def prover(spec, context):
    """What the prover's record says of the faults the prover lists: "listed", the faults with
    no control; "covered", those the record holds; "proven", those it holds as PROVEN; and
    "controls_passed", whether every control it holds went unnoticed, as a control must."""
    faults, controls = listed_faults(source_text(context, spec["source"]), context.name)
    document = context.need_document("prover")
    entries = {item.get("fault_id"): item.get("verdict")
               for item in walk(document, ["faults"]) if isinstance(item, dict)}
    # A fault the record holds and the prover no longer lists is counted nowhere: the record
    # covers what it covers of the list as it stands.
    wanted = spec.get("count")
    if wanted == "listed":
        return len(faults)
    if wanted == "covered":
        return sum(1 for fault in faults if fault in entries)
    if wanted == "proven":
        return sum(1 for fault in faults if entries.get(fault) == "PROVEN")
    if wanted == "controls_passed":
        held = [entries[control] for control in controls if control in entries]
        if not held:
            raise Refused(f"{show(context.name)}: the record holds no control")
        return all(verdict == "UNDETECTED" for verdict in held)
    raise Refused(f"{show(context.name)}: prover counts listed, covered, proven or "
                  "controls_passed, not {wanted!r}")


def flagged(reads, context):
    """Whether any of the timed phases named is one render_results.py marks: a disturbance the
    tree's disturbance records place in it, or a query block its record flags as timed while a
    page was swapped in. Each read names a system, or a figure whose value is one, and its
    headline variant's class B write or a query and kind."""
    document = context.need_document("flagged")
    entry = context.entry
    path, _ = context.resolver.file_of(entry["kind"], entry["host"], context.name)
    phases = []
    for read in reads:
        system = read["system"]
        if isinstance(system, dict):
            system = evaluate(system, context)
        # A read names its variant, or takes the system's headline one.
        variant = read.get("variant") or walk(document, ["systems", system, "headline_variant"])
        if "write" in read:
            phases.append(("write", system, variant, read["write"]))
        else:
            phases.append(("queries", system, variant, read["kind"], read["query"]))
    root = context.resolver.root
    return bool(render_results.disturbed(render_results.disturbance_entries(root), root, path,
                                         phases) or
                render_results.swapped_reads(phases, document))


def evaluate(expression, context):
    """A value by the figure list's expressions; scripts/targets.json's own forms go to
    render_results.evaluate on the entry's document."""
    name = show(context.name)
    if isinstance(expression, (int, float)) and not isinstance(expression, bool):
        return expression
    if isinstance(expression, str):
        return expression
    if not isinstance(expression, dict):
        raise Refused(f"{name}: {expression!r} is no expression")
    if "figure" in expression:
        return context.resolver.raw(expression["figure"])
    if "bar" in expression:
        return bar_value(context.resolver.target_row(expression["bar"], context.name),
                         context.name)
    if "verdict_is" in expression:
        spec = expression["verdict_is"]
        row = context.resolver.target_row(spec, context.name)
        parts, _ = context.resolver.judged(row, context.name)
        return verdict_class_of(parts, spec, context.name) == spec.get("class")
    if "constant" in expression:
        return constant(expression["constant"], context)
    if "lines" in expression:
        files = []
        for pattern in expression["lines"]:
            found = sorted(Path(path) for path in glob.glob(str(context.resolver.root / pattern),
                                                            recursive=True)
                           if Path(path).is_file())
            if not found:
                raise Refused(f"{name}: {pattern} matches no file")
            files += found
        return sum(path.read_bytes().count(b"\n") for path in dict.fromkeys(files))
    if "count_matches" in expression:
        text = source_text(context, expression["source"])
        if "between" in expression:
            start, end = (re.search(pattern, text, re.MULTILINE)
                          for pattern in expression["between"])
            if start is None or end is None or end.start() < start.end():
                raise Refused(f"{name}: {expression['source']} has no "
                              f"{expression['between']} to count between")
            text = text[start.end():end.start()]
        return len(re.findall(expression["count_matches"], text, re.MULTILINE))
    if "prover" in expression:
        return prover(expression["prover"], context)
    for key, operation in (("sub", lambda a, b: a - b), ("add", lambda a, b: a + b),
                           ("mul", lambda a, b: a * b), ("div", lambda a, b: a / b)):
        if key in expression:
            first, second = (number(evaluate(item, context), name)
                             for item in expression[key])
            return operation(first, second)
    if "same" in expression:
        found = [number(value, name) for value in values_of(expression["same"], context)]
        if len(found) < 2 or len(set(found)) != 1:
            raise Refused(f"{name} stands for {len(found)} values that the "
                          f"template says are one, and they differ: {found}")
        return found[0]
    if "log2" in expression:
        value = number(evaluate(expression["log2"], context), name)
        if not isinstance(value, int) or value < 1 or value & (value - 1):
            raise Refused(f"{name}: {value!r} is not a power of two")
        return value.bit_length() - 1
    if "pow2" in expression:
        value = number(evaluate(expression["pow2"], context), name)
        if not isinstance(value, int) or value < 0:
            raise Refused(f"{name}: 2 to the {value!r} is no whole power")
        return 1 << value
    for key in ("min", "max", "median"):
        if key in expression:
            values = [number(value, name) for value in values_of(expression[key], context)]
            if "only_above" in expression:
                floor = evaluate(expression["only_above"], context)
                values = [value for value in values if value > floor]
            if "only_below" in expression:
                ceiling = evaluate(expression["only_below"], context)
                values = [value for value in values if value < ceiling]
            if not values:
                raise Refused(f"{name}: no value to take the {key} of")
            return {"min": min, "max": max, "median": statistics.median}[key](values)
    if "each" in expression:
        over = expression["each"]
        items = over if isinstance(over, list) else evaluate(over, context)
        if not isinstance(items, list) or not items:
            raise Refused(f"{name}: each over {over!r}, which gives no items")
        return [evaluate(substitute(expression["of"], item), context) for item in items]
    for key, combine in (("all", all), ("any", any)):
        if key in expression:
            found = values_of(expression[key], context)
            if not found or any(not isinstance(value, bool) for value in found):
                raise Refused(f"{name}: {key} of {found!r}, which are not true or false")
            return combine(found)
    if "not" in expression:
        value = evaluate(expression["not"], context)
        if not isinstance(value, bool):
            raise Refused(f"{name}: not of {value!r}, which is not true or false")
        return not value
    for key, operation in (("above", lambda a, b: a > b), ("below", lambda a, b: a < b)):
        if key in expression:
            first, second = (number(evaluate(item, context), name)
                             for item in expression[key])
            return operation(first, second)
    if "is" in expression:
        return evaluate(expression["is"][0], context) == expression["is"][1]
    if "concat" in expression:
        pieces = []
        for item in expression["concat"]:
            value = item if isinstance(item, str) else evaluate(item, context)
            if not isinstance(value, str):
                raise Refused(f"{name}: {value!r} is not text to join")
            pieces.append(value)
        return "".join(pieces)
    if "prefix" in expression:
        value = evaluate(expression["prefix"], context)
        if not isinstance(value, str):
            raise Refused(f"{name}: {value!r} is not text")
        return value[:expression["length"]]
    if "target_values" in expression:
        row = context.resolver.target_row(expression["target_values"], context.name)
        document = context.need_document("target_values")
        parts = [part for part in row.get("parts", []) if part.get("file") == context.pattern]
        if not parts or any("value" not in part for part in parts):
            raise Refused(f"{name}: {row['id']} has no parts with one value each "
                          f"read from {context.pattern}")
        return [render_results.evaluate(part["value"], document) for part in parts]
    if not any(form in expression for form in (*DOCUMENT_FORMS, *SHARED_FORMS)):
        raise Refused(f"{name}: unknown expression {sorted(expression)}")
    document = context.need_document(next(iter(expression)))
    if "codec_columns" in expression:
        spec = expression["codec_columns"]
        names = [column["name"] for column in walk(document, ["columns"])
                 if any(codec.get("family") == spec["with_family"] for codec in column["codecs"])]
        missing = [item for item in spec.get("except", []) if item not in names]
        if missing:
            raise Refused(f"{name}: {missing} are no {spec['with_family']} "
                          "columns, so nothing is excepted")
        return [item for item in names if item not in spec.get("except", [])]
    if "codec_column" in expression:
        return walk(render_results.codec_column(document, expression["codec_column"]),
                    expression["field"])
    if "least" in expression:
        spec = expression["least"]
        codecs = exact_codecs(render_results.codec_column(document, spec["column"]), spec)
        if not codecs:
            raise Refused(f"{name}: no exact codec of that family on {spec['column']!r}")
        return min(walk(codec, [spec["field"]]) for codec in codecs)
    if "codec" in expression and isinstance(expression.get("field"), list):
        codecs = [codec for codec in render_results.codec_column(
            document, expression["column"])["codecs"] if codec.get("codec") == expression["codec"]]
        if len(codecs) != 1:
            raise Refused(f"{name}: no one codec {expression['codec']!r} on "
                          f"{expression['column']!r}")
        return walk(codecs[0], expression["field"])
    if "items" in expression:
        found = walk(document, expression["items"])
        if not isinstance(found, list) or not found:
            raise Refused(f"{name}: {expression['items']} is no list of items")
        return [walk(item, expression["field"]) for item in found]
    if "count" in expression:
        found = walk(document, expression["count"])
        if not isinstance(found, list):
            raise Refused(f"{name}: {expression['count']} is no list to count")
        where = expression.get("where")
        if where:
            found = [item for item in found if walk(item, where["field"]) == where["equals"]]
        return len(found)
    if "where" in expression:
        found = [item for item in walk(document, expression["where"])
                 if all(item.get(key) == value for key, value in expression["match"].items())]
        if not found:
            raise Refused(f"{name}: no item of {expression['where']} matches "
                          f"{expression['match']}")
        return [walk(item, expression["field"]) for item in found]
    if "count_cases" in expression:
        cases = render_results.bench_cases(document, expression["count_cases"])
        if "at_least" in expression:
            floor = evaluate(expression["at_least"], context)
            cases = [case for case in cases if walk(case, expression["field"]) >= floor]
        return len(cases)
    if "flagged" in expression:
        return flagged(expression["flagged"], context)
    if "flagged_queries" in expression:
        variant = walk(document, expression["flagged_queries"])
        return [f"{query} {kind}" for query in render_results.QUERY_NAMES
                for kind in ("warm", "cold")
                if (variant["queries"][query][kind].get("swap_timed") or {}).get("flagged")]
    if "path" in expression and any(isinstance(key, (dict, int)) and not isinstance(key, bool)
                                    for key in expression["path"]):
        return walk(document, expression["path"])
    return render_results.evaluate(expression, document)


# --- Formats ----------------------------------------------------------------------------------

def number_text(value, entry, name):
    """A number as its entry's number format writes it, scale already applied."""
    form = entry.get("format")
    if not isinstance(form, str) or not NUMBER_FORMAT.fullmatch(form):
        raise Refused(f"{show(name)}: {form!r} is no format: a number's is {{:,}}, {{:d}} or "
                      "{:,.2f} and the like, or text, list or system")
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise Refused(f"{show(name)}: {value!r} is not a number to write as {form}")
    if NUMBER_FORMAT.fullmatch(form).group(2) is None:
        if isinstance(value, float) and value.is_integer():
            value = int(value)
        if not isinstance(value, int):
            raise Refused(f"{show(name)}: {value!r} is not a whole number, and {form} writes one")
        return form.format(value)
    if not math.isfinite(value):
        raise Refused(f"{show(name)}: {value!r} is not a finite number")
    return form.format(value)


def formatted(value, entry, name):
    """The value as its format writes it. A value that does not fit the format is refused: a
    count that is not a whole number, a figure that is not a finite number, text that is empty
    or runs over a line."""
    what = show(name)
    if "words" in entry:
        if not isinstance(value, bool):
            raise Refused(f"{what}: words for {value!r}, which is not true or false")
        key = "true" if value else "false"
        if key not in entry["words"]:
            raise Refused(f"{what} is {key} on these results, and the template's sentence holds "
                          f"only if it is {'false' if value else 'true'}: change the sentence "
                          "and give the words")
        return entry["words"][key]
    form = entry.get("format")
    if form == "text":
        if not isinstance(value, str) or not value.strip() or "\n" in value or "{{" in value:
            raise Refused(f"{what}: {value!r} is not one line of text")
        return value
    if form == "list":
        if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
            raise Refused(f"{what}: {value!r} is not a list of text")
        if not value and not entry.get("empty"):
            raise Refused(f"{what}: an empty list, and no words for one")
        return ", ".join(value) if value else entry["empty"]
    if form == "system":
        if value not in render_results.SYSTEM_LABEL:
            raise Refused(f"{what}: {value!r} is no system")
        return render_results.SYSTEM_LABEL[value]
    scale = entry.get("scale", 1)
    if NUMBER_FORMAT.fullmatch(form or "") and NUMBER_FORMAT.fullmatch(form).group(2) is None \
            and "scale" in entry:
        raise Refused(f"{what}: a whole-number format {form} takes no scale")
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        value = value * scale if "scale" in entry else value
    return number_text(value, entry, name)


def own_words(entry):
    """Every text an entry gives that the page shows: its words, its empty text, its notes."""
    found = []
    for key in ("words",):
        if isinstance(entry.get(key), dict):
            found += [text for text in entry[key].values() if isinstance(text, str)]
    if isinstance(entry.get("empty"), str):
        found.append(entry["empty"])
    return found


# --- Verdicts ---------------------------------------------------------------------------------

def verdict_class_of(parts, spec, name):
    """render_results.py's verdict class on the parts a spec names, or on every part."""
    if parts is not None and spec.get("parts"):
        labels = [part[0] for part in parts]
        unknown = [label for label in spec["parts"] if label not in labels]
        if unknown:
            raise Refused(f"{show(name)}: no part {unknown} in {labels}")
        parts = [part for part in parts if part[0] in spec["parts"]]
    try:
        return render_results.verdict_class(parts)
    except render_results.TargetError as error:
        raise Refused(f"{show(name)}: {error}") from error


def verdict_parts(resolver, name, entry):
    """A verdict token's row and the parts it takes, judged by render_results.py, or None and
    the reason the row has no figure."""
    spec = entry["verdict"]
    row = resolver.target_row(spec, name)
    parts, reason = resolver.judged(row, name)
    if parts is not None and spec.get("parts"):
        verdict_class_of(parts, spec, name)
        parts = [part for part in parts if part[0] in spec["parts"]]
    return row, parts, reason


def marks_of(row, parts, name):
    """Each mark RESULTS.md sets beside the verdict on these parts, as the forms a quote of it may
    give, by render_results.py's own functions and its rule for a hand-written quote (row_marks,
    quote_problem) on the token's parts: each part the replaced run judges otherwise, in full or
    short, with both outcomes, which way the change leans and the outcome counted; a part met
    among headline setups that another setup beats, with that setup; each disturbed phase
    and each swapped-in figure, in full or in the short form the README's first screen uses and
    RESULTS.md explains; and, for a write against other systems whose inputs did not start alike,
    the input clause, whose input was in memory before the clock and whose was read from disk
    inside it, with the bytes read. Each is a tuple of forms, the shortest last."""
    if parts is None:
        return []
    try:
        forms = [tuple(pair) for pair in render_results.change_mark_pairs(parts)]
        forms += [tuple(pair) for pair in render_results.phase_mark_pairs(parts, row)]
        classes = [render_results.notes_of(part)["write"] for part in parts
                   if render_results.notes_of(part).get("write")]
        clause = render_results.input_clause(parts[0][4], classes[0]) if classes else None
    except (render_results.TargetError, KeyError, TypeError) as error:
        raise Refused(f"{show(name)}: {error}") from error
    return forms + ([(clause,)] if clause else [])


def verdict_value(resolver, name, entry, marks=True):
    """A target's verdict, the Targets block's own: render_results.py judges the row on its
    files, its verdict code names the parts the token takes, and its marks stand beside it, each
    in its shortest form, once. A verdict of several parts given as text is short_verdict's
    sentence, each changed part's mark within it (review R-B3); a setup mark follows a met word
    without a semicolon, as it says what the word was judged against, and on any other verdict
    names the parts met that it stands beside."""
    row, parts, reason = verdict_parts(resolver, name, entry)
    if entry.get("words", "class") == "text":
        word = reason if parts is None else render_results.short_verdict(parts) \
            if len(parts) > 1 else render_results.core_verdict(parts)
    else:
        word = verdict_class_of(parts, {}, name)
    if not marks:
        return word
    shown = [mark for mark in dict.fromkeys(forms[-1] for forms in marks_of(row, parts, name))
             if mark not in word]
    # On a verdict not met in whole, the setup mark names the parts it stands beside, and the
    # parts met that no other setup beats follow it, so every part met is named and each mark
    # has its part (review P1-27).
    beaten = [part[0] for part in parts or [] if render_results.notes_of(part).get("faster")]
    unbeaten = [part[0] for part in parts or [] if part[0] not in beaten
                and render_results.counted(part) is True]
    setup = " " if word == "met" or not beaten else \
        f"; met on {render_results.and_list(beaten)} "
    after = f", and on {render_results.and_list(unbeaten)}" \
        if word != "met" and beaten and unbeaten else ""
    return word + "".join(f"{setup}{mark}{after}" if mark.startswith(render_results.SHORT_SETUP)
                          else f"; {mark}" for mark in shown)


def check_marks(resolver, values):
    """Every verdict on the page carries each mark RESULTS.md gives it, in one of the forms
    marks_of allows: a changed outcome and the input clause word for word, a phase's mark in
    full or in its short form, and no other."""
    for name, value in values.items():
        entry = resolver.entries.get(name, {})
        if "verdict" not in entry:
            continue
        row, parts, _ = verdict_parts(resolver, name, entry)
        for forms in marks_of(row, parts, name):
            if not any(form in value for form in forms):
                raise Refused(f"{show(name)} drops a mark RESULTS.md sets beside its verdict: "
                              f"{forms[0][:100]!r}")


def moves_and_keeps(resolver, text):
    """render_results.py's rules for every results page, on the essay: a sentence stating a move
    between the replaced run and this one says its cause was not measured and is not known and
    joins it to no cause, and a sentence on what the replaced record keeps is the one its fields
    give."""
    page = [(PAGE_PATH.as_posix(), text)]
    problem = render_results.move_problem(page) or \
        render_results.keeps_problem(page, resolver.root)
    if problem:
        raise Refused(problem)


def prose(text):
    """The text as render_results.py reads a page for the faster-write rule: each paragraph, list
    item, table row and heading on a line of its own, a wrapped sentence joined whole."""
    return "\n".join(unit for _, unit in render_results.prose_units(text))


def faster_write(resolver, text):
    """render_results.py's rule for every page: no sentence says tick-store writes faster than
    a system whose write read its input from disk inside its clock."""
    problem = render_results.faster_write_problem(
        [text], render_results.disk_read_writers(resolver.root))
    if problem:
        raise Refused(problem)


# --- The page ---------------------------------------------------------------------------------

def referenced(expression):
    """Every token an expression names with {"figure": name}."""
    if isinstance(expression, dict):
        found = [expression["figure"]] if "figure" in expression else []
        return found + [name for value in expression.values() for name in referenced(value)]
    if isinstance(expression, list):
        return [name for value in expression for name in referenced(value)]
    return []


def section_of(body, start):
    """The text a census names: a section from its ## heading to the next, or a paragraph from
    its opening words."""
    if start.startswith("#"):
        found = re.search(rf"^{re.escape(start)}\s*$(.*?)(?=^## |\Z)", body, re.M | re.S)
        return found.group(1) if found else None
    for block in re.split(r"\n\s*\n", body):
        if block.strip().startswith(start):
            return block
    return None


class Rendered:
    def __init__(self, text, values, waiting, states, notes, total, words):
        self.text = text
        self.values = values
        self.waiting = waiting
        self.states = states
        self.notes = notes
        self.total = total
        self.words = words


def check_list(figures):
    """The figure list's own shape, before any token is read."""
    if figures.get("schema") != SCHEMA:
        raise Refused(f"{FIGURES_PATH.as_posix()} is not a {SCHEMA} figure list")
    for key, fixed in FIXED_PATHS.items():
        if key in figures:
            raise Refused(f"the figure list names its {key}, {figures[key]!r}: the {key} is "
                          f"{fixed.as_posix()}, fixed in render_writeup.py")


def check_entries(resolver, names, factors):
    entries = resolver.entries
    uses = {name for entry in entries.values() if isinstance(entry, dict)
            for name in referenced(entry.get("value"))}
    uses |= {name for claim in resolver.figures.get("claims") or []
             for name in referenced(claim.get("holds"))}
    for name, entry in entries.items():
        if not isinstance(entry, dict) or not entry.get("what"):
            raise Refused(f"{show(name)}: its entry says nothing of what it is")
        helper = entry.get("helper") is True
        if helper and name not in uses:
            raise Refused(f"the figure list's helper {name!r} is used by no entry")
        if not helper and name not in names:
            raise Refused(f"the figure list's {name!r} is used nowhere in the template, so a "
                          "figure could be read and never shown")
        if helper and name in names:
            raise Refused(f"the figure list's helper {name!r} is in the template: give it a "
                          "format and drop helper")
        if "scale" in entry and entry["scale"] not in factors:
            raise Refused(f"{show(name)}: its scale {entry['scale']!r} is no declared factor")
        if "value" in entry and not grounding(entry["value"], name, factors, entry.get("kind")):
            raise Refused(f"{show(name)}: its value reads no results file, bar, constant or "
                          "count of the tree, so it is a number typed in the list")
        for text in own_words(entry):
            if TYPED.search(text) or NUMBER_WORDS.search(text):
                raise Refused(f"{show(name)}: its words {text!r} type a number")
        kind = entry.get("kind")
        if kind is not None and kind not in resolver.kinds:
            raise Refused(f"{show(name)}: no kind {kind!r} in the figure list")
    for kind, spec in resolver.kinds.items():
        if not isinstance(spec, dict) or not spec.get("what"):
            raise Refused(f"kind {kind!r} says nothing of what its files are")
        for pattern in (spec.get("files") or {}).values():
            check_pattern(kind, pattern)
        for key in ("note", ):
            if key in spec and (not isinstance(spec[key], str) or TYPED.search(spec[key])):
                raise Refused(f"kind {kind!r}: its {key} is no text free of numbers")


def paragraph_rules(resolver, blocks):
    """Each block that shows a figure of a kind with a pin or a note says so through the kind's
    pin or note token; and each table row holds one target's tokens."""
    for block in blocks:
        names = TOKEN.findall(block)
        kinds = set()
        for name in names:
            for _, kind, _ in resolver.deps.get(name, set()):
                kinds.add(kind)
        for kind in sorted(kinds):
            spec = resolver.kinds.get(kind, {})
            for key, token in (("pin", f"pin:{kind}"), ("note", f"note:{kind}")):
                if spec.get(key) and token not in names:
                    raise Refused(f"a block shows figures of the kind {kind!r}, and does not hold "
                                  f"{show(token)}: {block[:80]!r}")
        if block.lstrip().startswith("|"):
            ids = set()
            for name in names:
                entry = resolver.entries.get(name, {})
                for key in ("bar", "target", "verdict"):
                    if key in entry:
                        ids.add(entry[key].get("id"))
            if len(ids) > 1:
                raise Refused(f"a table row holds the tokens of {sorted(ids)}, not of one "
                              f"target: {block[:80]!r}")


def commit_of(resolver, path, kind):
    spec = resolver.kinds[kind]
    exception = spec.get("exception") or {}
    if exception.get("no_commit"):
        return None
    where = spec.get("commit", ["header", "commit"])
    try:
        commit = walk(resolver.document_of(path), where)
    except render_results.TargetError as error:
        raise Refused(f"{path.name} names no commit at {where}: {error}") from error
    if not isinstance(commit, str) or not re.fullmatch(r"[0-9a-f]{12,40}", commit):
        raise Refused(f"{path.name}: {commit!r} at {where} is no commit")
    # A file names its commit in full or as its first twelve characters, as its name does.
    return commit[:COMMIT_SHOWN]


def git_tree(root):
    done = subprocess.run(["git", "-C", str(root), "rev-parse", "--show-toplevel"],
                          capture_output=True, text=True, check=False)
    return done.returncode == 0 and Path(done.stdout.strip()).resolve() == Path(root).resolve()


def git_out(root, *arguments):
    """git's output, or a Waiting naming what git could not tell."""
    done = subprocess.run(["git", "-C", str(root), *arguments], capture_output=True, text=True,
                          check=False)
    if done.returncode != 0:
        raise Waiting("history", "git", f"git {arguments[0]} cannot tell: "
                      f"{done.stderr.strip()[:80]}")
    return done.stdout


def pins_at(root, commit):
    """scripts/data_pins.json as of a commit, or {} where it has none."""
    if not git_out(root, "ls-tree", "--name-only", commit, "--", PINS_PATH.as_posix()).strip():
        return {}
    try:
        return json.loads(git_out(root, "show", f"{commit}:{PINS_PATH.as_posix()}"))
    except json.JSONDecodeError as error:
        raise Refused(f"{PINS_PATH.as_posix()} at {commit[:12]} is not JSON: {error}") from None


def measured_changes(root, first, second, names):
    """What the files read at the measured commit `first` measure, by check_ties.py's own kinds,
    that differs between it and `second`: each path, and each input pin by member. A Waiting if
    this tree has no history to tell; a refusal for a file check_ties.py gives no kind."""
    if not git_tree(root):
        raise Waiting("history", "git", "this tree has no git history to tell what the two "
                      "commits differ in")
    kinds = []
    for name in names:
        kind = check_ties.kind_of(Path(name).relative_to("results").as_posix())
        if kind is None:
            raise Refused(f"{name}: check_ties.py has no kind for it, so nothing says what its "
                          "figures measure")
        kinds.append(kind)
    changed = set()
    for specs in dict.fromkeys(check_ties.pathspecs(kind.measured, kind.excluded)
                               for kind in kinds):
        if specs:
            changed.update(git_out(root, "diff", "--name-only", first, second, "--",
                                   *specs).split())
    members = sorted({member for kind in kinds for member in kind.pin_members})
    if members:
        before, after = pins_at(root, first), pins_at(root, second)
        changed.update(f"{PINS_PATH.as_posix()} ({member})" for member in members
                       if before.get(member) != after.get(member))
    return sorted(changed)


def commits(resolver):
    """The measured commit every results file read names, and the exceptions' phrase."""
    measured, exceptions = {}, {}
    for path, (kind, _) in sorted(resolver.read.items()):
        commit = commit_of(resolver, path, kind)
        if resolver.kinds[kind].get("exception"):
            exceptions.setdefault(kind, set()).add(commit)
        else:
            measured.setdefault(commit, []).append(path.relative_to(resolver.root).as_posix())
    if len(measured) > 1:
        raise Refused("the results files read name " + "; ".join(
            f"{commit[:COMMIT_SHOWN]} ({', '.join(files[:3])})"
            for commit, files in sorted(measured.items())) +
            ", and the essay says every figure was measured at one commit: measure them at "
            "one, or declare the kind an exception with its reason")
    if None in measured:
        raise Refused("a results file the essay reads names no commit")
    commit = next(iter(measured), None)
    phrases, at = [], {}
    for kind in sorted(exceptions, key=lambda item: resolver.kinds[item]["exception"]["says"]):
        spec = resolver.kinds[kind]["exception"]
        if not spec.get("why") or not spec.get("says"):
            raise Refused(f"kind {kind!r}: an exception says what it is and why")
        found = exceptions[kind]
        if len(found) != 1:
            raise Refused(f"kind {kind!r}: its files name {len(found)} commits")
        own = next(iter(found))
        if own is None:
            phrases.append(spec["says"])
            continue
        if own == commit:
            continue
        if "differs_only_in" in spec:
            raise Refused(f"kind {kind!r}: differs_only_in is no rule: a kind measured at "
                          "another commit is held to what the figures read at the measured "
                          "commit measure, by check_ties.py's kinds, and to nothing else")
        if commit is None:
            raise Refused(f"kind {kind!r}: no measured commit to compare its own with")
        changed = measured_changes(resolver.root, commit, own, measured[commit])
        if changed:
            raise Refused(f"kind {kind!r}: its files name {own[:COMMIT_SHOWN]}, and what the "
                          f"figures read at {commit[:COMMIT_SHOWN]} measure differs there: "
                          f"{changed[:5]}{' and more' if len(changed) > 5 else ''}; measure "
                          "them at one commit")
        at.setdefault(own, []).append(spec["says"])
    # The kinds at one other commit are named together, so the commit and what holds there are
    # said once: "the fuzz result and its record, at `8616263b7561`, where ...".
    phrases = [f"{and_words(says)}, at `{own[:COMMIT_SHOWN]}`, where the code every other figure "
               "measures is unchanged" for own, says in at.items()] + phrases
    if not phrases:
        return commit, ""
    joined = phrases[0] if len(phrases) == 1 else \
        "; ".join(phrases[:-1]) + "; and " + phrases[-1]
    return commit, f", except {joined}"


def and_words(items):
    return items[0] if len(items) == 1 else ", ".join(items[:-1]) + " and " + items[-1]


# The forms whose value ranks what it reads, so a token computed by one may carry the ranking word
# that describes it, as "the best codec" for a figure that is the best codec's.
RANKING_FORMS = ("best", "least", "min", "max", "lowest_value", "lowest_system", "min_variant",
                 "lowest_of")
# The words a bar's comparator stands for.
COMPARATOR_WORDS = {"at_most": ("most",), "at_least": ("least",), "above": ("above",),
                    "below": ("below",)}


def has_ranking_form(expression):
    if isinstance(expression, dict):
        return any(key in RANKING_FORMS for key in expression) or \
            any(has_ranking_form(value) for value in expression.values())
    if isinstance(expression, list):
        return any(has_ranking_form(value) for value in expression)
    return False


def carried_words(resolver, name):
    """The judging words a token carries, each lower-cased: a verdict, those of its row's target
    and of the parts it shows; a bar, its comparator's; a figure that ranks, the words its entry
    names under carries, refused where its value ranks nothing."""
    entry = resolver.entries.get(name) or {}
    words = set()
    if "verdict" in entry:
        spec = entry["verdict"]
        row = resolver.target_row(spec, name)
        labels = [item.get("label", "") for item in
                  ([row["lowest_of"]] if "lowest_of" in row else row.get("parts", []))
                  if not spec.get("parts") or item.get("label") in spec["parts"]]
        texts = [row.get("target_text", ""), *labels]
        if "lowest_of" in row:
            texts += [part for part in spec.get("parts", [])]
        words |= {found.lower() for text in texts for found in JUDGING.findall(text)}
    if "bar" in entry:
        row = resolver.target_row(entry["bar"], name)
        words |= set(COMPARATOR_WORDS.get(bar_comparator(row, name), ()))
    if entry.get("carries"):
        expression = entry.get("value")
        if "target" in entry:
            row = resolver.target_row(entry["target"], name)
            expression = [item.get("value") for item in row.get("parts", [])
                          if item.get("label") == entry["target"].get("part")]
        if not has_ranking_form(expression):
            raise Refused(f"{show(name)} carries {entry['carries']}, and its value ranks nothing: "
                          "a word that ranks needs a figure that does, or a claim")
        words |= {word.lower() for word in entry["carries"]}
    return words


def table_headers(body):
    """Each table's header row, with the tokens of its columns below it, column by column."""
    found = {}
    for chunk in re.split(r"\n\s*\n", body):
        lines = [line for line in chunk.strip().splitlines() if line.strip()]
        if len(lines) < 2 or not lines[0].lstrip().startswith("|") or \
                not TABLE_RULE.fullmatch(lines[1]):
            continue
        cells = lambda line: [cell for cell in line.strip().strip("|").split("|")]
        columns = [set() for _ in cells(lines[0])]
        for line in lines[2:]:
            for index, cell in enumerate(cells(line)[:len(columns)]):
                columns[index] |= set(TOKEN.findall(cell))
        found[lines[0]] = (cells(lines[0]), columns)
    return found


def masked_sentence(resolver, sentence, covered, headers):
    """A sentence with its claims' words, its tokens' carried words, code and link targets taken
    out: what is left must not rank or judge."""
    masked = sentence
    for text in covered:
        masked = masked.replace(text, " ")
    words = set()
    for name in TOKEN.findall(sentence):
        words |= carried_words(resolver, name)
    header = headers.get(sentence.strip())
    if header:
        pieces = []
        for cell, tokens in zip(*header):
            carried = set()
            for name in tokens:
                carried |= carried_words(resolver, name)
            pieces.append(re.sub(r"\b\w+\b", lambda found: " " if found.group(0).lower() in carried
                                 else found.group(0), cell))
        masked = "|".join(pieces)
    masked = re.sub(r"\b[\w-]+\b", lambda found: " " if found.group(0).lower() in words
                    else found.group(0), masked)
    masked = TOKEN.sub(" ", masked)
    return re.sub(r"`[^`]*`|\]\([^)]*\)", " ", masked)


def pitch_spec(resolver, claim, name):
    """The README pitch's one claim whose sentence holds the words claim["pitch_claim"]: the
    pitch ties whole sentences to the results, and the essay reuses a sentence's conditions."""
    definitions = resolver.targets_file()
    found = [item for item in (definitions.get("pitch") or {}).get("claims", [])
             if claim["pitch_claim"] in item.get("text", "")]
    if len(found) != 1:
        raise Refused(f"{name}: {len(found)} claims of the pitch of {TARGETS_PATH.as_posix()} "
                      f"hold the words {claim['pitch_claim']!r}, not one")
    return definitions, found[0]


def claim_value(resolver, claim, name):
    """Whether a claim holds: its condition, or every condition of the README pitch's claim
    holding its words, judged by render_results.py on its rows and files."""
    if "pitch_claim" in claim:
        definitions, spec = pitch_spec(resolver, claim, name)
        try:
            conditions = render_results.conditions_of(spec)
        except render_results.TargetError as error:
            raise Refused(f"{name}: {error}") from error
        for condition in conditions:
            if "target" in condition:
                row = resolver.target_row(condition["target"], name)
                resolver.judged(row, name)
            else:
                kind, host = resolver.kind_of_pattern(condition["file"], name)
                resolver.file_of(kind, host, name)
            try:
                problem = render_results.claim_problem(condition, definitions, resolver.root)
            except (render_results.TargetError, KeyError, TypeError) as error:
                raise Refused(f"{name}: {error}") from error
            if problem is not None:
                return problem
        return True
    holds = claim.get("holds")
    entry = {"what": name, "kind": claim.get("kind"), "host": claim.get("host", "any")}
    context = resolver.context(name, entry) if claim.get("kind") else \
        Context(resolver, name, entry)
    try:
        return evaluate(holds, context)
    except (render_results.TargetError, render_results.NoFigure, KeyError, TypeError,
            ValueError, ZeroDivisionError, IndexError) as error:
        raise Refused(f"{name}: {error}") from error


def claims_hold(resolver, body, blocks, figures):
    """Every sentence that ranks, compares or presumes a verdict is covered by a claim that holds
    or a token that carries its word. A claim names its words, which must sit in one sentence,
    and a condition that reads the figures the sentence prints, or a row's verdict, or the README
    pitch's claim of the same kind. Returns the claims that wait on a file still to come."""
    claims = figures.get("claims") or []
    covered, waits = [], []
    flat = " ".join(body.split())
    for claim in claims:
        text = claim.get("text")
        if not text or flat.count(text) != 1:
            raise Refused(f"claims: {text!r} is not in the template exactly once")
        sentence = next((sentence for block in blocks for sentence in sentences_of(block)
                         if text in " ".join(sentence.split())), None)
        if sentence is None:
            raise Refused(f"claims: {text!r} does not sit inside one sentence")
        printed = set(TOKEN.findall(sentence))
        name = f"claim {text[:40]!r}"
        holds = claim.get("holds")
        if ("pitch_claim" in claim) == ("holds" in claim):
            raise Refused(f"{name}: a claim gives holds or pitch_claim, one of them")
        if "holds" in claim:
            if not grounding(holds, name, resolver.factors, claim.get("kind")):
                raise Refused(f"{name}: its condition reads no file")
            used = set(referenced(holds))
            foreign = sorted(used - printed)
            if foreign:
                raise Refused(f"{name}: its condition reads {foreign}, which its sentence does "
                              "not print")
            if printed and not used & printed and "verdict_is" not in json.dumps(holds):
                raise Refused(f"{name}: its sentence prints {sorted(printed)}, and its condition "
                              "reads none of them")
        rests = json.dumps(claim)
        if "pitch_claim" in claim:
            rests += json.dumps(pitch_spec(resolver, claim, name)[1])
        for name_used in set(referenced(holds)):
            rests += json.dumps(resolver.entries.get(name_used, {}))
        for row_id, host in sorted(set(re.findall(r'"id": "([^"]+)", "host": "([^"]+)"',
                                                  rests))):
            rows = [row for row in resolver.targets_file().get("rows", [])
                    if (row.get("id"), row.get("host")) == (row_id, host)]
            try:
                parts = [part for row in rows for part in resolver.judged(row, name)[0] or []]
            except Waiting:
                continue
            if any(render_results.notes_of(part).get("change") for part in parts):
                raise Refused(f"{name} rests on {row_id} on {host}, whose outcome changed with "
                              "the replaced run: no count or ranking rests on it")
        try:
            value = claim_value(resolver, claim, name)
        except Waiting as wait:
            waits.append(wait)
            covered.append(text)
            continue
        if value is not True:
            raise Refused(f"the template says {text!r}, and the results do not bear it out: "
                          f"its condition is {value!r}; change the sentence")
        covered.append(text)
    headers = table_headers(body)
    for block in blocks_of(scoped(figures.get("not_results", []), body)):
        if block.startswith("#"):
            continue
        for sentence in sentences_of(block):
            masked = masked_sentence(resolver, sentence, covered, headers)
            found = JUDGING.search(masked) or NUMBER_WORDS.search(masked)
            if found:
                raise Refused(f"the template ranks, compares, judges or counts with "
                              f"{found.group(0)!r} in {sentence[:120]!r}, and no claim of the "
                              "figure list or token ties those words to the results")
    return waits


def census_holds(resolver, body, figures):
    """Each census names a block, a section or a paragraph, and the verdict classes it holds:
    its whole-row verdict tokens must be exactly the target rows of those classes. A row whose
    files are still to come is left to wait. Returns the waits."""
    waits = []
    for census in figures.get("census") or []:
        block = section_of(body, census.get("block", ""))
        if block is None:
            raise Refused(f"census: the template has no {census.get('block')!r}")
        classes = census.get("classes") or []
        shown = {}
        for name in TOKEN.findall(block):
            entry = resolver.entries.get(name, {})
            if "verdict" in entry and not entry["verdict"].get("parts"):
                key = (entry["verdict"]["id"], entry["verdict"]["host"])
                try:
                    shown[key] = verdict_value(resolver, name, dict(entry, words="class"),
                                               marks=False)
                except Waiting as wait:
                    waits.append(wait)
                    shown[key] = None
        wanted = set()
        for row in resolver.targets_file().get("rows", []):
            try:
                parts, _ = resolver.judged(row, f"census {census['block']!r}")
            except Waiting as wait:
                waits.append(wait)
                continue
            if render_results.verdict_class(parts) in classes:
                wanted.add((row["id"], row["host"]))
        wrong = sorted(key for key, value in shown.items() if value is not None
                       and value not in classes)
        if wrong:
            raise Refused(f"census {census['block']!r} holds {wrong}, whose verdict is not "
                          f"{classes}")
        missing = sorted(wanted - set(shown))
        if missing:
            raise Refused(f"census {census['block']!r} leaves out {missing}, which are "
                          f"{classes}: each has a row with its verdict token")
    return waits


def render(root):
    """The page from the template and the figure list, as far as the results resolve it."""
    root = Path(root)
    figures = read_json(root / FIGURES_PATH, FIGURES_PATH.as_posix())
    check_list(figures)
    try:
        template = (root / TEMPLATE_PATH).read_text(encoding="utf-8")
    except OSError as error:
        raise Refused(f"cannot read the template: {error}") from error
    body, comments = body_of(template)
    names = tokens_of(body)
    problem = opening_problem(body)
    if problem:
        raise Refused(problem)
    resolver = Resolver(root, figures)
    unknown = [name for name in dict.fromkeys(names) if name not in resolver.entries]
    if unknown:
        raise Refused(f"the template's {show(unknown[0])} is not in the figure list")
    check_entries(resolver, names, resolver.factors)
    typed = typed_numbers(body, figures.get("not_results", []))
    if typed:
        raise Refused(f"the template types the number {typed[0]!r} by hand: give it as a token "
                      "from the figure list, or, if it is no result, list it under not_results "
                      "with the reason and its place")
    faster_write(resolver, prose(TOKEN.sub(" ", body)))
    blocks = blocks_of(body)
    values, waiting, states = {}, {}, {}
    later = []
    for name in dict.fromkeys(names):
        entry = resolver.entries[name]
        if any(key in entry for key in ("measured_commit", "commit_exceptions", "pin_of",
                                        "note_of")):
            later.append(name)
            continue
        try:
            values[name] = resolver.shown(name)
            states[name] = "resolved"
        except Waiting as wait:
            waiting[name] = wait.label
            states[name] = f"waits on {wait.label}" + (f": {wait.why}" if wait.why else "")
    pending = census_holds(resolver, body, figures)
    pending += claims_hold(resolver, body, blocks, figures)
    for name in later:
        entry = resolver.entries[name]
        try:
            values[name] = special_value(resolver, name, entry, waiting)
            states[name] = "resolved"
        except Waiting as wait:
            waiting[name] = wait.label
            states[name] = f"waits on {wait.label}" + (f": {wait.why}" if wait.why else "")
    check_marks(resolver, values)
    paragraph_rules(resolver, blocks)
    for wait in pending:
        waiting[f"({wait.label})"] = wait.label
        states[f"({wait.label})"] = f"waits on {wait.label}" + (f": {wait.why}" if wait.why
                                                                 else "")
    for name, value in values.items():
        if isinstance(value, str) and resolver.entries[name].get("words") and \
                "verdict" not in resolver.entries[name] and \
                (TYPED.search(value) or NUMBER_WORDS.search(value)):
            raise Refused(f"{show(name)} prints {value!r}, words with a number in them")
    text = TOKEN.sub(lambda found: values.get(found.group(1), found.group(0)), body)
    text = text.rstrip("\n") + "\n\n" + MARKER + "\n"
    faster_write(resolver, prose(text))
    moves_and_keeps(resolver, text)
    notes = sum(1 for comment in comments if CUT_NOTE in comment)
    count = word_count(text)
    if not WORDS_AT_LEAST <= count <= WORDS_AT_MOST:
        raise Refused(f"the essay runs to {count:,} words"
                      + (", each token not yet resolved one" if waiting else "")
                      + f", outside its {WORDS_AT_LEAST:,} to {WORDS_AT_MOST:,}")
    return Rendered(text, values, waiting, states, notes, len(dict.fromkeys(names)), count)


def special_value(resolver, name, entry, waiting):
    """The tokens shown from the page's whole reading: the measured commit and its exceptions,
    and a kind's pin and note."""
    if "measured_commit" in entry or "commit_exceptions" in entry:
        commit, phrase = commits(resolver)
        if "measured_commit" in entry:
            if commit is None and waiting:
                raise Waiting("measured commit", "any", "no results file it names is read yet")
            if commit is None:
                raise Refused(f"{show(name)}: no results file read names a measured commit")
            return commit[:COMMIT_SHOWN]
        return phrase
    if "pin_of" in entry or "note_of" in entry:
        key = "pin_of" if "pin_of" in entry else "note_of"
        kind = entry[key]
        spec = resolver.kinds.get(kind) or {}
        if key == "pin_of":
            words = (spec.get("pin") or {}).get("says")
        else:
            words = spec.get("note")
        if not words:
            raise Refused(f"{show(name)}: kind {kind!r} has no {key[:-3]} to show")
        if name != f"{key[:-3]}:{kind}":
            raise Refused(f"{show(name)}: a kind's {key[:-3]} token is {key[:-3]}:{kind}")
        return words
    raise Refused(f"{show(name)}: no value")


def waiting_lines(rendered):
    tokens, checks = {}, {}
    for name, label in rendered.waiting.items():
        into = checks if name.startswith("(") else tokens
        into[label] = into.get(label, 0) + 1
    lines = ["not judged: {} of {} tokens have no results file yet".format(
                 sum(tokens.values()), rendered.total),
             "waiting, by kind of file: " + ", ".join(f"{label} {count}"
                                                       for label, count in sorted(tokens.items()))]
    if checks:
        lines.append("checks waiting on the same files: " + ", ".join(
            f"{label} {count}" for label, count in sorted(checks.items())))
    return tuple(lines)


def final_problem(rendered):
    if rendered.notes:
        return (f"the template still holds {rendered.notes} {CUT_NOTE} notes: keep each sentence "
                "or take its replacement, and drop the note, before the page is rendered")
    return None


def main(argv=None):
    parser = argparse.ArgumentParser(description="Render WRITEUP.md from its template and its "
                                                 "figure list, and check it.")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if WRITEUP.md differs from a fresh render")
    parser.add_argument("--list", action="store_true",
                        help="print each token, its state and its value, and exit")
    parser.add_argument("--root", type=Path, default=REPO_ROOT,
                        help="the tree the template, the list and the results are in")
    parser.add_argument("--draft", type=Path,
                        help="also write what resolves here, waiting tokens left as {{name}}; "
                             "never the page itself")
    args = parser.parse_args(argv)
    try:
        rendered = render(args.root)
    except Refused as error:
        print(f"render_writeup: {error}", file=sys.stderr)
        return EXIT_REFUSED
    page = args.root / PAGE_PATH
    page_name = PAGE_PATH.as_posix()
    if args.list:
        for name, state in rendered.states.items():
            print(f"{name}\t{state}\t{rendered.values.get(name, '')}")
    if args.draft:
        if args.draft.resolve() == page.resolve():
            print("render_writeup: --draft is never the page", file=sys.stderr)
            return EXIT_REFUSED
        args.draft.write_text(rendered.text, encoding="utf-8")
    if args.list:
        return EXIT_UNJUDGED if rendered.waiting else EXIT_OK
    if rendered.waiting and not page.exists():
        for line in waiting_lines(rendered):
            print(f"render_writeup: {line}")
        print(f"render_writeup: {page_name} does not exist yet; it is rendered once every token "
              "resolves")
        return EXIT_UNJUDGED
    if rendered.waiting:
        names = ", ".join(sorted(rendered.waiting)[:8])
        print(f"render_writeup: {page_name} exists, and {len(rendered.waiting)} of its tokens have "
              f"no results file: {names}", file=sys.stderr)
        print(f"render_writeup: {waiting_lines(rendered)[1]}", file=sys.stderr)
        return EXIT_STALE
    problem = final_problem(rendered)
    if problem:
        print(f"render_writeup: {problem}", file=sys.stderr)
        return EXIT_REFUSED
    if args.check:
        if not page.exists():
            print(f"render_writeup: every token resolves, and there is no {page_name}: render it",
                  file=sys.stderr)
            return EXIT_STALE
        if page.read_text(encoding="utf-8") != rendered.text:
            print(f"render_writeup: {page_name} differs from a fresh render: it is stale or was "
                  "edited by hand. Render it again.", file=sys.stderr)
            return EXIT_STALE
        print(f"render_writeup: {page_name} is current: {rendered.total} tokens, "
              f"{rendered.words:,} words")
        return EXIT_OK
    page.write_text(rendered.text, encoding="utf-8")
    print(f"render_writeup: wrote {page_name}: {rendered.total} tokens, {rendered.words:,} words")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
