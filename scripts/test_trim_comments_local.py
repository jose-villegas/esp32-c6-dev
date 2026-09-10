#!/usr/bin/env python3
"""Regression test for the two mechanical checks response_problems() gained
after wave one/two of the comment-trim pipeline: directive leakage and
unresolvable cross-references (see trim_comments_local.py's PROMPT and
response_problems() for the checks themselves).

Each case is either an OBSERVED failure (real text from a run, or the exact
wording the brief that added these checks quoted) or a CONTROL that proves
the check does not fire on legitimate text. Run directly:

    python scripts/test_trim_comments_local.py

Exits 0 with "ALL PASSED" when every case behaves as expected, prints the
failing cases and exits 1 otherwise.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import trim_comments_local as tcl  # noqa: E402

FAILURES = []
TOTAL = 0


def check(name, got, want):
    global TOTAL
    TOTAL += 1
    ok = bool(got) == bool(want)
    print(f"{'ok  ' if ok else 'FAIL'}  {name}"
          f"{'' if ok else f'  (got {got!r}, want truthy={want})'}")
    if not ok:
        FAILURES.append(name)


# ---------------------------------------------------------------------------
# leaks_directive() - directive leakage
# ---------------------------------------------------------------------------

# Observed verbatim (caught in review, never committed): the model's own
# instruction word landing mid-prose in an otherwise plausible rewrite.
check(
    "directive leak: observed 'DELETE the measurement details.' mid-prose",
    tcl.leaks_directive(
        "",
        "Checked before anything else runs. DELETE the measurement "
        "details. Fire may rise before the assertion runs."),
    True,
)

# Control: ordinary lowercase prose using the same English word must not
# trip this - the whole point of requiring exact-case DELETE.
check(
    "control: lowercase 'delete' in ordinary prose",
    tcl.leaks_directive(
        "", "Dead code the compiler can delete the walk for is fine."),
    False,
)

# Control: this repo's own house style uses ALL-CAPS emphasis phrases that
# are not the directive vocabulary at all.
check(
    "control: house-style ALL-CAPS emphasis, not a directive verb",
    tcl.leaks_directive(
        "", "THE ONLY CHECK THE BAKED DISC-COUNT TABLE GETS is this one."),
    False,
)

# Control: a directive-shaped phrase already present in the ORIGINAL text is
# quoted forward, not leaked.
check(
    "control: directive phrase already present in the original",
    tcl.leaks_directive(
        "A config flag lets a caller REMOVE the cache entirely.",
        "A config flag lets a caller REMOVE the cache entirely if unused."),
    False,
)

# ---------------------------------------------------------------------------
# unresolved_citations() / response_problems() - unresolvable cross-reference
# ---------------------------------------------------------------------------

# Observed shape: a wrapped identifier truncated at the house-style `_-`
# break, with nothing valid stitched onto the far side of the hyphen - reads
# as a citation to a function that does not exist.
bad = tcl.unresolved_citations(
    "Same idea, reproducing test_a_screen_of_- and elsewhere.")
check(
    "unresolved: truncated wrap ('test_a_screen_of_-' + garbage)",
    bad,
    True,
)

# Control: a real, currently-over-the-limit repo identifier, cited plainly -
# must resolve.
bad = tcl.unresolved_citations(
    "See step_impulses()'s own comment in sand.c for the roll order.")
check(
    "control: real citation (step_impulses(), sand.c) resolves",
    bad,
    False,
)

# Control: the SAME wrap convention used correctly - continuing the
# identifier onto a real function name - must resolve after rejoining, not
# be flagged just because it happens to contain a `_-` break.
bad = tcl.unresolved_citations(
    "See test_the_four_liquid_scene_keeps_reacting_after_- settling for "
    "the other half.")
check(
    "control: correctly-wrapped identifier resolves after rejoining",
    bad,
    False,
)

# Control: an outright fabricated function name must be flagged.
bad = tcl.unresolved_citations(
    "See totally_fabricated_helper_xyz() for the details.")
check(
    "unresolved: a function name invented out of nothing",
    bad,
    True,
)

# ---------------------------------------------------------------------------
# rejoin_wraps() itself - the mechanism the citation check depends on
# ---------------------------------------------------------------------------

check(
    "rejoin_wraps: drops the hyphen and closes the gap",
    tcl.rejoin_wraps("after_- settling") == "after_settling",
    True,
)

print()
if FAILURES:
    print(f"FAILED: {len(FAILURES)} of {TOTAL} case(s) - see FAIL lines above")
    sys.exit(1)
print(f"ALL PASSED ({TOTAL} cases)")
sys.exit(0)
