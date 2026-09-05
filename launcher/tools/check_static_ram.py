#!/usr/bin/env python3
"""Link-time static-RAM gate: does the framebuffer, then one sand grid,
still fit after this build's .data/.bss?

    python3 launcher/tools/check_static_ram.py --map <build>/launcher.map
    python3 launcher/tools/check_static_ram.py --self-test

Wired into launcher/CMakeLists.txt as a POST_BUILD step on the ELF target,
so it runs for every variant (release, dev, diag) on every `idf.py build`,
on a laptop, before anything is flashed.

WHY THIS EXISTS (beads esp32c6-14a)

DIRAM is one pool behind .data/.bss AND the heap (see docs/Notes/
Board-and-Memory.md, "Static growth taxes the heap too"). The single
368x448x2 framebuffer (gfx_init(), launcher/main/gfx/gfx.c) must be one
contiguous DMA allocation at boot, and the sand app's real-size grid (and
the real-size fixtures in main/apps/sand/suite_sand.c) then need one
contiguous 41,216-byte block after it. Twice, a static test buffer grew
until the framebuffer - or the grid - no longer fit, and nothing caught it
at build time: the host suite stayed green (it has megabytes of stack and
gigabytes of heap), the firmware still linked and flashed, and the failure
only showed up as a boot hang or dozens of on-device "Expected Non-NULL"
failures. This script moves that failure to link time, on a laptop, naming
the largest offenders.

This is a PREDICTION, not a reproduction. It has no visibility into runtime
fragmentation from allocation order, so it is deliberately conservative
(see FRAGMENTATION_BYTES below) and the messages say "predicted" - expect
about +-1 KiB of slop from the two calibrated constants.

THE ARITHMETIC

  APP_USABLE_DRAM_END = 0x4087c610. From ESP-IDF's components/heap/port/
  esp32c6/memory_layout.c: SOC_ROM_STACK_START (0x4087e610) minus
  SOC_ROM_STACK_SIZE (0x2000). The main heap region runs from the linker
  symbol _heap_start up to this address, in one contiguous piece. (An
  additional ~11 KiB above it is added to the heap later at startup as a
  SEPARATE region and never helps a single large allocation - ignored
  here.)

    usable_heap = APP_USABLE_DRAM_END - _heap_start

  _heap_start is read straight out of the map file - it already reflects
  this build's .data/.bss (and .sdata/.sbss), so it is the one number this
  script needs the linker to have computed, not recompute itself.

  FRAMEBUFFER_BYTES = 368 * 448 * 2 = 329,728 (launcher/main/gfx/gfx.c,
  gfx_init()).

  BOOT_OVERHEAD_BYTES = 12,548. Calibrated 2026-09-05 from a
  development-image boot log: heap_init reported the region "At 40817390
  len 00065280" (usable = 414,336) and gfx logged "329728 bytes, 72060
  bytes of heap still free" immediately after allocating the framebuffer:
  414,336 - 329,728 - 72,060 = 12,548 bytes taken before/around the
  framebuffer by task stacks, drivers, the SD probe and gfx's own gather
  buffer. Re-peg from those two log lines whenever boot allocations
  change.

  FRAGMENTATION_BYTES = 20,480. Same boot: POST reported "memory 70 KiB
  free, DMA block 50 KiB"; the diagnostics image on 2026-09-02 reported
  "59 KiB free, DMA block 38 KiB". About 20 KiB of the post-framebuffer
  free heap is never in the largest contiguous block. Empirical; re-peg
  from POST's memory line.

  GRID_BYTES = 41,216 = 184 * 224: the sand app's grid at the real screen
  size (main/apps/sand/app_sand.c) and the STRESS_W*STRESS_H fixture of
  the real-size tests in main/apps/sand/suite_sand.c.

    predicted_largest_before_fb = usable_heap - BOOT_OVERHEAD_BYTES
    predicted_largest_after_fb  = predicted_largest_before_fb
                                   - FRAMEBUFFER_BYTES - FRAGMENTATION_BYTES

TWO GATES, applied to every variant (the numbers differ only in how much
headroom is left):

  1. predicted_largest_before_fb >= FRAMEBUFFER_BYTES + FRAMEBUFFER_-
     SLACK_BYTES  -  the framebuffer itself must fit, with a little slack.
  2. predicted_largest_after_fb  >= GRID_BYTES  -  one real-size grid must
     still fit afterwards. Zero margin here on purpose: this is exactly
     the number that failed on-device twice.

If a build fails here: shrink or malloc-on-use the largest offenders
printed below (see docs/Notes/Optimization-Playbook.md, "Test and debug
code shares your production memory budget"), or, if boot allocations
genuinely changed on purpose, re-peg BOOT_OVERHEAD_BYTES /
FRAGMENTATION_BYTES from a fresh boot log as described above and update
this header comment's provenance alongside the constant.
"""

