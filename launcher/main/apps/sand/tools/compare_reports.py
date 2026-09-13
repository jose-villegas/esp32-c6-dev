#!/usr/bin/env python3
"""Diffs two markdown reports from report_performance.py and prints a delta
table, so a round of tuning gets compared by machine instead of by eye.

Hand-comparing two 13+ row tables across a browser tab or two terminal
scrollbacks is slow and error-prone, and it throws away the one piece of
context that actually tells a real regression from flash-layout noise: two
of the frame-budget tests exercise no liquid, reaction or gas code at all
(`test_a_full_size_step_fits_in_the_frame_budget` and
`test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget`). Nothing
under test changes what those two cost, so however much *they* move between
two captures is exactly how much any other row can move for free - link
placement, cache lines, whatever the flash layout lottery deals that build.
This tool takes that measured movement as a per-comparison noise floor and
uses it to label every other row `signal` or `layout?`, instead of leaving
that judgment call to a human skimming two tables.

The floor is measured fresh from the two reports being compared, every
time - not a constant, because the controls are build-specific and would
rot the moment someone tuned an unrelated budget. See classify() below.

It also reads the `*_raw.txt` beside each report, for the pass-decomposition
rows a round's gates print (`gas split: rise off: 191471 us`). Those never
reach the markdown table - report_performance.py only tabulates budgeted
tests - so without this they get diffed by eye, which is how a round ends up
hand-writing this comparison in a scratch file. A decomposition line is
recognised by its shape and nothing else: `device_tests: <prefix>: <label>:
<n> us`, the second colon being what separates a split's row from an
ordinary benchmark's one-line result.

THE CAVEAT IS PRINTED, NOT JUST DOCUMENTED. Absolute microseconds do not
compare across differently-scoped builds, and barely compare across builds
whose flash layout moved much (Perf-Round-Guide.md, "Only within-capture
comparisons are trustworthy"). The clearest machine-readable signature of the
first mistake is the total run time: a perf-scoped capture runs 39 timed
tests in ~2m and a full one 952 in ~7m, so a large gap between the two
reports' totals means the two tables are not comparable at all. That check
runs on every comparison and says so above the numbers.

Usage:
    python compare_reports.py OLD.md NEW.md [--threshold PCT]

Exits non-zero if any row regressed by more than the threshold, so it can
gate a round. --verdict keeps its own stricter contract (exit 0 only for a
measured win) for the optimisation loop, which is the only caller.
"""
import argparse
import os
import re
import sys

# report_performance.py's budget table: | `name` | budget | measured | headroom | status |
BUDGET_ROW_RE = re.compile(
    r"^\|\s*`(?P<name>\w+)`\s*\|\s*[^|]+\|\s*(?P<measured>[^|]+?)\s*\|\s*[^|]+\|\s*[^|]+\|\s*$"
)
# Its second table, tests measured with no fixed budget: | `name` | measured |
MEASURED_ROW_RE = re.compile(r"^\|\s*`(?P<name>\w+)`\s*\|\s*(?P<measured>[^|]+?)\s*\|\s*$")

# report_performance.py's headline: `Total run time: 7m 10.6s (430600 ms)`.
TOTAL_RE = re.compile(r"^Total run time:\s*(?P<human>.+?)\s*\((?P<ms>\d+)\s*ms\)")
# Where the report says its capture came from, used only for that file's name.
SOURCE_RE = re.compile(r"^Source:\s*`(?P<path>[^`]+)`")

# A pass-decomposition row in the raw serial capture. The trailing text after
# `us` varies (some rows print a share of the whole, some do not), so only the
# leading value is captured; the two-colon shape is what tells a split's row
# from an ordinary benchmark result on the same log tag.
DECOMP_RE = re.compile(
    r"^I \(\d+\) device_tests: (?P<key>.+?): (?P<us>\d+) us(?![a-z])"
)

# Total run times further apart than this are the signature of a perf-scoped
# capture being compared against a full one - 39 timed tests against 952.
# Deliberately loose: a genuine within-scope pair lands within a few percent,
# so anything near this is already the wrong comparison.
SCOPE_MISMATCH_PCT = 20.0

# The two frame-budget tests that run no liquid, reaction or gas code -
# anything that moves them is flash layout, not the change under test.
CONTROLS = (
    "test_a_full_size_step_fits_in_the_frame_budget",
    "test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget",
)


