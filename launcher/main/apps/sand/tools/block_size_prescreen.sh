#!/bin/sh
#
# Host pre-screen for SAND_BLOCK_W/SAND_BLOCK_H, ahead of the device sweep
# (block_size_sweep.ps1 beside this file). Builds one attribution probe per
# candidate shape, then ranks them against the shipped shape with
# perf_probe/run_probe.py --compare.
#
# The sweep costs a build, a flash and a serial capture per candidate, and
# holds the board for the whole run. This narrows thirteen candidates to the
# two or three worth that. It RANKS and never prices: the host-to-device
# ratio is scene-specific and has been off by more than the differences
# being ranked - see docs/sand/Perf-Instruments.md.
#
# Every binary is built BEFORE any is measured. A host timing taken while a
# compiler is running is worthless here - the same binary has measured 268
# us quiet and 499 us under load - and interleaving builds with runs would
# hand the first candidate a quiet machine and the last a busy one.
#
# Usage:
#   ./block_size_prescreen.sh [-n BLOCKS] [-o OUTDIR] [scene ...]
#
# Default scenes are the three landscape rows, the two settled-board rows
# and both controls; naming scenes overrides that list. --list on any built
# probe prints what is available.
#
# settled_screen is not optional. A block shape trades two things against
# each other - a smaller block skips a busy board more finely, and scans
# more blocks on a board where nothing moves - and that second half only
# appears on a settled row. Rank a candidate without it and every small
# block looks free.
#
# POSIX sh, same portability reasoning as launcher/test/run_tests.sh.

set -eu

HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
APP_SAND="$(CDPATH= cd -- "$HERE/.." && pwd)"
SAND_H="$APP_SAND/sand.h"

BLOCKS=8
OUT="$HERE/results/prescreen"
SCENES=""

while [ $# -gt 0 ]; do
    case "$1" in
        -n) BLOCKS="$2"; shift 2 ;;
        -o) OUT="$2"; shift 2 ;;
        *)  SCENES="$SCENES $1"; shift ;;
    esac
done

[ -n "$SCENES" ] || SCENES="landscape_water landscape_deep_water landscape_sand settled_screen plant_idle full_step_control settled_flip_control"

# Closed under transpose, plus the square, for the reason
# block_size_sweep.ps1's header gives. BASELINE must be the shape sand.h
# ships, since every delta below is reported against it.
BASELINE="32x64"
CANDIDATES="8x32 16x32 8x64 16x64 32x64 32x128 32x8 32x16 64x8 64x16 64x32 128x32 32x32"

mkdir -p "$OUT"

ORIGINAL="$OUT/sand.h.orig"
cp "$SAND_H" "$ORIGINAL"
# Restores the header whatever happens - a sweep left half-applied is the
# one failure mode that costs somebody else their afternoon.
trap 'cp "$ORIGINAL" "$SAND_H"' EXIT INT TERM

for c in $CANDIDATES; do
    w=${c%x*}
    h=${c#*x}
    sed -e "s/^#define SAND_BLOCK_W [0-9]*/#define SAND_BLOCK_W $w/" \
        -e "s/^#define SAND_BLOCK_H [0-9]*/#define SAND_BLOCK_H $h/" \
        "$ORIGINAL" > "$SAND_H"
    echo "building $c"
    sh "$HERE/perf_probe/build_probe.sh" "$OUT/probe_$c" >/dev/null
done

cp "$ORIGINAL" "$SAND_H"

probe_path() {
    [ -x "$OUT/probe_$1" ] && { echo "$OUT/probe_$1"; return; }
    echo "$OUT/probe_$1.exe"
}

BASE_BIN=$(probe_path "$BASELINE")
for c in $CANDIDATES; do
    [ "$c" = "$BASELINE" ] && continue
    echo
    echo "=== $BASELINE (A) vs $c (B) ==="
    python "$HERE/perf_probe/run_probe.py" "$BASE_BIN" \
        --compare "$(probe_path "$c")" --n "$BLOCKS" $SCENES
done