import argparse
import re
import sys
from pathlib import Path

# --- calibrated constants (see header comment for provenance) -------------

APP_USABLE_DRAM_END = 0x4087C610
FRAMEBUFFER_BYTES = 368 * 448 * 2
BOOT_OVERHEAD_BYTES = 12548
FRAGMENTATION_BYTES = 20480
GRID_BYTES = 184 * 224
FRAMEBUFFER_SLACK_BYTES = 4096

TOP_SYMBOLS_SHOWN = 12
TOP_OBJECTS_SHOWN = 8


# --- pure arithmetic (no file I/O - this is what --self-test exercises) ---

def compute_gates(heap_start, dram_data_size, dram_bss_size):
    """Everything the gate decides, as a function of three numbers off the
    map file. Kept separate from parsing so --self-test can hit the math
    directly without a synthetic map for every boundary case."""
    usable_heap = APP_USABLE_DRAM_END - heap_start
    predicted_before = usable_heap - BOOT_OVERHEAD_BYTES
    predicted_after = predicted_before - FRAMEBUFFER_BYTES - FRAGMENTATION_BYTES
    gate1_threshold = FRAMEBUFFER_BYTES + FRAMEBUFFER_SLACK_BYTES
    gate2_threshold = GRID_BYTES
    return {
        "dram_data_size": dram_data_size,
        "dram_bss_size": dram_bss_size,
        "heap_start": heap_start,
        "usable_heap": usable_heap,
        "predicted_before": predicted_before,
        "predicted_after": predicted_after,
        "gate1_pass": predicted_before >= gate1_threshold,
        "gate1_threshold": gate1_threshold,
        "gate2_pass": predicted_after >= gate2_threshold,
        "gate2_threshold": gate2_threshold,
    }


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
    print("  predicted largest before framebuffer: %10s bytes  (usable heap - boot overhead %s)"
          % (_fmt(gates["predicted_before"]), _fmt(BOOT_OVERHEAD_BYTES)))
    print("  predicted largest after  framebuffer: %10s bytes  (- framebuffer %s - fragmentation slop %s)"
          % (_fmt(gates["predicted_after"]), _fmt(FRAMEBUFFER_BYTES), _fmt(FRAGMENTATION_BYTES)))
    print("  gate 1 (framebuffer fits):    %-4s  %s >= %s (framebuffer %s + slack %s)"
          % ("PASS" if gates["gate1_pass"] else "FAIL",
             _fmt(gates["predicted_before"]), _fmt(gates["gate1_threshold"]),
             _fmt(FRAMEBUFFER_BYTES), _fmt(FRAMEBUFFER_SLACK_BYTES)))
    print("  gate 2 (one grid fits after): %-4s  %s %s %s (grid 184x224)"
          % ("PASS" if gates["gate2_pass"] else "FAIL",
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
    print("  docs/Notes/Optimization-Playbook.md, \"Test and debug code")
    print("  shares your production memory budget\"). If boot allocations")
    print("  genuinely changed on purpose, re-peg BOOT_OVERHEAD_BYTES /")
    print("  FRAGMENTATION_BYTES from a fresh boot log - see this script's")
    print("  header comment for how the current values were calibrated.")


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

    gates = compute_gates(parsed.heap_start, parsed.dram_data_size, parsed.dram_bss_size)
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
        APP_USABLE_DRAM_END - BOOT_OVERHEAD_BYTES - FRAMEBUFFER_BYTES
        - FRAGMENTATION_BYTES - (GRID_BYTES - 1)
    )
    grid_only_fail = compute_gates(
        heap_start=grid_only_fail_heap_start, dram_data_size=0, dram_bss_size=0)
    assert grid_only_fail["gate1_pass"], "grid-only case should still pass gate 1"
    assert not grid_only_fail["gate2_pass"], "grid-only case should fail gate 2"

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
