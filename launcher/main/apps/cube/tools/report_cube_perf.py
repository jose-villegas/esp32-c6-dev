#!/usr/bin/env python3
"""Turns a raw device self-test capture into a markdown report of
suite_cube_perf.c's frame-budget breakdown: one comparison table across every
run variant found (with_hud, no_hud, full_clear, ...), then each variant's
own min/max/avg/median/p95 detail - the same shape suite_cube_perf.c would
write itself if this project had a mounted filesystem to write to (it does
not: no SPIFFS partition exists, and the SD card is unmounted again right
after POST to free the SPI2 bus the display needs - see main/boot/post.c).
ESP_LOGI is the only persistent output a DEVICE_BUILD suite has here, the
same as suite_sand.c's own frame-budget tests, so this generates the report
on the host from a captured serial log instead, mirroring
main/apps/sand/tools/report_performance.py's own reason for existing.

Usage:
    python main/apps/cube/tools/report_cube_perf.py <raw_capture.txt> <out.md>

Lives under the cube app because report_cube_perf.sh is its only caller -
same convention as sand's own tools/ folder: deleting the app takes its
tooling with it.
"""
import argparse
import re
import sys
from datetime import datetime, timezone

# "I (11692) cube_perf: === CUBE PERF with_hud (274 frames over 10s) ==="
HEADER_RE = re.compile(
    r"cube_perf:\s*===\s*CUBE PERF\s+(?P<label>\S+)\s+"
    r"\((?P<frames>\d+)\s+frames over\s+(?P<seconds>\d+)s\)\s*==="
)

# "I (...) cube_perf: Logic:   min=766us max=...us avg=766us med=772us p95=...us (2.1%)"
# The trailing parenthetical differs per phase (Total's is an fps triplet,
# the rest are a percentage of Total) and is not needed for the table - the
# five numbers before it are.
PHASE_RE = re.compile(
    r"cube_perf:\s*(?P<phase>Total|Logic|Raster|HUD|Present):\s*"
    r"min=(?P<min>\d+)us\s+max=(?P<max>\d+)us\s+avg=(?P<avg>\d+)us\s+"
    r"med=(?P<med>\d+)us\s+p95=(?P<p95>\d+)us"
)

# "I (...) cube_perf: INTERLACED (100 frames): avg=28446us (35.2 fps) min=24000 max=32998"
INTERLACED_RE = re.compile(
    r"cube_perf:\s*INTERLACED\s*\((?P<frames>\d+)\s+frames\):\s*"
    r"avg=(?P<avg>\d+)us\s*\((?P<fps>[\d.]+)\s*fps\)\s*"
    r"min=(?P<min>\d+)\s*max=(?P<max>\d+)"
)

PHASE_ORDER = ["Total", "Logic", "Raster", "HUD", "Present"]


def parse_capture(capture_path: str):
    with open(capture_path, "r", errors="replace") as f:
        lines = f.read().splitlines()

    runs = {}          # label -> {"frames": int, "seconds": int, "phases": {phase: {...}}}
    interlaced = None
    current_label = None

    for line in lines:
        hm = HEADER_RE.search(line)
        if hm:
            current_label = hm.group("label")
            runs[current_label] = {
                "frames": int(hm.group("frames")),
                "seconds": int(hm.group("seconds")),
                "phases": {},
            }
            continue

        pm = PHASE_RE.search(line)
        if pm and current_label is not None:
            runs[current_label]["phases"][pm.group("phase")] = {
                "min": int(pm.group("min")),
                "max": int(pm.group("max")),
                "avg": int(pm.group("avg")),
                "med": int(pm.group("med")),
                "p95": int(pm.group("p95")),
            }
            continue

        im = INTERLACED_RE.search(line)
        if im:
            interlaced = {
                "frames": int(im.group("frames")),
                "avg": int(im.group("avg")),
                "fps": float(im.group("fps")),
                "min": int(im.group("min")),
                "max": int(im.group("max")),
            }

    return runs, interlaced


def fps(us: int) -> str:
    return f"{1000000.0 / us:.1f}" if us > 0 else "?"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_path", help="Raw capture from tools/sweeps/capture_selftest.py")
    parser.add_argument("out_path", help="Markdown file to write")
    args = parser.parse_args()

    runs, interlaced = parse_capture(args.capture_path)

    lines = []
    lines.append("# Cube App Performance Report")
    lines.append("")
    lines.append(f"Captured: {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')}")
    lines.append(f"Source: `{args.capture_path}`")
    lines.append("")

    if not runs:
        lines.append("> No `cube_perf` run headers found in this capture - "
                      "the suite may not have run, or the device may have "
                      "crashed before it reached one.")
        lines.append("")
    else:
        labels = list(runs.keys())

        lines.append("## Comparison (average, us)")
        lines.append("")
        lines.append("| Phase | " + " | ".join(f"`{label}`" for label in labels) + " |")
        lines.append("|---|" + "---:|" * len(labels))
        for phase in PHASE_ORDER:
            row = [phase]
            for label in labels:
                p = runs[label]["phases"].get(phase)
                row.append(str(p["avg"]) if p else "?")
            lines.append("| " + " | ".join(row) + " |")
        lines.append("")
        lines.append("| | " + " | ".join(labels) + " |")
        lines.append("|---|" + "---:|" * len(labels))
        total_row = ["**fps (avg)**"]
        for label in labels:
            total = runs[label]["phases"].get("Total")
            total_row.append(fps(total["avg"]) if total else "?")
        lines.append("| " + " | ".join(total_row) + " |")
        lines.append("")

        for label in labels:
            run = runs[label]
            lines.append(f"## `{label}` ({run['frames']} frames over {run['seconds']}s)")
            lines.append("")
            lines.append("| Phase | Min | Max | Avg | Median | P95 |")
            lines.append("|---|---:|---:|---:|---:|---:|")
            for phase in PHASE_ORDER:
                p = run["phases"].get(phase)
                if p is None:
                    lines.append(f"| {phase} | ? | ? | ? | ? | ? |")
                    continue
                bold = "**" if phase == "Total" else ""
                lines.append(
                    f"| {bold}{phase}{bold} | {p['min']} | {p['max']} | "
                    f"{p['avg']} | {p['med']} | {p['p95']} |"
                )
            lines.append("")

    if interlaced:
        lines.append("## Interlaced")
        lines.append("")
        lines.append(f"{interlaced['frames']} frames, avg={interlaced['avg']}us "
                     f"({interlaced['fps']} fps), min={interlaced['min']}us, "
                     f"max={interlaced['max']}us")
        lines.append("")

    with open(args.out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print(f"{len(runs)} run(s), interlaced={'yes' if interlaced else 'no'} -> {args.out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