def parse_report(path: str) -> dict:
    """Returns {test name: measured microseconds}, pooling both of
    report_performance.py's tables - a name appears in exactly one of them,
    so there is no ambiguity to resolve between the two row shapes."""
    measured = {}
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = BUDGET_ROW_RE.match(line)
            if not m:
                m = MEASURED_ROW_RE.match(line)
            if not m:
                continue
            value = m.group("measured").strip()
            if value == "?" or not value.lstrip("+-").isdigit():
                continue  # unmeasured row (test didn't run this capture)
            measured[m.group("name")] = int(value)
    return measured


def parse_total(path: str):
    """(human, milliseconds) from the report's headline, or None."""
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = TOTAL_RE.match(line.strip())
            if m:
                return m.group("human"), int(m.group("ms"))
    return None


def find_raw(path: str):
    """The `*_raw.txt` beside a report.

    By timestamp, not by the report's own `Source:` line: that line records an
    absolute path from whichever checkout took the capture, and a report read
    on another machine - or moved out of the capture checkout, which is the
    normal way one is kept - would send this looking in a directory that does
    not exist here. The stamp in the two filenames is the same by
    construction (capture_ref_<ref>_<stamp>.md next to
    performance_<stamp>_raw.txt), so it survives the move."""
    directory = os.path.dirname(os.path.abspath(path))
    base = os.path.basename(path)
    stamp = re.search(r"(\d{8}_\d{6})", base)
    if stamp:
        candidate = os.path.join(directory, "performance_%s_raw.txt" % stamp.group(1))
        if os.path.exists(candidate):
            return candidate
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = SOURCE_RE.match(line.strip())
            if m:
                candidate = os.path.join(directory, os.path.basename(m.group("path")))
                if os.path.exists(candidate):
                    return candidate
            if line.startswith("|"):
                break
    return None


def parse_decomposition(path: str) -> dict:
    """{`<prefix>: <label>`: microseconds} from a raw capture.

    First occurrence wins. A repeat means the same split ran twice in one
    capture, which is a capture worth looking at by hand rather than one to
    silently average."""
    rows = {}
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = DECOMP_RE.match(line.strip())
            if not m:
                continue
            key = m.group("key")
            if ": " not in key:
                continue    # an ordinary benchmark result, not a split's row
            rows.setdefault(key, int(m.group("us")))
    return rows


# Smallest absolute movement, in microseconds, that --verdict will treat as
# real regardless of how large it looks as a percentage. See its use below.
MIN_ABS_DELTA_US = 20


def pct_delta(old: int, new: int) -> float:
    if old == 0:
        return float("inf") if new else 0.0
    return (new - old) / old * 100.0


def classify(old_report: dict, new_report: dict):
    """Returns (floor_pct, control_rows, error). error is a message string
    if either control is missing from either report - without both, there
    is nothing to measure the floor from and calling every row "signal" or
    "layout?" would just be a guess."""
    control_rows = []
    for name in CONTROLS:
        if name not in old_report or name not in new_report:
            return None, [], f"control `{name}` is missing from one of the two reports - cannot establish a noise floor"
        old_v, new_v = old_report[name], new_report[name]
        control_rows.append((name, old_v, new_v, new_v - old_v, pct_delta(old_v, new_v)))
    floor_pct = max(abs(row[4]) for row in control_rows)

    # A floor of exactly zero is not a claim that 1us is meaningful - it
    # means both controls happened to land on identical values, which does
    # happen here: the flash layout lottery on this device is quantised
    # (controls come back on one of a small number of value-pairs, never
    # between), so two different builds can produce byte-identical control
    # rows. Taking 0.0% literally labels a +1us move "signal" and invites
    # chasing noise, which is the exact failure this tool exists to prevent.
    # 0.5% is the smallest move this campaign has ever attributed to a real
    # cause; below that, a single capture cannot tell you anything.
    floor_pct = max(floor_pct, 0.5)
    return floor_pct, control_rows, None


def format_row(name, old_v, new_v, tag=None):
    delta = new_v - old_v
    pct = pct_delta(old_v, new_v)
    line = f"| `{name}` | {old_v} | {new_v} | {delta:+d} | {pct:+.1f}% |"
    if tag:
        line += f" {tag} |"
    return line


