#!/bin/sh
#
# Build and run grid_fingerprint.c: a hash of the simulation's actual
# output per reference scene, with the material histogram behind it.
#
# The point is behavioural, not numeric. Every frame-budget row in this
# app measures TIME; nothing measures whether an optimisation still
# simulates the same thing. This does, over every cell of four scenes,
# so a perf change can be shown to be free of behavioural consequence
# rather than merely green - see grid_fingerprint.c's own top comment.
#
# Usage:
#   main/apps/sand/tools/report_fingerprint.sh            # print
#   main/apps/sand/tools/report_fingerprint.sh --check    # diff vs baseline
#   main/apps/sand/tools/report_fingerprint.sh --update   # re-record baseline
#
# --check is the gate: exit 0 means byte-identical behaviour, exit 1 means
# something moved. It prints the diff, so a caller that cannot interpret a
# hash can still show a human which scene changed and whether the material
# counts moved with it (they should not, for a pure reordering).
#
# --update rewrites the baseline and is DELIBERATELY not something the
# optimisation loop may call. A loop that can re-record its own baseline
# has no baseline; recording one is a human act, done when a behavioural
# change has been reviewed and accepted.

set -eu

# This file lives at main/apps/sand/tools/, four levels below launcher/ -
# tools -> sand -> apps -> main -> launcher - resolved the same way as
# report_reactions.sh and report_performance.sh beside it.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SAND_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
MAIN_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)

BASELINE="$SCRIPT_DIR/fingerprint_baseline.txt"
BUILD_DIR="$SCRIPT_DIR/build"

# --- find a compiler -------------------------------------------------------
# Sourced, not copied - see tools/find_cc.sh's own top comment.
# shellcheck source=../../../../tools/find_cc.sh
. "$LAUNCHER_DIR/tools/find_cc.sh"

if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    echo "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
    echo "  Debian:  sudo apt install build-essential" >&2
    echo "  macOS:   xcode-select --install" >&2
    exit 1
fi

# Same flags as run_tests.sh, deliberately copied rather than relaxed: this
# tool's output is used to accept or reject changes, so it has no business
# being built more loosely than the tests whose verdict it supplements.
# -Wno-unused-parameter is part of that set, not a concession made here -
# the app's own sources do not build without it.
CFLAGS="-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -g -O1"

mkdir -p "$BUILD_DIR"
OUT_BIN="$BUILD_DIR/grid_fingerprint"

# The portable half of the app only. app_sand.c and sand_ui.c are the
# hardware-facing entry points (the apps/<name>/app_*.c convention in
# CLAUDE.md) and do not belong in a host build; palette.c and row_runs.c
# are draw-path concerns the grid state does not depend on.
# shellcheck disable=SC2086
"$CC_BIN" $CFLAGS -I "$MAIN_DIR" -I "$SAND_DIR" \
    "$SCRIPT_DIR/grid_fingerprint.c" \
    "$SAND_DIR/sand.c" \
    "$SAND_DIR/sand_reactions.c" \
    "$SAND_DIR/sand_gas.c" \
    "$SAND_DIR/sand_liquid.c" \
    "$SAND_DIR/material.c" \
    -o "$OUT_BIN"

# MinGW appends .exe; elsewhere the plain name is produced.
[ -x "$OUT_BIN" ] || OUT_BIN="$OUT_BIN.exe"

case "${1:-}" in
--check)
    if [ ! -f "$BASELINE" ]; then
        echo "No baseline at $BASELINE - record one with --update first." >&2
        exit 1
    fi
    TMP_OUT="$BUILD_DIR/fingerprint.current.txt"
    "$OUT_BIN" > "$TMP_OUT"
    if diff -u "$BASELINE" "$TMP_OUT"; then
        echo "fingerprint: identical to baseline"
        exit 0
    fi
    echo >&2
    echo "BEHAVIOUR CHANGED. The simulation no longer produces the same" >&2
    echo "grid it did at the recorded baseline." >&2
    echo >&2
    echo "Read the diff above by the numbers, not just the hash: the 16" >&2
    echo "columns after it are per-material cell counts. Identical counts" >&2
    echo "with a different hash means cells moved but nothing was created" >&2
    echo "or destroyed - the signature of a REORDERING, which this project" >&2
    echo "has found to be semantically fine before (and expensive to prove" >&2
    echo "so). Changed counts mean material appeared or vanished, which is" >&2
    echo "a bug until someone demonstrates otherwise." >&2
    exit 1
    ;;
--update)
    "$OUT_BIN" > "$BASELINE"
    echo "Baseline recorded: $BASELINE"
    ;;
"")
    "$OUT_BIN"
    ;;
*)
    echo "Unknown argument: $1" >&2
    echo "Usage: report_fingerprint.sh [--check|--update]" >&2
    exit 2
    ;;
esac
