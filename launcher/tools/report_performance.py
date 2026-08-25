#!/usr/bin/env python3
"""Turns a raw device self-test capture into a markdown table of just the
DEVICE_BUILD frame-budget tests: scenario, budget, measured number, headroom,
pass/fail - the same shape as the table in docs/Sand/Architecture.md, except
generated fresh from a real capture instead of hand-transcribed (and so it
cannot go stale the way a hand-written copy can).

Reads the budget for each test straight from suite_sand.c rather than
hardcoding it here, so retuning a budget in the source is immediately
reflected in the next report with no second place to update.

Usage:
    python tools/report_performance.py <raw_capture.txt> <out.md> \
        [--source main/apps/sand/suite_sand.c]
"""
import argparse
import re
import sys
from datetime import datetime, timezone

# A frame-budget test: a `static void test_...(void) { ... }` function body
# that calls TEST_ASSERT_LESS_THAN_MESSAGE somewhere inside it. Matched
# non-greedily up to the next `static void` or end of the DEVICE_BUILD
# block, which is good enough for this file's own formatting (one test
# function's closing brace per line, blank line, next function).
DEVICE_BUILD_RE = re.compile(r"#ifdef DEVICE_BUILD(.*?)#endif\s*/\*\s*DEVICE_BUILD", re.DOTALL)
FUNC_RE = re.compile(
    r"static void (test_\w+)\(void\)\s*\{(.*?)\n\}",
    re.DOTALL,
)
BUDGET_RE = re.compile(r"TEST_ASSERT_LESS_THAN_MESSAGE\(\s*(\w+)\s*,")
DEFINE_RE = re.compile(r"#define\s+(\w+)\s+(\d+)")

RESULT_RE = re.compile(r"^\S+:\d+:(?P<name>\w+):(?P<status>PASS|FAIL)(?::\s*(?P<message>.*))?$")
# ESP_LOGI lines this project's own frame-budget tests print - always
# tagged "device_tests", always some number of microseconds. Phrasing
# after the number varies per test ("us per step", "us for the one
# step", or just "us"), so only the tag and the number are required.
MEASURE_RE = re.compile(r"device_tests.*?(\d+)\s*us\b")


def parse_budgets(source_path: str) -> dict:
    with open(source_path, "r", errors="replace") as f:
        text = f.read()

    defines = {name: int(value) for name, value in DEFINE_RE.findall(text)}

    # Only functions inside a #ifdef DEVICE_BUILD ... #endif /* DEVICE_BUILD */
    # block - a handful of ordinary host-portable tests elsewhere in this
    # file also happen to use TEST_ASSERT_LESS_THAN_MESSAGE for a non-timing
    # numeric check (an angle, a height), and are not frame-budget tests.
    device_build_text = "\n".join(DEVICE_BUILD_RE.findall(text))

    budgets = {}
    for name, body in FUNC_RE.findall(device_build_text):
        m = BUDGET_RE.search(body)
        if not m:
            continue
        token = m.group(1)
        if token.isdigit():
            budgets[name] = int(token)
        elif token in defines:
            budgets[name] = defines[token]
        else:
            budgets[name] = None  # unresolved macro - report it as unknown rather than guess
    return budgets


def parse_capture(capture_path: str):
    with open(capture_path, "r", errors="replace") as f:
        text = f.read()

    entries = {}  # test name -> {"status", "message", "measured"}
    pending_measure = None
    for line in text.splitlines():
        mm = MEASURE_RE.search(line)
        if mm:
            pending_measure = int(mm.group(1))
            continue
        rm = RESULT_RE.match(line.strip())
        if rm and rm.group("name") not in entries:
            entries[rm.group("name")] = {
                "status": rm.group("status"),
                "message": rm.group("message"),
                "measured": pending_measure,
            }
            pending_measure = None
    return entries


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_path", help="Raw capture from capture_selftest.py")
    parser.add_argument("out_path", help="Markdown file to write")
    parser.add_argument(
        "--source", default="main/apps/sand/suite_sand.c",
        help="Path to the file the frame-budget tests live in (default: %(default)s)",
    )
    args = parser.parse_args()

    budgets = parse_budgets(args.source)
    capture = parse_capture(args.capture_path)

    # Only tests that both declare a budget AND actually ran this capture -
    # a test present in one but not the other is worth surfacing, not
    # silently dropping.
    known = sorted(set(budgets) & set(capture))
    only_in_source = sorted(set(budgets) - set(capture))
    only_in_capture = sorted(name for name in set(capture) - set(budgets)
                             if capture[name]["measured"] is not None)

    lines = []
    lines.append("# Device Performance Report")
    lines.append("")
    lines.append(f"Captured: {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')}")
    lines.append(f"Source: `{args.capture_path}`, budgets from `{args.source}`")
    lines.append("")
    lines.append("| Test | Budget (us) | Measured (us) | Headroom | Status |")
    lines.append("|---|---:|---:|---:|---|")
    for name in known:
        budget = budgets[name]
        entry = capture[name]
        measured = entry["measured"]
        status = entry["status"]
        if budget is None:
            budget_s, headroom_s = "?", "?"
        else:
            budget_s = str(budget)
            headroom_s = f"{(budget - measured) / budget * 100:+.1f}%" if measured else "?"
        measured_s = str(measured) if measured is not None else "?"
        mark = "PASS" if status == "PASS" else f"**FAIL**"
        lines.append(f"| `{name}` | {budget_s} | {measured_s} | {headroom_s} | {mark} |")
    lines.append("")

    if only_in_source:
        lines.append("> Declared a budget in source but did not appear in this capture "
                     "(not run, or renamed): " + ", ".join(f"`{n}`" for n in only_in_source))
        lines.append("")
    if only_in_capture:
        lines.append("> Printed a measurement in this capture but declared no budget in "
                     f"`{args.source}` - likely a perf test that lives in a different "
                     "source file (pass `--source` to point at it), not necessarily a "
                     "problem: " + ", ".join(f"`{n}`" for n in only_in_capture))
        lines.append("")

    with open(args.out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    failed = [n for n in known if capture[n]["status"] == "FAIL"]
    print(f"{len(known)} frame-budget tests, {len(failed)} failing -> {args.out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
