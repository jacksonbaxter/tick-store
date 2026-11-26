# How the results pages are checked

What three of `scripts/check.sh`'s steps hold the results pages, the essay and the results files
to, beyond the steps the README's "Checks" section lists, and what each does not check.

`render_results.py --check` also holds the first screen to its rules. The pitch, headings
included, runs to at most `max_words` words, 100; a line is a heading only as GitHub shows one,
so `#It` is prose. Each figure in it is the one the results give now, and any other number is a
text `scripts/targets.json` lists with its reason, found once. Such a number is compared with no
results file: the check holds it to its listing and its count alone. A figure read from a timed
phase in which the records list a disturbance, or flag a page swapped in, must stand with that
phase's mark in brackets beside it, as the first screen gives such a mark. Each heading and
sentence is one of three things: a claim, a whole sentence listed under `claims` that must hold on
the results, holds the words its target lists for its bar and names the one machine it is judged
on, so a new results file that turns a verdict stops the check at the sentence to change; a
sentence holding a figure; or a text listed as stating no result, with its reason. A claim that
pairs figures or names the fastest is held on the replaced run's file as well wherever one is
kept, and one marked `on_replaced` is refused if none is; a claim that names the replaced run's
file as its own is refused. No claim rests on a part met by the rule and not counted: a claim on
that part's target, or one comparing two figures that part compares, is refused, and so is one
ranking tick-store below or ahead of a system on that part's query and kind, whatever the
variants. A claim ranking two other systems on such a part is refused too, though it favours
neither side: the rule fails safe. A word that ranks or judges, such as fastest, least, below, less, ahead or keeps up,
must sit inside a claim or a figure's words, headings included. Each table row says what it is,
then its bar and the bar's source, then what was measured, from a template whose figures are
named values read from the results files: a template that types a figure, names a value it is not given, or is given one it does not name
stops the render, and a lowest of the other systems takes every other system the comparison
ran. The list under the table, under its own heading, opens with a lead-in giving the targets
shown and the count of other rows, then names every part of a target with a row on the screen that
the table leaves out, one item a verdict class, the met parts too, and counts every other
target-and-machine row by verdict, a row's class taken from its counted parts alone, so a part met by the rule and not
counted makes no row met or missed in part. It names parts that carry the same marks together,
each mark once, under their machine and setup, and opens the class of parts met and not counted
with a gloss that fits their reasons; there a not-counted mark stands in for the swap mark of the
same systems, and names beside it each side only the earlier run flags, so the screen names every
side either run flags, as RESULTS.md does. It is checked apart from the code that writes it, by verdict classes of its own: each
such part named whole, with each of its marks, under its machine and nothing else named, the
gloss the one its parts' reasons give, each count the targets' own, the lead-in the one the
screen's targets and count give,
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
spans and places in the documents are checked. RESULTS.md's benchmark tables mark each row that
holds such a case " (disturbed)" beside its case or kernel, the mark covering every figure in the
row, a kernel row's medians, range, spread and counters among them, as the block's legend says;
its comparison tables mark each timed figure they show from such a phase. So an entry is marked
on the verdicts and figures the generated pages show; an entry in a phase no page shows, such as
a slice setup no table gives, is in the record only. A figure typed by hand on
`compare/README.md` or `BENCHMARKS.md` is not held to the disturbance records: nothing checks
that one quoted from a phase with an entry says so. A part met by the rule whose swap or disturbance
mark falls on another system's side, and which the replaced run does not confirm, keeps its word
and its mark and is counted nowhere; its mark names every side either run flags as swapped in, in
RESULTS.md with each run's pages read from both records and on the first screen by system, a side
only this run flags by its swap mark and one only the earlier run flags as such, and a write side gives the pages both runs swapped in over the whole
write phase, which is wider than the clock, so the swapping is not placed inside the timed write. No results page, the essay included, may hold a
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
pages. A verdict missed on every part it counts while a part is met by the rule and not counted
reads "missed on each counted part", so the parts it then names as met are those not counted. A
figure read on one host must be
described as that host's. The essay must run 2,500 to 4,060 words as `LC_ALL=C wc -w` counts the
rendered page, its tables, marks and generated line included, and a page that macOS's `wc -w` in a
UTF-8 locale counts otherwise is refused, as that count ends a word at a dagger with no space after
it. It opens with one paragraph of three sentences, each holding a token. Two marks stand only
through their own tokens and are typed nowhere: a diagnostic kind's note mark and the flag's
dagger, each defined once, by its token, in the first block it stands in, the note mark only in a
block showing its kind's figures. A flag stands straight after a token whose figure rests on what
it flags: a query and kind or a class's write, of its system and, where both name one, its
variant, or a bench case; a figure whose expression fixes no system is not beside it. Every
figure from a phase the disturbance records place a disturbance in, or the record flags as timed
while pages were swapped in, carries in its sentence or table row a flag of that phase or the
verdict whose marks name it, through whatever its expression reads: each item and filter of a
ratio, product, sum, difference, minimum, maximum, median or other operator, a results file's
items, and every bench case a median or count over cases takes, a same held to its first item
and a minimum or maximum of figures with no filter to the one it takes. A form that reads the
code, the targets or a codec or size field names no timed phase, and a form the rule cannot read
is refused by name. A sentence ends at ., ! or ? before a letter of either case, a token, a bold
lead-in, a code span, a bracket or a quote mark, so one starting "tick-store" stands alone. A
verdict's swap mark carries a query figure of its system, setup, query and kind, and only a
swap of this run. A write verdict's input
clause may stand short, its bytes kept, only where it points to the method section and that section
says what engine-only may still hold; the section gives `render_results.py`'s input note whole,
through a token that gives the note alone. A not-counted token stands in the sentence or table row
of a figure of the query and kind, or write, of the part it names. A verdict judged on the replaced
run's figures names that run, at its own commit, in the commit sentence. Until every file it reads
is committed there is no `WRITEUP.md`, and the step is not judged, naming how many tokens and checks
wait and on what, which is a failure under `--no-skips`; once the page exists, a missing file or a
page that differs from a fresh render fails. What it does not check is that the prose is true
beyond its figures, claims and verdicts: a sentence that misleads with no judging word, a verdict
token set beside the wrong target, a line of the list that misdescribes what its entry reads, a
number after a letter, as in a version, and the content of the profiler and counter summaries in
`results/diag/`, which no separate validator re-reads, would all pass. Nor does it compare a flag's
file and host with its figure's; hold a flag to the side of a ratio it marks; hold a figure of a
fuzzing or gate run, a read-back or a capacity part to a flag, though their verdicts carry the
marks; tie a not-counted token of a query part to the part's system, only to its query and kind;
check the words an exception that names its own commit gives; hold a flag that prints no mark
to carry nothing by a test, which the page's own check does on today's data; or hold the sort's net, whose guard
that it is a cost lives in the list's `only_above` and in no check. "Straight after" is the token before the flag in
its cell or sentence, so words between them pass, and an abbreviation's stop before a word ends a
sentence too; the essay has none. Two right pages are refused: a flag in a table
column of its own, not straight after its figure, and the input note after a lead-in such as "On
inputs:", which joins it to other words.

