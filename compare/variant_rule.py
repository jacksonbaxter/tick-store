"""Which variants a full-day run takes: the rule in compare/README.md, written before any run.

Every variant of every system on a whole day would take more than the measurement time the
host can give (the estimate is in compare/README.md), so a full-day run takes each system's
headline variant, and every other variant of that system that won a query on the Linux slice
at the same commit. A variant wins a query if, among its own system's variants, it has both the
lowest median and the lowest tail figure of that query, warm or cold: the test RESULTS.md's
"Lowest" column and QL-4 use. The tail figure is the nearest-rank 99th percentile, which at the
slice's 30 warm and 7 cold repetitions is the slowest run: the slowest of 30 warm, the slowest
of 7 cold, never a 1% tail. A file holds it as `p99_us`, its old name, and from protocol 2 also
as `tail_us`, with n and the rank used. The rule is the same for all four systems.

Standard library only, so that scripts/check_compare.py can apply it again to the slice file a
full-day file names, and refuse a file whose variants are not what the rule gives.
"""

QUERY_NAMES = ("Q1", "Q2", "Q3", "Q4", "Q5")
KINDS = ("warm", "cold")
# p99_us is the tail figure's old name, which every file has: the slowest run at these counts.
FIELDS = ("median_us", "p99_us")
RULE = ("Each system's headline variant, and every other variant of that system that won a "
        "query on the Linux slice at the same commit: among that system's variants, the lowest "
        "median and the lowest tail figure of one query, warm or cold. The tail figure is the "
        "nearest-rank 99th percentile, which at 30 warm and 7 cold repetitions is the slowest "
        "run: the slowest of 30 warm, the slowest of 7 cold. The same rule for all four "
        "systems.")
# The rule as the files measured before protocol 2 state it: the same test, with the slowest
# run called a p99.
RULE_PROTOCOL_1 = ("Each system's headline variant, and every other variant of that system that "
                   "won a query on the Linux slice at the same commit: among that system's "
                   "variants, the lowest median and the lowest p99 of one query, warm or cold. "
                   "The same rule for all four systems.")


def wins(system):
    """{variant: ["Q1 warm", ...]} for the measures on which each variant won, among its own
    system's variants. A tie on both numbers is a win for each variant tied."""
    variants = system["variants"]
    found = {}
    for query in QUERY_NAMES:
        for kind in KINDS:
            entries = {name: variant["queries"][query][kind] for name, variant in variants.items()}
            for name, entry in entries.items():
                if all(entry[field] == min(other[field] for other in entries.values())
                       for field in FIELDS):
                    found.setdefault(name, []).append(f"{query} {kind}")
    return found


def select(slice_document):
    """{system: {"headline", "won", "selected"}} from a slice results document. `selected`
    lists the headline first, then the winners in the order the slice ran them."""
    chosen = {}
    for name, system in slice_document["systems"].items():
        won = wins(system)
        headline = system["headline_variant"]
        selected = [headline] + [variant for variant in system["variants"]
                                 if variant != headline and variant in won]
        chosen[name] = {"headline": headline, "won": won, "selected": selected}
    return chosen


# The full day of tick-store alone. The four-system full day runs on Linux only, since its warm
# proof reads each process's own storage reads from /proc. On macOS, where BENCHMARKS.md's WT-1,
# WT-2 and QL-1 to QL-3 also ask for a figure, tick-store runs the same full day by itself, so
# there is no other system's result for a rule to rank: it runs every variant it has, the
# headline first. compare/systems/tickstore.py defines them, and tests/compare/test_tickstore.py
# fails if this list and that one part.
TICKSTORE_ONLY = "tickstore-only"
TICKSTORE_VARIANTS = ("v3-seg4Mi", "v3-seg1Mi")
TICKSTORE_ONLY_RULE = ("tick-store alone, every variant it has, the headline first. The "
                       "four-system full day runs on Linux only, so a host without it runs "
                       "tick-store by itself, and with no other system there is nothing for "
                       "the slice rule to rank.")


def tickstore_only():
    """The variant selection of a tick-store-only full day."""
    return {"rule": TICKSTORE_ONLY_RULE, "source": "none: no slice file chooses them",
            "systems": {"tickstore": {"headline": TICKSTORE_VARIANTS[0],
                                      "won": "not ranked: tick-store ran alone",
                                      "selected": list(TICKSTORE_VARIANTS)}}}
