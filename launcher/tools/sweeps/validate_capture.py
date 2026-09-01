#!/usr/bin/env python3
"""Answers exactly one question about a raw device self-test capture: is it
worth reading at all?

A long sand performance-optimisation session wasted repeated cycles acting
on captures that looked plausible but measured nothing - a timeout with no
SELFTEST_COMPLETE, a crash loop, or (worst, because it produced a clean-
looking report) an image where the suites never actually ran and the device
just sat in the launcher printing its idle frame rate. Each of those burned
a full build+flash+capture cycle before anyone noticed. This tool runs
straight after the capture step and before report_performance.py, so a
worthless capture is rejected with a specific reason instead of turning into
a plausible-looking table.

Deliberately read-only: it never modifies, filters or deletes the raw
capture. Stripping a diagnostic before a human reads it is exactly how the
one explaining line goes missing - this only reports what it sees.

Usage:
    python validate_capture.py <raw_capture.txt>
    python validate_capture.py --selftest

Exit 0 = valid, non-zero = invalid (one or more checks below failed).
"""
import argparse
import contextlib
import io
import os
import re
import sys

# The device prints this line only when the self-test loop actually reaches
# its end - absent means the run never finished, for any reason (timeout,
# device wedged, serial dropped).
SELFTEST_COMPLETE_RE = re.compile(r"SELFTEST_COMPLETE(?:\s+failures=(\d+)\s+elapsed_ms=(\d+))?")

# Both phrases appear on ESP-IDF's panic banner; either is sufficient to
# call it a crash. The parenthesised text after "panic'ed" is the exception
# type (e.g. "Stack protection fault") and is worth surfacing verbatim -
# it is usually enough on its own to point at the offending change.
PANIC_LINE_RE = re.compile(r"Guru Meditation|panic'ed")
PANIC_TYPE_RE = re.compile(r"panic'ed\s*\(([^)]+)\)")

# One per boot. More than one means the device reset mid-run - a crash
# loop, not a slow run - which changes what a stall in the capture means.
BOOT_BANNER = "ESP-ROM:esp32c6"

# Printed once per frame-budget test as it starts. Its absence, with an
# otherwise unremarkable capture, means the flashed image had the suites
# compiled in but not running (CONFIG_LAUNCHER_SELFTEST_AUTORUN off, or
# plain wrong image) - the device just sat in the launcher for the whole
# capture window. This exact failure burned two full capture cycles before
# anyone thought to check for it specifically.
SENTINEL = "device_tests: sand_step on"

# A line matching this is a Unity test result - used only to find the last
# few tests that ran before a panic, which is how the crashing test gets
# identified without a second capture.
RESULT_RE = re.compile(r"^\S+:\d+:(?P<name>\w+):(?P<status>PASS|FAIL)")

# Not fatal by itself, but its presence means an old diag image: the
# current build disables the task watchdog on purpose, because historically
# a watchdog dump landed inside a timed frame-budget window and inflated
# that one row's measurement by up to 2.6x. A stale image silently
# reintroduces that noise.
TASK_WDT_MARKER = "task_wdt"

CONTEXT_LINES = 5  # how many prior test results to show before a panic


def _panic_context(lines, panic_index):
    """Last few Unity result lines before the first panic - the crashing
    test is usually the very last PASS before the dump starts."""
    context = []
    for line in reversed(lines[:panic_index]):
        if RESULT_RE.match(line.strip()):
            context.append(line.rstrip("\n"))
            if len(context) >= CONTEXT_LINES:
                break
    return list(reversed(context))