def print_delta_table(old: dict, new: dict, threshold: float,
                      floor_pct: float = None, skip: set = frozenset()) -> int:
    """Common rows, largest absolute percentage move first. Returns how many
    rows regressed by more than `threshold`.

    Rows that moved less than the threshold are hidden, and the COUNT OF THEM
    IS ALWAYS PRINTED, even when it is zero. A filter that quietly drops rows
    is how a regression leaves a round unnoticed, and the whole point of
    comparing by machine is that nothing falls off the bottom of the table."""
    common = sorted((set(old) & set(new)) - set(skip),
                    key=lambda n: -abs(pct_delta(old[n], new[n])))

    print("| Test | Old (us) | New (us) | Delta | Delta % |"
          + (" |" if floor_pct is not None else ""))
    print("|---|---:|---:|---:|---:|" + ("---|" if floor_pct is not None else ""))

    hidden, regressed = 0, 0
    for name in common:
        old_v, new_v = old[name], new[name]
        pct = pct_delta(old_v, new_v)
        if pct > threshold:
            regressed += 1
        if abs(pct) < threshold:
            hidden += 1
            continue
        tag = None
        if floor_pct is not None:
            tag = "signal" if abs(pct) > floor_pct else "layout?"
        print(format_row(name, old_v, new_v, tag))
    print()
    print(f"{len(common) - hidden} of {len(common)} rows shown; {hidden} hidden "
          f"for moving less than the {threshold:.1f}% threshold.")
    print()
    return regressed


def report_missing(old: dict, new: dict, skip: set, old_title: str, new_title: str):
    """Rows in one side only - a renamed test, or a scope change."""
    for title, missing, source in (
        (old_title, sorted(set(old) - set(new) - set(skip)), old),
        (new_title, sorted(set(new) - set(old) - set(skip)), new),
    ):
        if not missing:
            continue
        print(f"### Only in {title}")
        print()
        for name in missing:
            print(f"- `{name}`: {source[name]} us")
        print()


def decompositions(old_path: str, new_path: str):
    """The gate rows from each report's raw capture, ({}, {}) if absent."""
    out = []
    for path in (old_path, new_path):
        raw = find_raw(path)
        out.append(parse_decomposition(raw) if raw else {})
    return out[0], out[1]


