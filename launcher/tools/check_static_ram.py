#!/usr/bin/env python3
"""Link-time static-RAM gate: does the framebuffer, then one sand grid,
still fit after this build's .data/.bss?

    python3 launcher/tools/check_static_ram.py --map <build>/launcher.map
    python3 launcher/tools/check_static_ram.py --self-test

Wired into launcher/CMakeLists.txt as a POST_BUILD step on the ELF target,
so it runs for every variant (release, dev, diag) on every `idf.py build`,
on a laptop, before anything is flashed.

WHY THIS EXISTS (beads esp32c6-14a)

DIRAM is one pool behind .data/.bss AND the heap (see docs/notes/
Board-and-Memory.md, "Static growth taxes the heap too"). The single
368x448x2 framebuffer (gfx_init(), launcher/main/gfx/gfx.c) must be one
contiguous DMA allocation at boot, and the sand app's real-size grid (and
the real-size fixtures in main/apps/sand/suite_sand_*.c) then need one
contiguous 41,216-byte block after it. Twice, a static test buffer grew
until the framebuffer - or the grid - no longer fit, and nothing caught it
at build time: the host suite stayed green (it has megabytes of stack and
gigabytes of heap), the firmware still linked and flashed, and the failure
only showed up as a boot hang or dozens of on-device "Expected Non-NULL"
failures. This script moves that failure to link time, on a laptop, naming
the largest offenders.

This is a PREDICTION, not a reproduction. It has no visibility into runtime
allocation order, so both calibrated constants below are pegged to the
worst point actually measured on a real board, not to true fragmentation
(which, measured directly from a heap block map, is 12 bytes - see
BOOT_ALLOCATIONS_AND_SEPARATE_REGION_BYTES below). Expect about +-1 KiB of
slop from the two calibrated constants, and the messages say "predicted".

THE ARITHMETIC

  APP_USABLE_DRAM_END = 0x4087c610. From ESP-IDF's components/heap/port/
  esp32c6/memory_layout.c: SOC_ROM_STACK_START (0x4087e610) minus
  SOC_ROM_STACK_SIZE (0x2000). The main heap region runs from the linker
  symbol _heap_start up to this address, in one contiguous piece. (An
  additional ~11 KiB above it is added to the heap later at startup as a
  SEPARATE region and never helps a single large allocation - it is folded
  into BOOT_ALLOCATIONS_AND_SEPARATE_REGION_BYTES below, not ignored.)

    usable_heap = APP_USABLE_DRAM_END - _heap_start

  _heap_start is read straight out of the map file - it already reflects
  this build's .data/.bss (and .sdata/.sbss), so it is the one number this
  script needs the linker to have computed, not recompute itself.

  FRAMEBUFFER_BYTES = 368 * 448 * 2 = 329,728 (launcher/main/gfx/gfx.c,
  gfx_init()).

  PRE_FRAMEBUFFER_OVERHEAD_BYTES = 10,760. Measured 2026-09-06 on a
  development image via a boot heap trace (heap_caps_get_free_size(
  MALLOC_CAP_DMA) at each phase): usable heap 414,096, "before framebuffer"
  free 403,336 -> 414,096 - 403,336 = 10,760 bytes taken by task stacks,
  drivers and the SD probe before gfx_init() runs. This covers gate 1 only
  (see below) - it is measured at the one point in boot where the
  framebuffer does not exist yet, so it cannot also describe what happens
  afterward. Re-peg by flashing a dev image and reading the "HEAPMARK
  before framebuffer" line against the usable heap.

  BOOT_ALLOCATIONS_AND_SEPARATE_REGION_BYTES = 40,336. Same 2026-09-06
  capture, but at "shell ready" - after touch_start(), buttons_start(),
  screenshot_start(), imu_init(), display_init() and ui_launcher_init()
  have all run, which is the actual moment the sand app's later grid
  allocation has to compete with, not gfx_init time: measured shell-ready
  largest contiguous block was 44,032, so 414,096 - 329,728 (framebuffer) -
  44,032 = 40,336 bytes accounts for everything else - every allocation
  from boot through a fully-up shell, AND the ~11 KiB separate region
  above APP_USABLE_DRAM_END that heap_init hands back but that can never
  serve a large allocation (see APP_USABLE_DRAM_END above). Real
  fragmentation inside the main heap region, measured directly from a heap
  block map at the framebuffer moment, was 12 bytes - not the ~20 KiB this
  constant used to be called "fragmentation" for (beads esp32c6-8h2,
  closed as disproved). Re-peg by flashing a dev image and reading the
  "HEAPMARK shell ready" line's largest-block figure against the usable
  heap and FRAMEBUFFER_BYTES.

  GRID_BYTES = 41,216 = 184 * 224: the sand app's grid at the real screen
  size (main/apps/sand/app_sand.c) and the STRESS_W*STRESS_H fixture of
  the real-size tests in main/apps/sand/suite_sand_locality.c.

    predicted_largest_before_fb = usable_heap - PRE_FRAMEBUFFER_OVERHEAD_BYTES
    predicted_largest_after_fb  = usable_heap - FRAMEBUFFER_BYTES
                                   - BOOT_ALLOCATIONS_AND_SEPARATE_REGION_BYTES

  (predicted_largest_after_fb is NOT predicted_largest_before_fb minus the
  framebuffer - it is pegged independently, straight off the shell-ready
  measurement, because the overhead that matters for each gate is measured
  at a different moment in boot.)

TWO GATES, applied to every variant (the numbers differ only in how much
headroom is left):

  1. predicted_largest_before_fb >= FRAMEBUFFER_BYTES + FRAMEBUFFER_-
     SLACK_BYTES  -  the framebuffer itself must fit, with a little slack.
  2. predicted_largest_after_fb  >= GRID_BYTES  -  one real-size grid must
     still fit once the shell is up. Zero margin here on purpose: this is
     exactly the number that failed on-device twice.

If a build fails here: shrink or malloc-on-use the largest offenders
printed below (see docs/notes/Optimization-Playbook.md, "Test and debug
code shares your production memory budget"), or, if boot allocations
genuinely changed on purpose, re-peg PRE_FRAMEBUFFER_OVERHEAD_BYTES /
BOOT_ALLOCATIONS_AND_SEPARATE_REGION_BYTES from a fresh boot log as
described above and update this header comment's provenance alongside the
constant.
"""

