#!/usr/bin/env python3
"""Static stack-frame gate for test-code translation units.

Reads the .su files GCC/Clang emit under -fstack-usage (one per translation
unit, one line per function: "file:line:col:function<TAB>bytes<TAB>qualifier")
and fails if any function's frame exceeds the device profile's ceiling.

Why this exists: two device panics in this project's history were "Stack
protection fault" loops, both caused by a test fixture declaring a huge local
array - a 4 KB comparison buffer, and later an impulse_t[4096] (24 KB). Both
passed green on the host, whose stack is megabytes; the ESP32-C6's main task
stack (CONFIG_ESP_MAIN_TASK_STACK_SIZE) is 3,584 bytes, shared with Unity,
printf, and the call chain above the fixture. See docs/Sand/
Performance-Tuning-Attempts.md, "Recurring failure modes". The host build
cannot reproduce a stack panic - it can only predict one, statically, from
the frame sizes GCC/Clang already compute for their own prologues. This gate
is that prediction, run every time the host suite runs.

The ceiling (DP_TEST_FRAME_CEILING_BYTES, see launcher/tools/device_profiles/
esp32c6.sh) is 1024 bytes: comfortably below the 3,584-byte device stack
(generous margin given that stack is shared with Unity and the interpreter
chain above a fixture, not just the fixture's own frame), yet it catches
both historical panics (24 KB and 4 KB) with two orders of magnitude to
spare. Measured against the tree on 2026-09-03, the largest frame that
clears the ceiling today is 864 bytes
(test_a_direction_flip_does_not_corrupt_the_boundary_debounce, suite_sand.c)
- so 1024 is not starving anything real, it is just below where the next
genuine outlier would have to be caught.

Seven functions already exceed it (PRE_EXISTING_STACK_DEBT below), now
ranging 1,088-1,792 bytes. The reason they are tracked individually rather
than absorbed by a higher ceiling is what the worst of them turned out to
be: an on-stack `unsigned depth[92 * 112]`, 42,848 bytes, nearly twelve
times the whole device stack, the same species of bug as both historical
panics. Raising the ceiling to fit what already existed would have hidden
it; listing each frame instead surfaced it on the gate's first run (bd
esp32c6-3h9, fixed in 4a17e07).

This gate is only worth anything if a frame that fits on x86 cannot secretly
be larger on the target, so that was measured rather than assumed. Compiling
the same suites with the ESP RISC-V toolchain and the device profile's own
flags (check_stack_usage_device.sh, which runs this same checker over the
target's .su files) gave, across the 411 functions of suite_sand.c present
in both builds: not one larger on RISC-V than on x86, median 0.40x, worst
case 0.99x, and no function over the ceiling on device that was not also
over it here. The host over-estimates - the safe direction for a gate to be
wrong in. Re-run that script after a toolchain or -O-level change, which is
the sort of thing that could invert it.

The profile is the source of truth for the ceiling and the device stack size
- neither number is hardcoded here. See launcher/tools/device_profile.py.

    launcher/test/check_stack_usage.py <su-dir> [--ceiling N] [--profile NAME]
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
import device_profile  # noqa: E402  (path must be set up first)


# Pre-existing stack-frame debt against the 3,584-byte device stack, measured
# once (esp32c6 profile, 2026-09-03) so the gate can land without either
# raising the ceiling to match the worst of them or leaving every one of
# them permanently red. This is NOT a permission slip: each value is the
# frame size measured when the entry was recorded, and a function only
# leaves this list by shrinking its frame below the ceiling - never by
# bumping the recorded number to match a regrowth. A function that grows
# meaningfully past its recorded size fails exactly like a brand-new
# offender would.
#
# "Meaningfully" because these are HOST frames, and host compilers disagree
# with each other about them: the same test_the_blend_has_no_jump_crossing_45_degrees
# measured 1,216 bytes on the Windows MinGW gcc the list was recorded with
# and 1,456 bytes (+19.7%) on the Linux gcc in CI, for identical source.
# So a recorded value is compared with STACK_FRAME_TOOLCHAIN_TOLERANCE of
# headroom rather than exactly - enough to absorb one compiler's opinion of
# another's frame, nowhere near enough to hide the class this gate exists
# for (the three historical offenders were 4 KB, 24 KB and 41 KB against a
# ~1.5 KB record). The device's own frames, which are what actually matter,
# are checked separately and exactly by check_stack_usage_device.sh.
STACK_FRAME_TOOLCHAIN_TOLERANCE = 0.25
#
# Keyed by (source file basename, function name) rather than a full path, so
# this survives being checked out to a different absolute location.
#
# A frame that shrinks but stays over the ceiling gets its entry RE-RECORDED
# at the lower number, which is tightening, not loosening - leaving the old
# value there would licence it to grow all the way back. That has already
# happened once: this list's worst entry was
# test_a_submerged_obstacle_casts_a_gravity_aligned_shadow at 42,848 bytes,
# an on-stack `unsigned depth[92 * 112]` and a genuine device risk (bd
# esp32c6-3h9, the first thing this gate caught). It was moved to the heap
# in 4a17e07 and now measures 1,632.
PRE_EXISTING_STACK_DEBT = {
    ("suite_sand.c",
     "test_a_submerged_obstacle_casts_a_gravity_aligned_shadow"): 1632,
    ("suite_sand.c",
     "test_a_sparse_repaint_does_not_band_a_tall_liquid_column"): 1792,
    ("suite_sand.c",
     "test_oil_dilutes_into_acid_but_the_acid_pays_for_it"): 1696,
    ("suite_sand.c",
     "test_a_settled_edge_does_not_flicker_stale_to_fresh"): 1488,
    ("suite_sand.c",
     "test_the_blend_has_no_jump_crossing_45_degrees"): 1216,
    ("suite_sand.c",
     "test_axis_lock_tremor_does_not_wipe_the_depth_debounce"): 1136,
    ("suite_sand.c",
     "test_turning_a_settled_pool_to_landscape_does_not_flash_the_whole_body"):
        1088,
}


class StackUsageRecord(object):
    __slots__ = ("path", "line", "func", "bytes", "qualifier")

    def __init__(self, path, line, func, nbytes, qualifier):
        self.path = path
        self.line = line
        self.func = func
        self.bytes = nbytes
        self.qualifier = qualifier


def parse_su_file(path):
    """Yield one StackUsageRecord per line of a single .su file.

    A line looks like:
        C:/repo/launcher/test/suites/suite_rng.c:16:13:test_foo	48	static

    The location field is itself colon-separated (file:line:col:function),
    and on Windows the file half already contains a drive-letter colon - so
    splitting from the right, a fixed three fields at a time, is the only
    split that is safe on every platform this runs on.
    """
    records = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if not line:
                continue
            fields = line.split("\t")
            if len(fields) < 2:
                raise ValueError("%s: malformed .su line (expected at least "
                                 "2 tab-separated fields): %r" % (path, line))
            loc_and_func = fields[0]
            try:
                src_path, src_line, _col, func = loc_and_func.rsplit(":", 3)
            except ValueError:
                raise ValueError("%s: malformed location field: %r"
                                 % (path, loc_and_func))
            try:
                nbytes = int(fields[1])
            except ValueError:
                raise ValueError("%s: non-numeric byte count: %r"
                                 % (path, fields[1]))
            qualifier = fields[2] if len(fields) > 2 else ""
            records.append(StackUsageRecord(src_path, src_line, func, nbytes,
                                             qualifier))
    return records


def find_su_files(su_dir):
    found = []
    for root, _dirs, files in os.walk(su_dir):
        for name in files:
            if name.endswith(".su"):
                found.append(os.path.join(root, name))
    return sorted(found)


def main(argv):
    parser = argparse.ArgumentParser(
        description="Fail if any test-code function's stack frame exceeds "
                    "the device profile's ceiling. The device profile is "
                    "the source of truth for both numbers this prints - "
                    "nothing here is a literal; see DP_TEST_FRAME_CEILING_"
                    "BYTES and DP_MAIN_TASK_STACK_BYTES in "
                    "launcher/tools/device_profiles/<profile>.sh.")
    parser.add_argument("su_dir",
                        help="directory to search recursively for .su "
                             "files (as produced by -fstack-usage)")
    parser.add_argument("--profile", default=None,
                        help="device profile name (default: $DEVICE_PROFILE "
                             "or esp32c6)")
    parser.add_argument("--profile-dir", default=None,
                        help="override the directory profiles are read "
                             "from (default: launcher/tools/device_profiles)")
    parser.add_argument("--ceiling", type=int, default=None,
                        help="override the per-function byte ceiling for "
                             "one-off experiments. The profile stays the "
                             "source of truth for normal runs - do not wire "
                             "this into a script that always runs.")
    args = parser.parse_args(argv)

    try:
        profile = device_profile.load(args.profile, args.profile_dir)
        stack_bytes = device_profile.require(profile,
                                              "DP_MAIN_TASK_STACK_BYTES", int)
        if args.ceiling is not None:
            ceiling = args.ceiling
        else:
            ceiling = device_profile.require(
                profile, "DP_TEST_FRAME_CEILING_BYTES", int)
    except device_profile.ProfileError as exc:
        print("check_stack_usage: %s" % exc, file=sys.stderr)
        return 1

    su_files = find_su_files(args.su_dir)
    if not su_files:
        print("check_stack_usage: no .su files found under %r - the "
              "compile pass that should have produced them either did not "
              "run or the compiler does not support -fstack-usage. A gate "
              "that silently finds nothing to check is worse than no gate; "
              "refusing to pass." % args.su_dir, file=sys.stderr)
        return 1

    records = []
    for su_path in su_files:
        try:
            records.extend(parse_su_file(su_path))
        except ValueError as exc:
            print("check_stack_usage: %s" % exc, file=sys.stderr)
            return 1

    if not records:
        print("check_stack_usage: %d .su file(s) found under %r but none "
              "contained a single function record - refusing to pass."
              % (len(su_files), args.su_dir), file=sys.stderr)
        return 1

    records.sort(key=lambda r: r.bytes, reverse=True)
    offenders = [r for r in records if r.bytes > ceiling]

    # Split what is over the ceiling into: brand-new (fails), grown past its
    # recorded debt (fails - the allowlist caps a frame, it does not exempt
    # it from ever growing further), and known debt within its recorded size
    # (does not fail the build, but is never silent - see the summary line).
    new_offenders = []
    grown_offenders = []
    known_debt = []
    for r in offenders:
        key = (os.path.basename(r.path), r.func)
        recorded = PRE_EXISTING_STACK_DEBT.get(key)
        if recorded is None:
            new_offenders.append(r)
        elif r.bytes > recorded * (1.0 + STACK_FRAME_TOOLCHAIN_TOLERANCE):
            grown_offenders.append((r, recorded))
        else:
            known_debt.append(r)

    if new_offenders or grown_offenders:
        print("check_stack_usage: %d function(s) exceed the %d-byte frame "
              "ceiling (profile %s):" %
              (len(new_offenders) + len(grown_offenders), ceiling,
               profile.get("DP_PROFILE_NAME", "?")))
        for r in new_offenders:
            print("  NEW    %s:%s: %s() - %d bytes" %
                 (r.path, r.line, r.func, r.bytes))
        for r, recorded in grown_offenders:
            print("  GREW   %s:%s: %s() - %d bytes (allowlisted at %d, now "
                  "over that too)" %
                 (r.path, r.line, r.func, r.bytes, recorded))
        if known_debt:
            print("  (%d pre-existing allowlisted frame(s) also still over "
                  "the ceiling, unchanged - not what failed this run)"
                  % len(known_debt))
        print("")
        print("why this matters: the device's main task stack is only %d "
              "bytes (CONFIG_ESP_MAIN_TASK_STACK_SIZE), shared with Unity "
              "and printf - a fixture this large panic-loops the board "
              "instead of failing a test. See this file's header and "
              "docs/Sand/Performance-Tuning-Attempts.md." % stack_bytes)
        return 1

    largest = records[0]
    if known_debt:
        worst_debt = max(known_debt, key=lambda r: r.bytes)
        debt_note = ("; %d pre-existing frame(s) allowlisted as known debt, "
                     "worst %s() at %d bytes (%.0f%% of the device stack)"
                     % (len(known_debt), worst_debt.func, worst_debt.bytes,
                        100.0 * worst_debt.bytes / stack_bytes))
    else:
        debt_note = ""
    print("check_stack_usage: %d function(s) checked across %d "
          "translation unit(s), largest frame %d bytes (%s, ceiling %d) - "
          "device stack is %d bytes%s" %
          (len(records), len(su_files), largest.bytes, largest.func,
           ceiling, stack_bytes, debt_note))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