def print_scope_check(old_path: str, new_path: str):
    """The totals, and the warning that the two tables may not be comparable
    at all. Printed first, above the numbers, because it decides whether the
    numbers mean anything."""
    print("## Scope check")
    print()
    old_total, new_total = parse_total(old_path), parse_total(new_path)
    if old_total is None or new_total is None:
        print("One of the reports carries no `Total run time:` line, so the "
              "scope check could not run. Confirm by hand that both captures "
              "were taken the same way before reading a single row below.")
        print()
        return

    print(f"Total run time: {old_total[0]} -> {new_total[0]}.")
    if old_total[1] > 0:
        gap = abs(new_total[1] - old_total[1]) / old_total[1] * 100.0
        if gap > SCOPE_MISMATCH_PCT:
            print()
            print(f"**WARNING: the two runs differ by {gap:.0f}% of wall time.** "
                  "That is the signature of a perf-scoped capture compared "
                  "against a full one, and two differently-scoped images are "
                  "different flash layouts: the absolute microseconds below do "
                  "not compare, and no threshold makes them.")
    print()
    print("Even within one scope, absolute numbers compare only between "
          "captures of the same shape - a change that grows a hot function "
          "relocates everything after it and moves every row together. Read "
          "the controls before anything else.")
    print()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("old_report", help="Earlier performance_*.md")
    parser.add_argument("new_report", help="Later performance_*.md")
    parser.add_argument("--verdict", action="store_true",
                        help="Print one machine-readable line instead of the "
                             "tables, and exit 0 only if this is a win: at "
                             "least one row improved beyond the noise floor "
                             "and none regressed beyond it. For the "
                             "optimisation loop, which cannot read a table.")
    parser.add_argument("--threshold", type=float, default=0.4,
                        help="percent below which a row is hidden, and above "
                             "which a regression fails the run (default 0.4)")
    args = parser.parse_args()

    old_report = parse_report(args.old_report)
    new_report = parse_report(args.new_report)

    floor_pct, control_rows, error = classify(old_report, new_report)
    if error:
        print(f"ERROR: {error}")
        return 1

    if args.verdict:
        # A row is only counted when BOTH reports measured it, so a newly
        # added or newly failing-to-run row can never be read as a win.
        improved, regressed, best, worst = [], [], 0.0, 0.0
        for name, new_v in sorted(new_report.items()):
            if name not in old_report:
                continue
            d = pct_delta(old_report[name], new_v)
            if abs(d) <= floor_pct:
                continue
            # A percentage floor alone is not enough on the very small rows.
            # test_an_unchanged_frame_costs_almost_nothing measures 3 us, so
            # one microsecond of timer quantisation reads as 33% and would
            # be banked as a large win by a loop that only checked percent.
            # Requiring an absolute movement too makes the tiny rows
            # unwinnable rather than wildly noisy, which is correct: nothing
            # worth finding hides in a 1 us move.
            if abs(new_v - old_report[name]) < MIN_ABS_DELTA_US:
                continue
            if d < 0:
                improved.append(name)
                best = min(best, d)
            else:
                regressed.append(name)
                worst = max(worst, d)
        # A regression beyond the floor disqualifies the candidate outright,
        # however large the win elsewhere: this loop is not authorised to
        # trade one budget against another. A human decides that.
        win = bool(improved) and not regressed
        print(f"VERDICT {'WIN' if win else 'NO'} floor={floor_pct:.1f}% "
              f"improved={len(improved)} regressed={len(regressed)} "
              f"best={best:.1f}% worst={worst:.1f}%")
        for name in improved:
            print(f"  improved {name} {pct_delta(old_report[name], new_report[name]):.1f}%")
        for name in regressed:
            print(f"  REGRESSED {name} {pct_delta(old_report[name], new_report[name]):.1f}%")
        return 0 if win else 1

    print(f"# Comparing `{args.old_report}` -> `{args.new_report}`")
    print()
    print_scope_check(args.old_report, args.new_report)
    print("## Controls (noise floor)")
    print()
    print("These two run no liquid, reaction or gas code, so their movement "
          "between these two specific captures is flash layout, not signal. "
          "The floor below is measured from THEM, here, not a hardcoded "
          "constant - it will differ for any other pair of captures.")
    print()
    print("| Test | Old (us) | New (us) | Delta | Delta % |")
    print("|---|---:|---:|---:|---:|")
    for name, old_v, new_v, _delta, _pct in control_rows:
        print(format_row(name, old_v, new_v))
    print()
    measured_floor = max(abs(row[4]) for row in control_rows)
    origin = ("the larger of the two control moves above"
              if measured_floor >= 0.5
              else f"the 0.5% minimum - the controls themselves moved only "
                   f"{measured_floor:.1f}%, which is too little to measure a "
                   f"floor from")
    print(f"Noise floor for this comparison: **{floor_pct:.1f}%** ({origin}).")
    # Worth printing raw, not just the floor: across this campaign the
    # controls were observed landing on one of two specific value pairs -
    # an observation worth a reader's attention, not worth hardcoding as a
    # rule, since it is a property of this build's flash layout, not of the
    # test.
    print(f"Raw control values: {control_rows[0][0]}: {control_rows[0][1]} -> {control_rows[0][2]}, "
          f"{control_rows[1][0]}: {control_rows[1][1]} -> {control_rows[1][2]}.")
    print()

    control_names = set(CONTROLS)

    print("## Everything else")
    print()
    print(f"Rows beyond the {floor_pct:.1f}% floor are marked `signal`; rows "
          "within it are marked `layout?` - their move is no bigger than "
          "what the two controls moved for free, so it's not distinguishable "
          "from flash layout noise in this comparison.")
    print()
    regressed = print_delta_table(old_report, new_report, args.threshold,
                                  floor_pct=floor_pct, skip=control_names)
    report_missing(old_report, new_report, control_names,
                   "the old report (removed or renamed)",
                   "the new report (added or renamed)")

    old_decomp, new_decomp = decompositions(args.old_report, args.new_report)
    if old_decomp and new_decomp:
        print("## Pass decomposition (from the raw captures)")
        print()
        print("Gate rows, read from the `*_raw.txt` beside each report. These "
              "are upper bounds from measure-by-deleting and need not sum, so "
              "they are not gated on below - a row moving here says where a "
              "step's time went, not that anything regressed.")
        print()
        print_delta_table(old_decomp, new_decomp, args.threshold)
        report_missing(old_decomp, new_decomp, set(),
                       "the old capture's splits",
                       "the new capture's splits")
    elif old_decomp or new_decomp:
        which = "old" if old_decomp else "new"
        print(f"> Only the {which} capture carries pass-decomposition rows, so "
              "there is nothing to diff. That is the normal state between "
              "rounds - the gates are scaffolding.")
        print()

    return 1 if regressed else 0


if __name__ == "__main__":
    sys.exit(main())