import argparse
import re
import sys
from pathlib import Path

# --- calibrated constants (see header comment for provenance) -------------

APP_USABLE_DRAM_END = 0x4087C610
FRAMEBUFFER_BYTES = 368 * 448 * 2
PRE_FRAMEBUFFER_OVERHEAD_BYTES = 10760
BOOT_ALLOCATIONS_AND_SEPARATE_REGION_BYTES = 40336

# The same accounting one boot phase earlier, for a SELFTEST image. Its
# grids are allocated by the suites, which run in selftest_run() straight
# after POST - before the boot animation, and before touch, buttons,
# screenshot, the IMU, the display and the launcher's UI have taken their
# share. Measured in the same 2026-09-06 capture, at "HEAPMARK after post":
# largest 50,176, so 414,096 - 329,728 - 50,176 = 34,192.
#
# Gating a selftest image at the shell-ready figure instead would ask it a
# question its own tests never face, and would fail the one build the
# device test workflow depends on. The trade is deliberate and has a
# backstop: a selftest image is also a development image, so a person CAN
# open the sand app on it interactively, and that allocation happens at
# shell ready with about 6 KiB less room than this gate guarantees. POST's
# own memory check on the device is what catches that case.
SELFTEST_BOOT_ALLOCATIONS_BYTES = 34192
GRID_BYTES = 184 * 224
FRAMEBUFFER_SLACK_BYTES = 4096

TOP_SYMBOLS_SHOWN = 12
TOP_OBJECTS_SHOWN = 8


# --- pure arithmetic (no file I/O - this is what --self-test exercises) ---

def compute_gates(heap_start, dram_data_size, dram_bss_size, selftest=False):
    """Everything the gate decides, as a function of three numbers off the
    map file. Kept separate from parsing so --self-test can hit the math
    directly without a synthetic map for every boundary case.

    `selftest` moves gate 2 to the boot phase where THAT image allocates
    its grids - see SELFTEST_BOOT_ALLOCATIONS_BYTES for which phase and
    why it is a different one."""
    usable_heap = APP_USABLE_DRAM_END - heap_start
    overhead = (SELFTEST_BOOT_ALLOCATIONS_BYTES if selftest
                else BOOT_ALLOCATIONS_AND_SEPARATE_REGION_BYTES)
    predicted_before = usable_heap - PRE_FRAMEBUFFER_OVERHEAD_BYTES
    predicted_after = usable_heap - FRAMEBUFFER_BYTES - overhead
    gate1_threshold = FRAMEBUFFER_BYTES + FRAMEBUFFER_SLACK_BYTES
    gate2_threshold = GRID_BYTES
    return {
        "dram_data_size": dram_data_size,
        "dram_bss_size": dram_bss_size,
        "heap_start": heap_start,
        "usable_heap": usable_heap,
        "selftest": selftest,
        "overhead": overhead,
        "predicted_before": predicted_before,
        "predicted_after": predicted_after,
        "gate1_pass": predicted_before >= gate1_threshold,
        "gate1_threshold": gate1_threshold,
        "gate2_pass": predicted_after >= gate2_threshold,
        "gate2_threshold": gate2_threshold,
    }


