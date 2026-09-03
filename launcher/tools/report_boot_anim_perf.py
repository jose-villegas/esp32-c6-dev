#!/usr/bin/env python3
"""Turns a raw device capture into a markdown report of suite_boot_anim_
perf.c's per-checkpoint frame breakdown - the same idea as main/apps/cube/
tools/report_cube_perf.py, but for boot_anim's own suite, whose output shape
is different enough (only Total/Image/Present carry a full min/max/avg/med/
p95 breakdown; Clear/Floor/Axes/Curve/Zeros/Title are logged as an average
only, to keep six checkpoints' worth of console output from scrolling past
what a 300s capture window can hold) that it needs its own parser rather
than reusing cube's.

ESP_LOGI is the only persistent output a DEVICE_BUILD suite has here (no
mounted filesystem - see report_cube_perf.py's own comment on why), so this
generates the report on the host from a captured serial log instead.

Each checkpoint's own label states what point in the animation it froze
time at (curve_climbing, crossfade_mid, ...) - see suite_boot_anim_perf.c's
own build_checkpoints() - rather than this script needing to know what any
particular one means, so a new checkpoint added to the suite shows up in
the report with no changes needed here.

Usage:
    python tools/report_boot_anim_perf.py <raw_capture.txt> <out.md>

Lives in tools/, not test/suites/, the same convention gen_boot_anim_
timeline.py and gen_boot_anim_image.py already follow for boot_anim's own
host-side tooling - it is not app-owned the way cube's report generator is.
"""
import argparse
import re
import sys
from datetime import datetime, timezone

# "I (2795) boot_anim_perf: === BOOT_ANIM PERF curve_climbing (now_ms=1510, 60 samples) ==="
HEADER_RE = re.compile(
    r"boot_anim_perf:\s*===\s*BOOT_ANIM PERF\s+(?P<label>\S+)\s+"
    r"\(now_ms=(?P<now_ms>\d+),\s+(?P<samples>\d+)\s+samples\)\s*==="
)

# "I (...) boot_anim_perf: Total:   min=29997us max=30607us avg=30009us med=29999us p95=29999us (33.3/33.3/33.3 fps)"
FULL_PHASE_RE = re.compile(
    r"boot_anim_perf:\s*(?P<phase>Total|Image|Present):\s*"
    r"min=(?P<min>\d+)us\s+max=(?P<max>\d+)us\s+avg=(?P<avg>\d+)us\s+"
    r"med=(?P<med>\d+)us\s+p95=(?P<p95>\d+)us"
)

# "I (...) boot_anim_perf: Floor:   avg=6852us (22.8%)"
AVG_PHASE_RE = re.compile(
    r"boot_anim_perf:\s*(?P<phase>Clear|Floor|Axes|Curve|Zeros|Title):\s*"
    r"avg=(?P<avg>\d+)us"
)

PHASE_ORDER = ["Total", "Clear", "Floor", "Axes", "Curve", "Zeros", "Image", "Title", "Present"]
FULL_PHASES = {"Total", "Image", "Present"}


def parse_capture(capture_path: str):
    with open(capture_path, "r", errors="replace") as f:
        lines = f.read().splitlines()

    runs = {}          # label -> {"now_ms": int, "samples": int, "phases": {phase: {...}}}
    order = []          # labels in the order they first appeared
    duplicates = []     # labels whose header line appeared more than once
    current_label = None

    for line in lines:
        hm = HEADER_RE.search(line)
        if hm:
            current_label = hm.group("label")
            if current_label not in runs:
                order.append(current_label)
            else:
                # A second header for a label already seen - two runs of the
                # same suite concatenated into one capture (e.g. re-run after
                # a fix, or a full-selftest capture that looped). Silently
                # overwriting here would let a LATER, possibly-worse run
                # (still contended, still warming up) quietly replace an
                # earlier one with no trace in the report - the exact "which
                # numbers am I actually looking at" trap this list exists to
                # avoid. Last occurrence still wins (the most recent run in
                # the capture is the most likely one anybody meant to keep),
                # but every overwrite is now surfaced instead of silent.
                duplicates.append(current_label)
            runs[current_label] = {
                "now_ms": int(hm.group("now_ms")),
                "samples": int(hm.group("samples")),
                "phases": {},
            }
            continue

        fm = FULL_PHASE_RE.search(line)
        if fm and current_label is not None:
            runs[current_label]["phases"][fm.group("phase")] = {
                "min": int(fm.group("min")),
                "max": int(fm.group("max")),
                "avg": int(fm.group("avg")),
                "med": int(fm.group("med")),
                "p95": int(fm.group("p95")),
            }
            continue

        am = AVG_PHASE_RE.search(line)
        if am and current_label is not None:
            runs[current_label]["phases"][am.group("phase")] = {
                "avg": int(am.group("avg")),
            }

    return runs, order, duplicates


