"""The comparison's repetitions: compare/run_system.py runs them, and scripts/check_compare.py
requires exactly these of every reported file (review F-5), so a file with fewer cannot pass for
the protocol. Standard library only, since the validator imports it."""

# Timed runs of each query on warm pages, after its untimed warm-up.
WARM = 30
# Timed runs of each query, each a fresh process on an evicted store.
COLD = 7
# Writes of each class and variant on the slice.
WRITE = 3
# A full-day write is one sustained run of minutes, and three of each class for every variant
# would not fit the run: see "Full-day comparison" in compare/README.md.
FULL_DAY_WRITE = 1