# Which image this is, read off the map itself rather than passed in from
# CMake, so running this by hand on any map file gives the verdict the build
# got. The test is whether a SUITE actually contributed static data: the
# mere presence of libunity.a proves nothing, because unity is listed
# unconditionally in REQUIRES (see launcher/main/CMakeLists.txt's own
# comment on why) and so appears in the link of every variant - with
# nothing referencing it, --gc-sections drops it and it owns no .data or
# .bss. A suite_*.c.obj with bytes to its name only exists when the suites
# were compiled in.
_SELFTEST_OBJECT_RE = re.compile(r"suite_[A-Za-z0-9_]*\.c\.obj|libunity\.a\(")


def parsed_is_selftest(parsed):
    return any(_SELFTEST_OBJECT_RE.search(raw_object)
               for _section, _name, _size, raw_object in parsed.entries)


# --- map-file parsing -------------------------------------------------------

# Output-section header at column 0, e.g.:
#   .dram0.data     0x40810420     0x2058
_SECTION_HEADER_RE = re.compile(r"^\.(\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)")

# The _heap_start assignment, wherever it falls (its own tiny output
# section, .dram0.heap_start): a lone address then the symbol name.
_HEAP_START_RE = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+_heap_start\b")

# One input-section entry, all on one line (short section name):
#   .bss.tilt      0x40816ac4       0x14 esp-idf/main/libmain.a(app_sand.c.obj)
_INLINE_ENTRY_RE = re.compile(
    r"^\s+(\.\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*\S)\s*$"
)

# The same entry split across two lines when the section name is long:
#   .bss.row_has_shine
#                  0x408169e4       0xe0 esp-idf/main/libmain.a(app_sand.c.obj)
_NAME_ONLY_RE = re.compile(r"^\s+(\.\S+)\s*$")
_ADDR_SIZE_OBJ_RE = re.compile(
    r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*\S)\s*$"
)

_FILL_RE = re.compile(r"^\s*\*fill\*")

# esp-idf/<component>/lib<name>.a(<objfile>) - used to shorten a very long
# toolchain path down to something a terminal-width report can show.
_ARCHIVE_MEMBER_RE = re.compile(r"([^/\\()]+)/lib[^/\\()]+\.a\(([^()]+)\)\s*$")


class ParsedMap(object):
    __slots__ = ("dram_data_size", "dram_bss_size", "heap_start", "entries")

    def __init__(self):
        self.dram_data_size = None
        self.dram_bss_size = None
        self.heap_start = None
        # list of (section, symbol_name, size_bytes, raw_object)
        self.entries = []


def shorten_object(raw_object):
    """'esp-idf/main/libmain.a(app_sand.c.obj)' -> 'app_sand.c.obj'; a
    component other than the project's own 'main' is kept as a prefix
    ('esp-idf/freertos/libfreertos.a(port.c.obj)' -> 'freertos port.c.obj')
    since object-file names are not unique across components."""
    raw_object = raw_object.strip()
    m = _ARCHIVE_MEMBER_RE.search(raw_object.replace("\\", "/"))
    if not m:
        # Not an archive member (e.g. a plain .o) - just the basename.
        return raw_object.replace("\\", "/").rsplit("/", 1)[-1]
    component, objfile = m.group(1), m.group(2)
    if component == "main":
        return objfile
    return "%s %s" % (component, objfile)


