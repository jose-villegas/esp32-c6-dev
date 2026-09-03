#!/usr/bin/env python3
"""Turns a raw device self-test capture into a markdown table of just the
DEVICE_BUILD frame-budget tests: scenario, budget, measured number, headroom,
pass/fail - the same shape as the table in docs/Sand/Architecture.md, except
generated fresh from a real capture instead of hand-transcribed (and so it
cannot go stale the way a hand-written copy can).

Reads budgets from whichever suite source is given via --source rather than
hardcoding them here, so retuning a budget in the source is immediately
reflected in the next report with no second place to update.

Usage:
    python main/apps/sand/tools/report_performance.py <raw_capture.txt> <out.md> \
        --source main/apps/<app>/suite_<app>.c

Lives under the sand app because sand's report_performance.sh is its only
caller. Nothing in the parsing is sand-specific - it keys on #ifdef
DEVICE_BUILD, Unity's TEST_ASSERT_LESS_THAN_MESSAGE and a "device_tests ...
us" log line - so --source takes any app's suite. If a second app ever grows
frame-budget tests, this belongs back in the shared tools/ and the move is a
rename plus one path. Until then, an app's folder holds the tools only that
app uses, so that deleting the app leaves nothing stranded.
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

# A separate line - not part of the result line above - emitted by
# test/timing.c for every test, on both host and device. Kept separate on
# purpose: RESULT_RE is anchored at end-of-line, so appending timing to the
# PASS/FAIL line itself would have broken it (and validate_capture.py's
# looser prefix match) rather than just adding a new thing to ignore. Its
# absence means an older capture, from before per-test timing existed - see
# make_slow_tests_section() below, which degrades to nothing rather than
# erroring on one.
#
# The trailing group is what keeps that promise for fields added since: the
# host runner appends peak_bytes= when it runs under the device-sized heap
# arena (test/heap_arena.c), and an end-anchored pattern would have silently
# matched nothing on every line rather than failing - degrading a capture to
# "no timing data" with no error to notice. New fields go on the END of that
# line, and are ignored here until something needs them.
TEST_TIME_RE = re.compile(
    r"^TEST_TIME name=(?P<name>\w+) elapsed_ms=(?P<ms>\d+)(?:\s.*)?$")

# selftest.c's own sentinel, printed once at the very end of the run - the
# one total this file cannot get by summing TEST_TIME lines, because a
# capture can be truncated (see validate_capture.py's panic handling) while
# still having logged individual test times right up to the crash.
SELFTEST_COMPLETE_RE = re.compile(r"SELFTEST_COMPLETE(?:\s+failures=(\d+)\s+elapsed_ms=(\d+))?")
# ESP_LOGI lines this project's own frame-budget tests print - always
# tagged "device_tests", always some number of microseconds. Phrasing
# after the number varies per test ("us per step", "us for the one
# step", or just "us"), so only the tag and the number are required.
#
# The LAST number on the line is the measurement, not the first. Every
# sand test logs exactly one, so for those the two rules agree. The gfx
# tests log a REFERENCE first and their own subject last ("present: full
# 18444 us, unchanged 3 us") - taking the first there would table the
# full-frame baseline as though it were the result, which is how these
# tests stayed out of the report until now. Checked against every
# device_tests line of capture performance_20260828_014644: nine lines
# carry more than one figure, all nine are gfx tests, and in all nine the
# subject is last.
MEASURE_RE = re.compile(r"device_tests.*(?<![\d.])(\d+)\s*us\b")


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


def format_duration(ms: int) -> str:
    """1234 -> "1.2s"; 462117 -> "7m 42.1s" - the ms column alone doesn't
    read at a glance across a range that spans sub-millisecond tests and an
    eight-minute one, which is the whole reason this section exists."""
    if ms < 1000:
        return f"{ms} ms"
    if ms < 60_000:
        return f"{ms / 1000:.1f}s"
    minutes, rest_ms = divmod(ms, 60_000)
    return f"{minutes}m {rest_ms / 1000:.1f}s"


def make_slow_tests_section(capture: dict, total_ms, top_n: int = 15) -> list:
    """The slowest tests by wall time, across every suite - not just the
    frame-budget ones. A test with no declared budget (the sand suite's own
    statistical vent-cap correctness test used to be exactly this: no budget
    of its own, ~7.7 minutes, and the single biggest cost in the whole run,
    until the mechanism it tested was removed along with it - bd
    esp32c6-0f2) is otherwise invisible to this report: it never enters
    `known`, and the "measured in this capture" table above only lists tests
    with a device_tests log line, which most tests don't print. This is the
    only place that ranks the whole suite.

    Returns [] when the capture predates per-test timing (test/timing.c) -
    an older raw capture still produces every other section of this report
    unchanged."""
    timed = [(name, e["elapsed_ms"]) for name, e in capture.items()
             if e["elapsed_ms"] is not None]
    if not timed:
        return []

    timed.sort(key=lambda pair: pair[1], reverse=True)

    section = []
    section.append("## Slowest Tests by Wall Time")
    section.append("")
    section.append(f"Top {min(top_n, len(timed))} of {len(timed)} timed tests.")
    section.append("")
    section.append("| Test | Elapsed | Share of run |")
    section.append("|---|---:|---:|")
    for name, ms in timed[:top_n]:
        share_s = f"{ms / total_ms * 100:.1f}%" if total_ms else "?"
        section.append(f"| `{name}` | {format_duration(ms)} | {share_s} |")
    section.append("")
    return section


def parse_capture(capture_path: str):
    with open(capture_path, "r", errors="replace") as f:
        text = f.read()

    entries = {}  # test name -> {"status", "message", "measured", "elapsed_ms"}
    test_times = {}  # test name -> elapsed_ms, from TEST_TIME lines
    pending_measure = None
    for line in text.splitlines():
        mm = MEASURE_RE.search(line)
        if mm:
            pending_measure = int(mm.group(1))
            continue
        tm = TEST_TIME_RE.match(line.strip())
        if tm:
            # Keyed by name, not by position relative to the result line -
            # timing.c always prints this after the PASS/FAIL line, but
            # nothing here should depend on that staying true.
            test_times[tm.group("name")] = int(tm.group("ms"))
            continue
        rm = RESULT_RE.match(line.strip())
        if rm and rm.group("name") not in entries:
            entries[rm.group("name")] = {
                "status": rm.group("status"),
                "message": rm.group("message"),
                "measured": pending_measure,
                "elapsed_ms": None,
            }
            pending_measure = None

    for name, ms in test_times.items():
        if name in entries:
            entries[name]["elapsed_ms"] = ms

    total_ms = None
    sm = SELFTEST_COMPLETE_RE.search(text)
    if sm and sm.group(2) is not None:
        total_ms = int(sm.group(2))

    return entries, total_ms


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_path", help="Raw capture from capture_selftest.py")
    parser.add_argument("out_path", help="Markdown file to write")
    # Required, not defaulted to any one app's suite: the parsing above keys
    # only on generic patterns (#ifdef DEVICE_BUILD, Unity's
    # TEST_ASSERT_LESS_THAN_MESSAGE, a "device_tests ... us" log line), so
    # this script lives in shared tools/ on purpose - it has no app-specific
    # knowledge to make an app the "default" caller. Its one caller
    # (main/apps/sand/tools/report_performance.sh) always passes --source
    # explicitly, so requiring it here breaks nothing.
    # Repeatable: the draw side of a frame is measured in a different suite
    # from the simulation side (suite_gfx.c vs an app's own suite_*.c), and
    # a report that shows only one of them hides half the frame. Pass
    # --source once per suite; budgets from all of them land in one table.
    parser.add_argument(
        "--source", required=True, action="append",
        help="Path to a file frame-budget tests live in (repeatable)",
    )
    args = parser.parse_args()

    budgets = {}
    for source in args.source:
        budgets.update(parse_budgets(source))
    capture, total_ms = parse_capture(args.capture_path)

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
    if total_ms is not None:
        lines.append(f"Total run time: {format_duration(total_ms)} ({total_ms} ms)")
    lines.append("")
    lines.append("| Test | Budget (us) | Measured (us) | Headroom | Status |")
    lines.append("|---|---:|---:|---:|---|")
    unmeasured = []
    for name in known:
        budget = budgets[name]
        entry = capture[name]
        measured = entry["measured"]
        status = entry["status"]
        # A row with no measurement is not a budget row. The source scan
        # matches any TEST_ASSERT_LESS_THAN_MESSAGE, which correctness tests
        # also use, so a scene test asserting "fewer than 10 of these" was
        # being rendered as a frame-budget row with budget 10, measured "?"
        # and a permanent PASS - an unfailable line in a table whose whole
        # purpose is showing what fails, and it inflated the row count too.
        if measured is None:
            unmeasured.append(name)
            continue
        if budget is None:
            budget_s, headroom_s = "?", "?"
        else:
            budget_s = str(budget)
            headroom_s = f"{(budget - measured) / budget * 100:+.1f}%" if measured else "?"
        measured_s = str(measured) if measured is not None else "?"
        mark = "PASS" if status == "PASS" else f"**FAIL**"
        lines.append(f"| `{name}` | {budget_s} | {measured_s} | {headroom_s} | {mark} |")
    lines.append("")

    if unmeasured:
        lines.append("> Matched the budget pattern in source but logged no "
                     "measurement, so they are not frame-budget rows and are "
                     "listed rather than tabled: "
                     + ", ".join(f"`{n}`" for n in unmeasured))
        lines.append("")

    if only_in_source:
        lines.append("> Declared a budget in source but did not appear in this capture "
                     "(not run, or renamed): " + ", ".join(f"`{n}`" for n in only_in_source))
        lines.append("")
    if only_in_capture:
        # Listed WITH their measurements, not just by name. Most of these are
        # suite_gfx.c's draw-path tests, which deliberately assert a RATIO
        # against a reference measured in the same run ("under a tenth of a
        # full frame", "cheaper than a whole band") rather than an absolute
        # microsecond budget - that is what makes them immune to the flash
        # layout lottery, and it is why they have no budget column to fill.
        # Their numbers still matter: a present costs as much as a step.
        lines.append("> Measured in this capture with no fixed budget declared in "
                     f"{', '.join(f'`{s}`' for s in args.source)}. The draw-path "
                     "tests in `suite_gfx.c` are here by design - they assert a "
                     "ratio against a reference measured in the same run, not an "
                     "absolute number - so read these as measurements, not as "
                     "passes or failures:")
        lines.append("")
        lines.append("| Test | Measured (us) |")
        lines.append("|---|---:|")
        for name in only_in_capture:
            m = capture[name]["measured"]
            lines.append(f"| `{name}` | {m if m is not None else '?'} |")
        lines.append("")

    lines.extend(make_slow_tests_section(capture, total_ms))

    with open(args.out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    failed = [n for n in known if capture[n]["status"] == "FAIL"]
    # len(known) minus the unmeasured ones, so this line agrees with the
    # table it is describing rather than counting rows that were dropped.
    print(f"{len(known) - len(unmeasured)} frame-budget tests, "
          f"{len(failed)} failing -> {args.out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
