#!/usr/bin/env python3
"""Turns a raw device self-test capture (see tools/sweeps/capture_selftest.py)
into a markdown report: a summary line, every failure with its assertion
message, and the full pass/fail list tucked into a collapsible section so the
failures are what a reader actually sees first.

Usage:
    python tools/report_test_results.py <raw_capture.txt> <out.md>
"""
import argparse
import re
import sys
from datetime import datetime, timezone

# Unity's own output shape: "<file>:<line>:<test_name>:PASS" or
# "<file>:<line>:<test_name>:FAIL: <message>". Only lines that end in
# :PASS or :FAIL(: ...) are test results - everything else in the capture
# (boot log, ESP_LOGI lines) is noise for this report.
RESULT_RE = re.compile(r"^(?P<file>\S+):(?P<line>\d+):(?P<name>\w+):(?P<status>PASS|FAIL)(?::\s*(?P<message>.*))?$")
COMPLETE_RE = re.compile(r"SELFTEST_COMPLETE failures=(\d+) elapsed_ms=(\d+)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_path", help="Raw capture from capture_selftest.py")
    parser.add_argument("out_path", help="Markdown file to write")
    args = parser.parse_args()

    with open(args.capture_path, "r", errors="replace") as f:
        text = f.read()

    results = []
    for line in text.splitlines():
        m = RESULT_RE.match(line.strip())
        if m:
            results.append(m.groupdict())

    complete = COMPLETE_RE.search(text)
    failures_reported = int(complete.group(1)) if complete else None
    elapsed_ms = int(complete.group(2)) if complete else None

    passed = [r for r in results if r["status"] == "PASS"]
    failed = [r for r in results if r["status"] == "FAIL"]

    lines = []
    lines.append("# Device Self-Test Results")
    lines.append("")
    lines.append(f"Captured: {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')}")
    lines.append(f"Source: `{args.capture_path}`")
    lines.append("")
    lines.append(f"**{len(results)} tests, {len(passed)} passed, {len(failed)} failed**"
                 + (f", {elapsed_ms} ms total" if elapsed_ms is not None else ""))
    if complete is None:
        lines.append("")
        lines.append("> **No `SELFTEST_COMPLETE` line found** - the capture may have "
                     "timed out or the device may have crashed mid-run. Treat this "
                     "report as partial.")
    elif failures_reported != len(failed):
        lines.append("")
        lines.append(f"> Device reported `failures={failures_reported}` but this report "
                     f"parsed {len(failed)} FAIL lines - a parsing mismatch, not "
                     "necessarily a device problem. Check the raw capture.")
    lines.append("")

    if failed:
        lines.append("## Failures")
        lines.append("")
        for r in failed:
            lines.append(f"- **`{r['name']}`** (`{r['file']}:{r['line']}`)")
            if r["message"]:
                lines.append(f"  {r['message']}")
        lines.append("")
    else:
        lines.append("## Failures")
        lines.append("")
        lines.append("None.")
        lines.append("")

    lines.append("<details>")
    lines.append("<summary>All results ({} tests)</summary>".format(len(results)))
    lines.append("")
    lines.append("| Test | Result |")
    lines.append("|---|---|")
    for r in results:
        mark = "PASS" if r["status"] == "PASS" else f"**FAIL** - {r['message'] or ''}"
        lines.append(f"| `{r['name']}` | {mark} |")
    lines.append("")
    lines.append("</details>")
    lines.append("")

    with open(args.out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print(f"{len(results)} tests, {len(passed)} passed, {len(failed)} failed -> {args.out_path}")
    # Deliberately not gated on a specific "expected" failure count here -
    # whether N failures is a known baseline (see docs/Sand/Architecture.md)
    # or a real regression is a judgement call for whoever reads the
    # report, not something this general-purpose parser should assume.
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