def parse_map(text):
    """Parse a GNU ld map file's .dram0.data / .dram0.bss output sections
    and the _heap_start assignment. See this module's docstring for the
    two entry shapes and the lines that must be skipped."""
    parsed = ParsedMap()
    lines = text.splitlines()
    n = len(lines)
    section = None  # "data", "bss", or None (outside the sections we track)
    i = 0
    while i < n:
        line = lines[i]

        m = _HEAP_START_RE.match(line)
        if m:
            parsed.heap_start = int(m.group(1), 16)
            i += 1
            continue

        m = _SECTION_HEADER_RE.match(line)
        if m:
            name = "." + m.group(1)
            size = int(m.group(3), 16)
            if name == ".dram0.data":
                parsed.dram_data_size = size
                section = "data"
            elif name == ".dram0.bss":
                parsed.dram_bss_size = size
                section = "bss"
            else:
                section = None
            i += 1
            continue

        if section is not None:
            if _FILL_RE.match(line):
                i += 1
                continue

            m = _INLINE_ENTRY_RE.match(line)
            if m:
                sub_name, _addr, size_hex, raw_object = m.groups()
                parsed.entries.append(
                    (section, sub_name, int(size_hex, 16), raw_object)
                )
                i += 1
                continue

            m = _NAME_ONLY_RE.match(line)
            if m and i + 1 < n:
                m2 = _ADDR_SIZE_OBJ_RE.match(lines[i + 1])
                if m2:
                    sub_name = m.group(1)
                    _addr, size_hex, raw_object = m2.groups()
                    parsed.entries.append(
                        (section, sub_name, int(size_hex, 16), raw_object)
                    )
                    i += 2
                    continue

        i += 1

    return parsed


# --- reporting ---------------------------------------------------------------

def _fmt(n):
    return "{:,}".format(n)


def print_summary(gates):
    print("static RAM gate (tools/check_static_ram.py, beads esp32c6-14a):")
    print("  .dram0.data              %10s bytes" % _fmt(gates["dram_data_size"]))
    print("  .dram0.bss               %10s bytes" % _fmt(gates["dram_bss_size"]))
    print("  _heap_start              0x%08x" % gates["heap_start"])
    print("  usable heap              %10s bytes  (APP_USABLE_DRAM_END 0x%08x - _heap_start)"
          % (_fmt(gates["usable_heap"]), APP_USABLE_DRAM_END))
    print("  predicted largest before framebuffer: %10s bytes  (usable heap - pre-fb overhead %s)"
          % (_fmt(gates["predicted_before"]), _fmt(PRE_FRAMEBUFFER_OVERHEAD_BYTES)))
    moment = "after POST (selftest)" if gates["selftest"] else "at shell ready"
    print("  predicted largest %-21s %10s bytes  (usable heap - framebuffer %s - boot/separate-region %s)"
          % (moment + ":", _fmt(gates["predicted_after"]),
             _fmt(FRAMEBUFFER_BYTES), _fmt(gates["overhead"])))
    print("  gate 1 (framebuffer fits):    %-4s  %s >= %s (framebuffer %s + slack %s)"
          % ("PASS" if gates["gate1_pass"] else "FAIL",
             _fmt(gates["predicted_before"]), _fmt(gates["gate1_threshold"]),
             _fmt(FRAMEBUFFER_BYTES), _fmt(FRAMEBUFFER_SLACK_BYTES)))
    print("  gate 2 (one grid fits %-21s %-4s  %s %s %s (grid 184x224)"
          % (moment + "):", "PASS" if gates["gate2_pass"] else "FAIL",
             _fmt(gates["predicted_after"]),
             ">=" if gates["gate2_pass"] else "< ",
             _fmt(gates["gate2_threshold"])))


def print_offenders(parsed):
    by_size = sorted(parsed.entries, key=lambda e: e[2], reverse=True)

    print("")
    print("  top %d static symbols (.dram0.data + .dram0.bss):" % TOP_SYMBOLS_SHOWN)
    for section, name, size, raw_object in by_size[:TOP_SYMBOLS_SHOWN]:
        print("    %-28s %10s  %-4s  %s"
              % (name.lstrip("."), _fmt(size), section, shorten_object(raw_object)))

    totals = {}
    for _section, _name, size, raw_object in parsed.entries:
        label = shorten_object(raw_object)
        totals[label] = totals.get(label, 0) + size
    top_objects = sorted(totals.items(), key=lambda kv: kv[1], reverse=True)

    print("")
    print("  top %d object files by .data+.bss:" % TOP_OBJECTS_SHOWN)
    for label, size in top_objects[:TOP_OBJECTS_SHOWN]:
        print("    %-28s %10s" % (label, _fmt(size)))

    print("")
    print("  what to do: shrink or malloc-on-use the offenders above (see")
    print("  docs/notes/Optimization-Playbook.md, \"Test and debug code")
    print("  shares your production memory budget\"). If boot allocations")
    print("  genuinely changed on purpose, re-peg PRE_FRAMEBUFFER_OVERHEAD_")
    print("  BYTES / BOOT_ALLOCATIONS_AND_SEPARATE_REGION_BYTES from a fresh")
    print("  boot log - see this script's header comment for how the")
    print("  current values were calibrated.")