def incomplete_labels(runs, order):
    """Labels whose run is missing one or more of PHASE_ORDER's lines - a
    capture that was cut off (timeout, device reset, a too-short --timeout on
    tools/sweeps/capture_runsuite.py) mid-checkpoint would otherwise produce a
    report that LOOKS complete: every table still renders, just with a "?" in
    a cell here and there that is easy to read as "this phase cost nothing"
    rather than "this line never arrived". Surfaced as an explicit warning
    instead, both to stdout and in the report itself."""
    missing = []
    for label in order:
        have = set(runs[label]["phases"].keys())
        gap = [p for p in PHASE_ORDER if p not in have]
        if gap:
            missing.append((label, gap))
    return missing


def fps(us: int) -> str:
    return f"{1000000.0 / us:.1f}" if us > 0 else "?"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_path", help="Raw capture (RUNSUITE run_boot_anim_perf_suite, or a full selftest capture)")
    parser.add_argument("out_path", help="Markdown file to write")
    args = parser.parse_args()

    runs, order, duplicates = parse_capture(args.capture_path)

    lines = []
    lines.append("# Boot Animation Performance Report")
    lines.append("")
    lines.append(f"Captured: {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')}")
    lines.append(f"Source: `{args.capture_path}`")
    lines.append("")

    gaps = []
    if not runs:
        lines.append("> No `boot_anim_perf` run headers found in this capture - "
                      "the suite may not have run (RUNSUITE run_boot_anim_perf_suite "
                      "not sent, or CONFIG_LAUNCHER_SELFTEST off), or the device may "
                      "have crashed before it reached one.")
        lines.append("")
    else:
        gaps = incomplete_labels(runs, order)
        if gaps or duplicates:
            lines.append("## Warnings")
            lines.append("")
            for label, missing_phases in gaps:
                msg = (f"`{label}` is missing {', '.join(missing_phases)} - "
                       f"the capture was likely cut off before this checkpoint "
                       f"finished logging; treat its numbers below as incomplete, "
                       f"not zero.")
                lines.append(f"- {msg}")
                print(f"WARNING: {msg}", file=sys.stderr)
            for label in duplicates:
                msg = (f"`{label}` had more than one BOOT_ANIM PERF header in "
                       f"this capture - only the LAST occurrence is reported "
                       f"below, earlier ones were discarded.")
                lines.append(f"- {msg}")
                print(f"WARNING: {msg}", file=sys.stderr)
            lines.append("")
        lines.append("## Checkpoints")
        lines.append("")
        lines.append("| Checkpoint | now_ms | samples |")
        lines.append("|---|---:|---:|")
        for label in order:
            run = runs[label]
            lines.append(f"| `{label}` | {run['now_ms']} | {run['samples']} |")
        lines.append("")

        lines.append("## Comparison (average, us)")
        lines.append("")
        lines.append("| Phase | " + " | ".join(f"`{label}`" for label in order) + " |")
        lines.append("|---|" + "---:|" * len(order))
        for phase in PHASE_ORDER:
            row = [f"{phase} (us)"]
            for label in order:
                p = runs[label]["phases"].get(phase)
                row.append(str(p["avg"]) if p else "?")
            lines.append("| " + " | ".join(row) + " |")
        lines.append("")
        lines.append("| | " + " | ".join(order) + " |")
        lines.append("|---|" + "---:|" * len(order))
        for stat, stat_label in (("avg", "avg"), ("med", "median"), ("p95", "p95")):
            row = [f"**Total fps ({stat_label})**"]
            for label in order:
                total = runs[label]["phases"].get("Total")
                row.append(fps(total[stat]) if total and stat in total else "?")
            lines.append("| " + " | ".join(row) + " |")
        lines.append("")

        for label in order:
            run = runs[label]
            lines.append(f"## `{label}` (now_ms={run['now_ms']}, {run['samples']} samples)")
            lines.append("")
            lines.append("| Phase | Min (us) | Max (us) | Avg (us) | Median (us) | P95 (us) |")
            lines.append("|---|---:|---:|---:|---:|---:|")
            for phase in PHASE_ORDER:
                p = run["phases"].get(phase)
                if p is None:
                    lines.append(f"| {phase} | ? | ? | ? | ? | ? |")
                    continue
                bold = "**" if phase == "Total" else ""
                if phase in FULL_PHASES:
                    lines.append(
                        f"| {bold}{phase}{bold} | {p['min']} | {p['max']} | "
                        f"{p['avg']} | {p['med']} | {p['p95']} |"
                    )
                else:
                    lines.append(f"| {phase} | - | - | {p['avg']} | - | - |")
            lines.append("")

    with open(args.out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    warning_count = len(gaps) + len(duplicates)
    suffix = f" ({warning_count} warning(s) - see stderr)" if warning_count else ""
    print(f"{len(runs)} checkpoint(s) -> {args.out_path}{suffix}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
