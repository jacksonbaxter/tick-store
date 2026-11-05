"""Which variants a full-day run takes: the rule in compare/README.md, written before any run.

Every variant of every system on a whole day would take more than the measurement time the
host can give (the estimate is in compare/README.md), so a full-day run takes each system's
headline variant, and every other variant of that system that won a query on the Linux slice
at the same commit. A variant wins a query if, among its own system's variants, it has both the
lowest median and the lowest p99 of that query, warm or cold: the test the README's "Lowest"
column and QL-4 use. The rule is the same for all four systems.

Standard library only, so that scripts/check_compare.py can apply it again to the slice file a
full-day file names, and refuse a file whose variants are not what the rule gives.
"""

QUERY_NAMES = ("Q1", "Q2", "Q3", "Q4", "Q5")
KINDS = ("warm", "cold")
FIELDS = ("median_us", "p99_us")
RULE = ("Each system's headline variant, and every other variant of that system that won a "
        "query on the Linux slice at the same commit: among that system's variants, the lowest "
        "median and the lowest p99 of one query, warm or cold. The same rule for all four "
        "systems.")


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