`results/fuzz-gate-8616263b7561.json` holds the fuzzing's bound-seed gate: each named seed of
`fuzz/bound_seeds.txt` run alone through its fuzz target five times on each host, 310 runs, with
each run's exit, findings, executed units, time an execution, wall time and load, and on Linux each
processor's busy share, the processes started and whether the sibling core's busy process ran at its
start and its end. `scripts/gate_results.py` assembled it on 2026-11-25, after the runs, from each
host's raw records; it times nothing, and a record that disagrees with the runner's own table or the
gate's verdicts stops it. `scripts/check_gate.py` refuses a file whose shape or name is not the
assembler's, that holds a machine's path or name, or whose bar is not half of `fuzz/launch.txt`'s
timeout; a run field not of the form the assembler writes there: a count that is not a whole number,
a time an execution or a load that is not a finite number of at least 0, -0.0 included, a start or
end that is not a real UTC time or an end before its start, a busy share outside 0 to 100 or past
one decimal, and a target not its seed's own; a header whose words are not the assembler's, whose
list is not the tree's `fuzz/bound_seeds.txt` by its SHA-256, or whose day of assembly is before its
last run's; a file that misses a seed or a run, gives other than exactly five runs a seed or numbers
them otherwise; a time an execution that is not its own fields'; a run marked pass or fail contrary
to the gate's rule; or a summary its runs do not give, in value, in type and in the order of its
keys; and a disturbance entry in a gate run that names no run of the file, whose span is not two UTC
times, start first, or misses its run. `render_results.py` runs that check before it renders the
gate's block in `fuzz/README.md`, and `--check` fails when the block differs from a fresh render.
`check_ties.py` gives the file a kind of its own, `fuzz-gate`: the fuzz binaries and their inputs at
the file's commit, without the fuzzing runs' launcher, which did not run the gate, its assembler
compared from the commit that wrote the file. What they do not check: the raw records, which are not
in the repository, so the file is held to the assembler's reading of them and to its own sums, and a
repeated line in a host's step log, which only the assembler could see and which it takes the last
of (docs/TASKS.md keeps it open); a run's fields against each other, so a wall time shorter than its
libFuzzer time, a span far from its wall time, runs that overlap in time, an extra cpu's busy share
or a load of 1e308 would pass; a day of assembly after today, which only the clock could refuse,
making the check's result depend on the day it runs; the busy shares, the load and the processes
started, which the gate held to no rule and the file keeps unjudged; and the untracked runner that
ran the gate, which no kind ties.

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