def run(map_path):
    text = Path(map_path).read_text(encoding="utf-8", errors="replace")
    parsed = parse_map(text)

    missing = [n for n in ("dram_data_size", "dram_bss_size", "heap_start")
               if getattr(parsed, n) is None]
    if missing:
        print("check_static_ram: could not find %s in %s - map format may "
              "have changed; see this script's parser." % (", ".join(missing), map_path),
              file=sys.stderr)
        return 1

    gates = compute_gates(parsed.heap_start, parsed.dram_data_size,
                          parsed.dram_bss_size, parsed_is_selftest(parsed))
    print_summary(gates)

    if gates["gate1_pass"] and gates["gate2_pass"]:
        return 0

    print_offenders(parsed)
    print("")
    print("check_static_ram: FAILED - the framebuffer and one sand grid must "
          "both fit, predicted from this build's .data/.bss. See the gate "
          "output above and this script's header comment.", file=sys.stderr)
    return 1


# --- self-test ---------------------------------------------------------------

_SELF_TEST_MAP = """\
.dram0.data     0x40810420       0x18
 *(.data)
 .data.foo      0x40810420        0x8 esp-idf/main/libmain.a(foo.c.obj)
                0x40810420                foo
 .data.a_very_long_symbol_name_that_wraps_to_its_own_line
                0x40810428       0x10 esp-idf/freertos/libfreertos.a(port.c.obj)
 *fill*         0x40810438        0x0
                0x40810438                        _data_end = ABSOLUTE (.)

.dram0.bss      0x40810440       0x20
 *(.bss .bss.*)
 .bss.baz       0x40810440       0x20 esp-idf/main/libmain.a(baz.c.obj)
                0x40810440                        baz
                0x40810460                        _bss_end = ABSOLUTE (.)

.dram0.heap_start
                0x40810460                        _heap_start = ABSOLUTE (.)
"""


def _assert_eq(actual, expected, what):
    if actual != expected:
        raise AssertionError("%s: expected %r, got %r" % (what, expected, actual))


