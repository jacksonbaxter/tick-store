# How the results pages are checked

What three of `scripts/check.sh`'s steps hold the results pages, the essay and the results files
to, beyond the steps the README's "Checks" section lists, and what each does not check.

`render_results.py --check` also holds the first screen to its rules. The pitch, headings
included, runs to at most `max_words` words, 100; a line is a heading only as GitHub shows one,
so `#It` is prose. Each figure in it is the one the results give now, and any other number is a
text `scripts/targets.json` lists with its reason, found once. Each heading and sentence is one
of three things: a claim, a whole sentence listed under `claims` that must hold on the results,
holds the words its target lists for its bar and names the one machine it is judged on, so a new
results file that turns a verdict stops the check at the sentence to change; a sentence holding a
figure; or a text listed as stating no result, with its reason. A word that ranks or judges, such
as fastest, least, below, less, ahead or keeps up, must sit inside a claim or a figure's words,
headings included. Each table row says what it is, then its bar and the bar's source, then what
was measured, from a template whose figures are named values read from the results files: a
template that types a figure, names a value it is not given, or is given one it does not name
stops the render, and a lowest of the other systems takes every other system the comparison
ran. The line under the table names every part of a target with a row on the screen that the
table leaves out and that is missed or not judged, and counts every other target-and-machine row
by verdict. It is checked apart from the code that writes it, by verdict classes of its own: each
such part named whole under its machine and nothing else named, each count the targets' own,
each target asked for named under its own count, and what is shown, named and counted adding up
to the whole table; the sentence under the table must give its row's verdict. A plain name is
one target's, and one part's, on a machine. The table must hold a miss. The whole screen names
no target id and no query's name, introduces each machine once, by what it is, holds no
character a reader cannot see and no figure in a character other than digits, and ends at the
first heading after the table. What it does not check is that the prose is true beyond those
figures and claims: a sentence with a figure, or one listed as stating no result, is read only
for ranking words, so it can mislead without one, as can a number written in words; a claim's
words beyond those its target lists, a plain name that misdescribes its target, or a bar whose
words drift from its target in `BENCHMARKS.md` would all pass.

`render_results.py --check` also holds a comparison verdict to the run it replaced. A comparison
file a later run replaced stays, bytes unchanged and pinned by its SHA-256 under
`replaced_comparison_files`, in `results/compare/replaced/`, and each comparison target is judged on
it as well; where it lacks a figure, the part is judged on the clock it keeps and the row says
so. Where the two outcomes differ, the row gives both runs' figures and outcomes and says beside
the verdict whether the change favours tick-store, and every count, claim and line of the first
screen takes the less favourable, which the checks work out from both runs apart from the code
that writes the verdict. A write verdict against other systems says, from each write's record,
whose input was in memory before the clock started and whose was read from disk inside it, the
bytes read from the device inside the clock, what engine-only holds, and that engine-only does
not take the reads out. A verdict that rests on a timed phase in which
`results/disturbances-<commit>.json` records something else running, such as a login or the
host's backup job, says so beside it, with the replaced run's figure there; the file's shape,
spans and places in the documents are checked. No results page, the essay included, may hold a
sentence saying tick-store writes faster than a system whose write read its input from disk inside
its clock: the rule reads the whole prose of `README.md`, `RESULTS.md`, `BENCHMARKS.md`,
`compare/README.md` and `fuzz/README.md`, a wrapped sentence whole, and names the page and line.
With no replaced file and no disturbance file, nothing is marked. What it does not check
is the disturbance list itself, made by hand from the host's logs: an event left out of it marks
nothing, and a faster write said in other words than the rule's passes. Nor does it read
`docs/NOTES.md`, `docs/TASKS.md`, `docs/REVIEW.md` or `research/`, the working record, which quotes
each run's verdicts as they stood.

