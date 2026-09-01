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

Usage:
    python compare_reports.py OLD.md NEW.md
"""
import argparse
import re
import sys

# report_performance.py's budget table: | `name` | budget | measured | headroom | status |
BUDGET_ROW_RE = re.compile(
    r"^\|\s*`(?P<name>\w+)`\s*\|\s*[^|]+\|\s*(?P<measured>[^|]+?)\s*\|\s*[^|]+\|\s*[^|]+\|\s*$"
)
# Its second table, tests measured with no fixed budget: | `name` | measured |
MEASURED_ROW_RE = re.compile(r"^\|\s*`(?P<name>\w+)`\s*\|\s*(?P<measured>[^|]+?)\s*\|\s*$")

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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("old_report", help="Earlier performance_*.md")
    parser.add_argument("new_report", help="Later performance_*.md")
    args = parser.parse_args()

    old_report = parse_report(args.old_report)
    new_report = parse_report(args.new_report)

    floor_pct, control_rows, error = classify(old_report, new_report)
    if error:
        print(f"ERROR: {error}")
        return 1

    print(f"# Comparing `{args.old_report}` -> `{args.new_report}`")
    print()
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
    print(f"Noise floor for this comparison: **{floor_pct:.1f}%** "
          "(the larger of the two control moves above).")
    # Worth printing raw, not just the floor: across this campaign the
    # controls were observed landing on one of two specific value pairs -
    # an observation worth a reader's attention, not worth hardcoding as a
    # rule, since it is a property of this build's flash layout, not of the
    # test.
    print(f"Raw control values: {control_rows[0][0]}: {control_rows[0][1]} -> {control_rows[0][2]}, "
          f"{control_rows[1][0]}: {control_rows[1][1]} -> {control_rows[1][2]}.")
    print()

    control_names = set(CONTROLS)
    common = sorted(
        (set(old_report) & set(new_report)) - control_names,
        key=lambda n: -abs(pct_delta(old_report[n], new_report[n])),
    )
    only_old = sorted(set(old_report) - set(new_report) - control_names)
    only_new = sorted(set(new_report) - set(old_report) - control_names)

    print("## Everything else")
    print()
    print(f"Rows beyond the {floor_pct:.1f}% floor are marked `signal`; rows "
          "within it are marked `layout?` - their move is no bigger than "
          "what the two controls moved for free, so it's not distinguishable "
          "from flash layout noise in this comparison.")
    print()
    print("| Test | Old (us) | New (us) | Delta | Delta % | |")
    print("|---|---:|---:|---:|---:|---|")
    for name in common:
        old_v, new_v = old_report[name], new_report[name]
        tag = "signal" if abs(pct_delta(old_v, new_v)) > floor_pct else "layout?"
        print(format_row(name, old_v, new_v, tag))
    print()

    if only_old:
        print("### Only in the old report (removed or renamed)")
        print()
        for name in only_old:
            print(f"- `{name}`: {old_report[name]} us")
        print()
    if only_new:
        print("### Only in the new report (added or renamed)")
        print()
        for name in only_new:
            print(f"- `{name}`: {new_report[name]} us")
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