`scripts/check.sh` before its first step, each script test that runs git, before it does,
`prove_checks.py` before it clones anything, and `check_ties.py`, `check_hygiene.py`,
`render_results.py` and `render_writeup.py`, which no results file is tied to, first thing in
`main`, drop the variables `git rev-parse --local-env-vars` prints, git's own list of a
repository's variables (`GIT_DIR`, `GIT_WORK_TREE`, `GIT_INDEX_FILE`, `GIT_OBJECT_DIRECTORY`,
`GIT_ALTERNATE_OBJECT_DIRECTORIES`, `GIT_COMMON_DIR`, `GIT_PREFIX` and the rest, as the git that
runs prints them), and `GIT_NAMESPACE`, which that list leaves out. Git exports them to hooks and
to `git rebase --exec`, and inherited they turned the tests' own `git init`, commits, resets and
config writes on the caller's repository, and a `git status`, or a `git diff` of a commit against
the work tree, refreshes the caller's index and writes it back ([R6-1](REVIEW.md#the-final-reviews-git-variables-r6) to [R6-3](REVIEW.md#the-final-reviews-git-variables-r6),
[R6-5](REVIEW.md#the-final-reviews-git-variables-r6)). `tests/scripts/test_git_env.py` sets them to a decoy repository of its own and
requires every file of the decoy, its work tree's and its `.git`'s, unchanged byte for byte after
one repository-making test of each script test that makes one, check.sh's hygiene step with a
stand-in that makes a repository, the prover's clone, and each of those four scripts run on its
own against a clone of this tree as the decoy, which must also give the output it gives with no
variables set; and it requires every script test that runs git to clear them at its top. What
this does not cover ([R6-4](REVIEW.md#the-final-reviews-git-variables-r6), [R6-5](REVIEW.md#the-final-reviews-git-variables-r6)): the measured code and the scripts tied to results
files are not edited, and run on their own, not through check.sh, under a caller whose variables
name another repository, they act on that one. `cmake/GenerateBuildInfo.cmake`, at configure
time, records that repository's HEAD as this build's commit and its `git status --porcelain` as
the build's dirty state, which every results file's build facts carry, and the `git status`
writes its index; `compare/run.py` runs `git status --porcelain`, writing its index and recording
its state as this tree's; `fuzz_g3.py` records its HEAD as the run's commit, and `check_fuzz.py`'s
`git diff` of a commit against the work tree, which `fuzz_g3.py` runs too, writes its index;
`run_bench.py` and `coverage_report.py` record its HEAD as the run's commit, and
`coverage_report.py` its file list; `fuzz_results.py` compares two commits in its history. A
script test that runs git only through a script in a child relies on that script's own clearing,
and the test's list of what runs git is a pattern of the calls the tests make today.
