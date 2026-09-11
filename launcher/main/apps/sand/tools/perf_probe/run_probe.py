#!/usr/bin/env python3
"""Interleaved best-of-N runner for the host attribution probe (bd
esp32c6-o2s).

Every earlier attribution round hand-rolled its own version of this loop -
run each candidate scene several times, round-robin rather than back-to-back,
so a slow moment on the machine (another process waking up, a thermal
throttle) lands on every scene about equally instead of being mistaken for
one scene's own cost. This is that loop, written once.

One probe invocation per (scene, round) - a fresh child process each time,
not one process asked to run several scenes in a row - so nothing about one
scene's malloc/free pattern or cache state can leak into the next scene's
number, and so a crash in one scene doesn't take the whole round down.

Usage:
    python run_probe.py <probe-binary> --n 5 water mixed_flip lava_stress
    python run_probe.py <probe-binary> --n 10 --scenes-from-list  # every
                                                                   # scene
                                                                   # the
                                                                   # binary
                                                                   # knows

Prints one row per scene: sample count, min, median, all in microseconds -
the same unit the probe's own ESP_LOGI lines report. Min is the number to
trust for attribution (the floor a mechanism can reach once nothing else on
the machine gets in the way); median is there to sanity-check min wasn't a
fluke.

COMPARING TWO BUILDS: --compare <other-binary>

Comparing two probe binaries (a candidate against a baseline) is something
this campaign does constantly, and everyone who has needed it has hand-
rolled a one-off driver rather than touch this file - the same accumulation
`bd esp32c6-o2s` already happened once for the probe binary itself
(build_probe.sh's own comment tells that history). Extend the one runner
instead of adding a second one.

    python run_probe.py <binary-A> --compare <binary-B> --n 15 water lava_stress

Runs both binaries in ABBA block order - A, B, B, A, repeated --n times per
scene - and takes the MIN across blocks, per binary, per scene. Both design
choices are deliberate, not arbitrary:

  - ABBA, not AABB or interleaved-by-round: a linear drift on the machine
    (something else waking up mid-run, a thermal ramp) lands roughly evenly
    on both binaries inside one block instead of favouring whichever binary
    happened to run first or last across the whole session.
  - MIN, not median: on this machine, at n=10, min and median have
    disagreed by 21-31% on the same pair of binaries and, worse, disagreed
    on which binary WON - a real reversal, not just a magnitude wobble.
    Going to n=15 across two ABBA blocks (four samples per scene per block)
    settled it. Min is the floor a binary can reach with nothing else on
    the machine getting in the way, which is the number attribution should
    trust; median folds in whatever noise happened to be sitting on the
    machine during the run, and that noise is exactly what ABBA block order
    is already there to cancel out - trusting median on top of it double-
    counts the same problem in the wrong direction.

Prints one row per scene: A's min, B's min, the delta, and the delta as a
percentage of A. All existing single-binary behaviour (no --compare) is
unchanged.
"""
import argparse
import re
import statistics
import subprocess
import sys

# Every scene prints exactly one "<number> us" figure to stdout when run
# alone - see the ESP_LOGI() call in whichever test body the requested
# scene's SAND_HOST_PROBE wrapper calls (suite_sand.c). Unity's own PASS/
# FAIL/summary text never matches this, so the last match in the captured
# output is always the scene's own timing line, whether or not the budget
# assertion after it "failed" (expected - see probe_main.c's own comment).
#
# No trailing \b after "us": the host stub for ESP_LOGI() (test/stubs/
# esp_log.h) is a bare printf with no newline appended, and Unity's very
# next output (the "file:line:name:PASS" line) starts printing immediately
# after with no separator - "us" and the "C" of the following path merge
# into one word as far as \b is concerned, so a boundary there never
# matches. The leading space before "us" is real (every format string has
# one), so it alone is enough to anchor this.
TIMING_RE = re.compile(r"(\d+) us")


def list_scenes(binary):
    out = subprocess.run(
        [binary, "--list"], capture_output=True, text=True, check=True
    ).stdout
    return [line.strip() for line in out.splitlines() if line.strip()]


def run_once(binary, scene):
    """Runs one scene once; returns its measured microseconds, or None if no
    timing line was found (the scene name was wrong, or the binary crashed
    before printing one - report_performance.py's raw-capture advice
    applies here too: if this keeps happening, look at the actual output,
    don't just retry)."""
    proc = subprocess.run(
        [binary, scene], capture_output=True, text=True
    )
    matches = TIMING_RE.findall(proc.stdout)
    if not matches:
        sys.stderr.write(
            f"run_probe: no timing line from scene '{scene}' "
            f"(exit {proc.returncode}); stdout follows:\n{proc.stdout}\n"
            f"stderr follows:\n{proc.stderr}\n"
        )
        return None
    return int(matches[-1])