def validate(capture_path: str):
    """Returns (failures, warnings) - both lists of message strings.
    Empty failures means the capture is valid."""
    with open(capture_path, "r", errors="replace") as f:
        text = f.read()
    lines = text.splitlines()

    failures = []
    warnings = []

    m = SELFTEST_COMPLETE_RE.search(text)
    if not m:
        failures.append(
            "SELFTEST_COMPLETE not found - the run never finished: either it "
            "timed out or the device stopped talking mid-capture."
        )

    panic_index = None
    panic_type = None
    for i, line in enumerate(lines):
        if PANIC_LINE_RE.search(line):
            panic_index = i
            tm = PANIC_TYPE_RE.search(line)
            panic_type = tm.group(1) if tm else line.strip()
            break
    if panic_index is not None:
        msg = f"panic detected: {panic_type}"
        context = _panic_context(lines, panic_index)
        if context:
            msg += "\n    last test results before the panic:\n      " + \
                "\n      ".join(context)
        failures.append(msg)

    boot_count = text.count(BOOT_BANNER)
    if boot_count == 0:
        failures.append(
            f"no boot banner ({BOOT_BANNER!r}) found - this doesn't look like "
            "a capture that started from a device reset at all."
        )
    elif boot_count > 1:
        failures.append(
            f"{boot_count} boot banners found, expected 1 - the device "
            "rebooted mid-run. That means a crash loop, not a slow run."
        )

    if SENTINEL not in text:
        failures.append(
            f"measurement sentinel {SENTINEL!r} not found - the flashed image "
            "either had the suites compiled in but not running (autorun off), "
            "or was the wrong image entirely. The device sat in the launcher "
            "for the whole capture window instead of running any test."
        )

    wdt_count = sum(1 for line in lines if TASK_WDT_MARKER in line)
    if wdt_count:
        warnings.append(
            f"{wdt_count} line(s) mention {TASK_WDT_MARKER!r} - the current "
            "diag image disables the task watchdog on purpose, so this looks "
            "like an old image. Historically a watchdog dump landed inside a "
            "timed window and inflated that row's measurement up to 2.6x - "
            "treat any single-row spike in this capture with suspicion."
        )

    return failures, warnings


def report(capture_path: str) -> bool:
    failures, warnings = validate(capture_path)
    valid = not failures
    print(f"{'VALID' if valid else 'INVALID'}: {capture_path}")
    for msg in failures:
        print(f"  [FAIL] {msg}")
    for msg in warnings:
        print(f"  [WARN] {msg}")
    if valid:
        with open(capture_path, "r", errors="replace") as f:
            m = SELFTEST_COMPLETE_RE.search(f.read())
        if m and m.group(1) is not None:
            print(f"  SELFTEST_COMPLETE failures={m.group(1)} elapsed_ms={m.group(2)}")
    return valid


# --- self-check against real captures on disk ------------------------------
#
# These are actual captures from the campaign, one per failure mode this
# tool exists to catch. Run with --selftest. If a listed file is missing or
# doesn't behave as described here, that's this tool disagreeing with
# reality and should be reported as such, not quietly special-cased.
SELFTEST_FIXTURES_DIR = os.path.normpath(
    os.path.join(os.path.dirname(__file__), "..", "..", "main", "apps", "sand", "tools", "results")
)
SELFTEST_CASES = [
    # (filename, expected valid, one substring that must appear in the output)
    ("performance_20260828_063605_raw.txt", True, "failures=18"),
    ("performance_20260831_144804_raw.txt", False, "sand_step on"),
    ("performance_20260831_151205_raw.txt", False, "Stack protection fault"),
    ("performance_20260828_062637_raw.txt", False, "SELFTEST_COMPLETE not found"),
]


def run_selftest() -> int:
    ok = True
    for filename, expect_valid, expect_substring in SELFTEST_CASES:
        path = os.path.join(SELFTEST_FIXTURES_DIR, filename)
        print(f"--- {filename} ---")
        if not os.path.isfile(path):
            print(f"  MISSING FIXTURE: {path}")
            ok = False
            continue

        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            valid = report(path)
        output = buf.getvalue()
        print(output, end="")

        case_ok = True
        if valid != expect_valid:
            print(f"  CHECK FAILED: expected valid={expect_valid}, got valid={valid}")
            case_ok = False
        if expect_substring not in output:
            print(f"  CHECK FAILED: expected {expect_substring!r} to appear in the output above")
            case_ok = False
        if case_ok:
            print(f"  CHECK OK (valid={valid})")
        ok = ok and case_ok
        print()
    print("SELFTEST PASSED" if ok else "SELFTEST FAILED")
    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("capture_path", nargs="?", help="Raw capture from capture_selftest.py")
    parser.add_argument("--selftest", action="store_true",
                         help="Run against known-good/known-bad captures in "
                              "main/apps/sand/tools/results/ and report the result")
    args = parser.parse_args()

    if args.selftest:
        return run_selftest()

    if not args.capture_path:
        parser.error("capture_path is required unless --selftest is given")

    return 0 if report(args.capture_path) else 1


if __name__ == "__main__":
    sys.exit(main())