`render_writeup.py` holds the essay to `docs/writeup/figures.json`, its figure list; the
template, the page and `scripts/targets.json` are fixed in the script, not chosen by the list. No
digit stands in `docs/writeup/WRITEUP.template.md` outside a token of the list, a host's name or a
text the list declares no result, with its reason and the places it may stand, and a declared text
holding a number with a unit is refused. A token names its results file by kind and host, by a
pattern under `results/` with one `*` where the commit goes, so a file measured again still
resolves and two files of a kind stop the render. Every value reaches a results file, a count of
the tree, a constant read from the code with its comments blanked, or a bar; a number written into
the list must be one of its declared factors, such as bytes in a GB. A token may not read what its
results file lists as not established, as a diagnostic summary lists what its run's raw output
did not show. A verdict is
`render_results.py`'s own; a bar is the target's value, shown exactly with its sign; a target's
figure is the row's own part, printed on the same side of its bar as its verdict; and a gap is
never printed as none. Every file a token reads names one measured commit, but for the exceptions
the list declares with their reasons, which the essay names through a generated token. A file
measured at another commit is taken only where every path that the files read at the measured
commit measure, by `check_ties.py`'s own kinds, and their input pins are the same at both
commits, as `git diff` shows; that is not judged in a tree with no history. A file under a pin of `scripts/data_pins.json` is read only
through a kind that declares the pin, and each paragraph showing its figures says so. A word that
compares, ranks or judges, or a number in words, must sit inside a claim of the list whose
condition reads the figures its sentence prints, or be carried by a token; the Missed targets and
Not judged sections hold exactly the rows of those verdicts; every verdict carries each mark RESULTS.md sets beside it, taken from render_results.py's own
functions and held as its rule for a hand-written quote holds the other pages: an outcome the
replaced run judges otherwise in full, a disturbed or swapped-in phase in full or in the short
form the README's first screen uses, and, for a write against other systems, the clause saying
whose input was in memory and whose was read from disk inside the clock, with the bytes read; a
verdict the replaced run judges otherwise rests no claim; and render_results.py's rules that no
sentence says tick-store writes faster than a system whose clock holds its disk reads, that a move
between the replaced run and this one is said with its cause not measured and not known, and that
what the replaced record keeps is said as its fields give it, hold on the essay as on the other
pages. A figure read on one host must be
described as that host's. The essay must run 2,500 to 4,000 words and open with one paragraph of
three sentences, each holding a token. Until every file it reads is committed there is no
`WRITEUP.md`, and the step is not judged, naming how many tokens and checks wait and on what, which
is a failure under `--no-skips`; once the page exists, a missing file or a page that differs from
a fresh render fails. What it does not check is that the prose is true beyond its figures, claims
and verdicts: a sentence that misleads with no judging word, a verdict token set beside the wrong
target, a line of the list that misdescribes what its entry reads, a number after a letter, as in
a version, and the content of the profiler and counter summaries in `results/diag/`, which no
separate validator re-reads, would all pass.

`check_ties.py` examines every file under `results/`, and fails one of no kind. It refuses a
commit not in HEAD's history, a symbolic link git tracks in a compared path, and any git command
that fails, naming it: a missing object, a `.git` git cannot read, a folder inside a checkout
given as the checkout, or a results folder outside it. A pin names its file by its exact path
under `results/`, from a set fixed in the script, and a line in the pins file for any other name
is refused. A results file not committed as it is is not judged against what assembled it, but
told to be committed first, a skip, which `--no-skips` fails. A committed result that another
file copies figures from ties that file through the fields its assembler reads, compared by
value, not through its bytes: the store-size files and the store estimate read the capacity
file's day and, on that day only, each compared system's stored bytes (with the variant and
outcome, for the store-size file). Each field is a JSON path written out in the script beside
the assembler's lines, and a test runs each assembler on a probe that notes every key it reads.
A moved field unties the file, a field nothing reads does not, and the second day's store-size
file is tied to the day alone. What it does not check: what
assembled a file is compared with the last commit that wrote the file, so any later commit of
the file, by hand or not, resets that comparison, and nothing shows the bytes are what the
assembler wrote; a new file this checkout's own excludes hide is not seen; and it compares the
work tree, which CI's clean checkout makes the commit.