def run_compare(binary_a, binary_b, scenes, n_blocks):
    """ABBA-ordered A/B comparison - see the module docstring's "COMPARING
    TWO BUILDS" section for why this order and why min. One block is four
    samples (A, B, B, A); returns {scene: {"A": [...], "B": [...]}}, the raw
    per-block values, so the caller can take min itself rather than this
    function silently deciding that on its behalf."""
    results = {scene: {"A": [], "B": []} for scene in scenes}
    for scene in scenes:
        for _block in range(n_blocks):
            for label, binary in (
                ("A", binary_a), ("B", binary_b),
                ("B", binary_b), ("A", binary_a),
            ):
                value = run_once(binary, scene)
                if value is not None:
                    results[scene][label].append(value)
    return results


def print_compare_table(results, scenes):
    name_w = max(len(s) for s in scenes)
    print(
        f"{'scene':<{name_w}}  {'A min':>8}  {'B min':>8}  "
        f"{'delta':>8}  {'delta %':>8}"
    )
    for scene in scenes:
        a_values = results[scene]["A"]
        b_values = results[scene]["B"]
        if not a_values or not b_values:
            print(f"{scene:<{name_w}}  {'--':>8}  {'--':>8}  {'--':>8}  {'--':>8}")
            continue
        a_min, b_min = min(a_values), min(b_values)
        delta = a_min - b_min
        pct = (delta / a_min) * 100 if a_min else 0.0
        print(
            f"{scene:<{name_w}}  {a_min:>8}  {b_min:>8}  "
            f"{delta:>8}  {pct:>7.2f}%"
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", help="path to the probe built by build_probe.sh")
    parser.add_argument("scenes", nargs="*", help="scene names, as printed by --list")
    parser.add_argument(
        "--n", type=int, default=5,
        help="rounds per scene (default 5); with --compare, ABBA blocks per "
             "scene instead (default still 5, but see the module docstring "
             "for why a comparison run wants --n 15)",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="run every scene the binary knows (--list), ignoring positional scenes",
    )
    parser.add_argument(
        "--compare", metavar="OTHER_BINARY",
        help="compare `binary` (A) against OTHER_BINARY (B) in ABBA block "
             "order, min per block per binary per scene - see the module "
             "docstring's 'COMPARING TWO BUILDS' section",
    )
    args = parser.parse_args()

    known = list_scenes(args.binary)
    if args.compare:
        known_b = list_scenes(args.compare)
        # A scene absent from one binary can never be compared - fail loud
        # here rather than silently reporting a one-sided min later.
        only_a = set(known) - set(known_b)
        only_b = set(known_b) - set(known)
        # Naming scenes explicitly says which comparison you want, so only
        # THOSE have to exist in both. Requiring the whole lists to match
        # made a bisect across time impossible: any commit that adds a scene
        # splits the range in two, and this campaign adds scenes regularly.
        if args.scenes:
            missing = [s for s in args.scenes
                       if s not in known or s not in known_b]
            if missing:
                parser.error("requested scenes missing from one binary: "
                             + ", ".join(sorted(missing)))
            only_a = only_b = set()
        if only_a or only_b:
            parser.error(
                f"scene lists differ between binaries - only in A: "
                f"{sorted(only_a)!r}, only in B: {sorted(only_b)!r}"
            )
    if args.all:
        scenes = known
    else:
        scenes = args.scenes
    if not scenes:
        parser.error("no scenes given (pass scene names, or --all)")
    unknown = [s for s in scenes if s not in known]
    if unknown:
        parser.error(
            f"unknown scene(s) {unknown!r} - known scenes: {known!r}"
        )

    if args.compare:
        results = run_compare(args.binary, args.compare, scenes, args.n)
        print_compare_table(results, scenes)
        return

    results = {scene: [] for scene in scenes}
    for round_num in range(args.n):
        for scene in scenes:
            value = run_once(args.binary, scene)
            if value is not None:
                results[scene].append(value)

    name_w = max(len(s) for s in scenes)
    print(f"{'scene':<{name_w}}  {'n':>3}  {'min':>8}  {'median':>8}")
    for scene in scenes:
        values = results[scene]
        if not values:
            print(f"{scene:<{name_w}}  {'0':>3}  {'--':>8}  {'--':>8}")
            continue
        print(
            f"{scene:<{name_w}}  {len(values):>3}  {min(values):>8}  "
            f"{round(statistics.median(values)):>8}"
        )


if __name__ == "__main__":
    main()
