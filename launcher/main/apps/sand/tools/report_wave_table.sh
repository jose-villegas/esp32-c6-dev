#!/bin/sh
#
# Build and run gen_wave_table.c, and either print the water_wave[256] table
# it computes or check it against the copy baked into material.c.
#
# Usage:
#   main/apps/sand/tools/report_wave_table.sh            print the table
#   main/apps/sand/tools/report_wave_table.sh --check     verify material.c's
#                                                          baked table matches
#                                                          the formula; exits
#                                                          nonzero on drift
#
# The table in material.c is what actually ships and what
# test_the_wave_table_matches_its_formula (suite_sand.c) checks on every
# `run_tests.sh` - that test is the real safety net, run on every build. This
# script is the other half: producing the 256 numbers in the first place
# without hand-typing them, and a convenience --check for confirming
# material.c has not been hand-edited out from under the generator since. If
# this script and the unit test ever disagree about drift, trust the test -
# it is what CI actually runs.
#
# POSIX sh on purpose, same as run_tests.sh and report_reactions.sh (this
# file's sibling): works under Git Bash or MSYS on Windows, and natively on
# Linux and macOS.

set -eu

# This file lives at main/apps/sand/tools/, four levels below launcher/ -
# tools -> sand -> apps -> main -> launcher - same layout report_
# reactions.sh (this file's sibling) already resolves the same way.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SAND_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)

MATERIAL_C="$SAND_DIR/material.c"
BUILD_DIR="$SCRIPT_DIR/build"

# --- find a compiler -------------------------------------------------------
# Sourced, not copied - see report_reactions.sh's own top comment for why a
# duplicated block is exactly the drift problem these scripts exist to avoid.
# shellcheck source=../../../../tools/find_cc.sh
. "$LAUNCHER_DIR/tools/find_cc.sh"

if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    echo "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
    echo "  Debian:  sudo apt install build-essential" >&2
    echo "  macOS:   xcode-select --install" >&2
    exit 1
fi

# Same strictness as run_tests.sh and report_reactions.sh: a host build
# catches mistakes the target build misses.
CFLAGS="-std=c11 -Wall -Wextra -Werror"

mkdir -p "$BUILD_DIR"
OUT_BIN="$BUILD_DIR/gen_wave_table"

# shellcheck disable=SC2086
"$CC_BIN" $CFLAGS "$SCRIPT_DIR/gen_wave_table.c" -o "$OUT_BIN" -lm

# MinGW appends .exe; elsewhere the plain name is produced.
[ -x "$OUT_BIN" ] || OUT_BIN="$OUT_BIN.exe"

if [ "${1:-}" = "--check" ]; then
    # Pull the water_wave[256] block straight out of material.c - between
    # its declaration and the closing "};" - and diff it against a fresh
    # generation. sed rather than a real parser: the table is the only
    # thing in material.c shaped like this, so "from the declaration line
    # to the next line that is exactly the closing brace" cannot match
    # anything else.
    TMP_CURRENT="$BUILD_DIR/water_wave.current.txt"
    TMP_FRESH="$BUILD_DIR/water_wave.fresh.txt"

    # tr -d '\r' on both sides: the generator is a native Windows binary on
    # that platform and stdio there writes text-mode CRLF, while material.c
    # itself is LF-only - a line-ending mismatch is not the drift this check
    # is looking for, and diffing it in would make --check fail on every
    # platform except whichever one baked the table originally.
    sed -n '/^static const int8_t water_wave\[256\] = {$/,/^};$/p' \
        "$MATERIAL_C" | tr -d '\r' > "$TMP_CURRENT"
    "$OUT_BIN" | tail -n +6 | tr -d '\r' > "$TMP_FRESH"   # drop the banner

    if [ ! -s "$TMP_CURRENT" ]; then
        echo "Could not find water_wave[256] in material.c - has it been" \
             "renamed or reformatted?" >&2
        exit 1
    fi

    if ! diff -u "$TMP_CURRENT" "$TMP_FRESH"; then
        echo >&2
        echo "material.c's water_wave[256] does not match the formula in" \
             "gen_wave_table.c - regenerate with" \
             "main/apps/sand/tools/report_wave_table.sh and paste the" \
             "result in, or fix whichever side changed by accident." >&2
        exit 1
    fi
    echo "material.c's water_wave[256] matches the formula."
    exit 0
fi

"$OUT_BIN"
