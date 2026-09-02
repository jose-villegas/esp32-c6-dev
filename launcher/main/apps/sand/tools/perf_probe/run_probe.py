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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", help="path to the probe built by build_probe.sh")
    parser.add_argument("scenes", nargs="*", help="scene names, as printed by --list")
    parser.add_argument(
        "--n", type=int, default=5, help="rounds per scene (default 5)"
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="run every scene the binary knows (--list), ignoring positional scenes",
    )
    args = parser.parse_args()

    known = list_scenes(args.binary)
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