def self_test():
    parsed = parse_map(_SELF_TEST_MAP)

    _assert_eq(parsed.dram_data_size, 0x18, "dram_data_size")
    _assert_eq(parsed.dram_bss_size, 0x20, "dram_bss_size")
    _assert_eq(parsed.heap_start, 0x40810460, "heap_start")
    _assert_eq(len(parsed.entries), 3, "entry count")

    by_name = {name: (section, size, raw_object)
               for section, name, size, raw_object in parsed.entries}
    _assert_eq(by_name[".data.foo"][1], 8, "foo size")
    _assert_eq(by_name[".data.foo"][0], "data", "foo section")
    _assert_eq(shorten_object(by_name[".data.foo"][2]), "foo.c.obj", "foo object")
    _assert_eq(by_name[".data.a_very_long_symbol_name_that_wraps_to_its_own_line"][1],
               16, "wrapped-name entry size (two-line shape)")
    _assert_eq(shorten_object(
        by_name[".data.a_very_long_symbol_name_that_wraps_to_its_own_line"][2]),
        "freertos port.c.obj", "wrapped-name entry object (non-main component prefixed)")
    _assert_eq(by_name[".bss.baz"][1], 32, "baz size")
    _assert_eq(by_name[".bss.baz"][0], "bss", "baz section")

    # A build with tons of headroom: both gates pass.
    roomy_heap_start = 0x40811000
    roomy = compute_gates(heap_start=roomy_heap_start, dram_data_size=0, dram_bss_size=0)
    _assert_eq(roomy["usable_heap"], APP_USABLE_DRAM_END - roomy_heap_start, "roomy usable_heap")
    assert roomy["gate1_pass"] and roomy["gate2_pass"], "roomy case should pass both gates"

    # A build so bloated even the framebuffer itself no longer fits: gate 1
    # fails (and gate 2 necessarily fails too, since it depends on gate 1's
    # remainder).
    tiny_heap = compute_gates(
        heap_start=APP_USABLE_DRAM_END - (FRAMEBUFFER_BYTES // 2),
        dram_data_size=0, dram_bss_size=0)
    assert not tiny_heap["gate1_pass"], "undersized-heap case should fail gate 1"
    assert not tiny_heap["gate2_pass"], "undersized-heap case should also fail gate 2"

    # A build where the framebuffer fits but the grid, allocated right
    # after it, does not - this is the shape both real-world incidents took.
    grid_only_fail_heap_start = (
        APP_USABLE_DRAM_END - FRAMEBUFFER_BYTES
        - BOOT_ALLOCATIONS_AND_SEPARATE_REGION_BYTES - (GRID_BYTES - 1)
    )
    grid_only_fail = compute_gates(
        heap_start=grid_only_fail_heap_start, dram_data_size=0, dram_bss_size=0)
    assert grid_only_fail["gate1_pass"], "grid-only case should still pass gate 1"
    assert not grid_only_fail["gate2_pass"], "grid-only case should fail gate 2"

    # Pin the real 2026-09-06 measurement itself: this exact heap_start
    # (0x40817480 in that boot's map) must reproduce the measured
    # shell-ready largest block, 44,032 bytes, and pass gate 2 - this is
    # the fixed point every re-peg of BOOT_ALLOCATIONS_AND_SEPARATE_REGION_
    # BYTES has to keep reproducing. Raising heap_start by 4 KiB (as if
    # .data/.bss grew by that much) eats straight into the same margin and
    # must fail it, since that margin is exactly what the two on-device
    # incidents ran out of.
    measured = compute_gates(heap_start=0x40817480, dram_data_size=0, dram_bss_size=0)
    _assert_eq(measured["predicted_after"], 44032, "measured shell-ready largest block")
    assert measured["gate2_pass"], "measured 2026-09-06 boot should pass gate 2"

    grown = compute_gates(heap_start=0x40817480 + 4096, dram_data_size=0, dram_bss_size=0)
    assert not grown["gate2_pass"], "4 KiB more static RAM should fail gate 2"

    # A SELFTEST image is judged one boot phase earlier, where its own
    # suites allocate - see SELFTEST_BOOT_ALLOCATIONS_BYTES. Same heap
    # start, so the only difference is which moment gate 2 asks about, and
    # the selftest moment must be the roomier of the two.
    as_selftest = compute_gates(heap_start=0x40817480, dram_data_size=0,
                                dram_bss_size=0, selftest=True)
    _assert_eq(as_selftest["predicted_after"], 50176,
               "measured after-POST largest block, the selftest moment")
    assert as_selftest["predicted_after"] > measured["predicted_after"], \
        "the selftest moment has to be roomier than shell ready, or it " \
        "would not be worth distinguishing"

    # And the map itself is what decides which of the two applies, on
    # whether a suite contributed static data - not on the mere presence of
    # libunity.a, which every variant links (see parsed_is_selftest).
    class _Fake(object):
        def __init__(self, entries):
            self.entries = entries

    assert parsed_is_selftest(_Fake([
        ("bss", ".bss.s", 232, "esp-idf/main/libmain.a(suite_sand.c.obj)")])), \
        "a suite object owning .bss means the suites were compiled in"
    assert parsed_is_selftest(_Fake([
        ("bss", ".bss.Unity", 344, "esp-idf/unity/libunity.a(unity.c.obj)")])), \
        "unity owning .bss means the same"
    assert not parsed_is_selftest(parsed), \
        "the synthetic map above carries no suite data and must read as ordinary"

    print("self-test OK")
    return 0


# --- CLI ---------------------------------------------------------------------

def main(argv):
    parser = argparse.ArgumentParser(
        description="Predict, from a GNU ld map file, whether the "
                    "framebuffer plus one sand grid still fit in the "
                    "device's contiguous DRAM heap. See this script's "
                    "module docstring for the full arithmetic and its "
                    "provenance (beads esp32c6-14a).")
    parser.add_argument("--map", help="path to launcher.map")
    parser.add_argument("--self-test", action="store_true",
                         help="run the parser/arithmetic self-test on an "
                              "embedded synthetic map and exit")
    args = parser.parse_args(argv)

    if args.self_test:
        try:
            return self_test()
        except AssertionError as exc:
            print("check_static_ram --self-test: FAILED: %s" % exc, file=sys.stderr)
            return 1

    if not args.map:
        parser.error("--map is required unless --self-test is given")

    return run(args.map)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
