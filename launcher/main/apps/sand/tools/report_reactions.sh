#!/bin/sh
#
# Build and run dump_reactions.c, and write its markdown to
# docs/Sand/Reaction-Table.md - generated fresh from material.c's own
# tables so it can never go stale the way a hand-transcribed copy can (see
# report_performance.sh's own top comment, and this file's sibling
# dump_reactions.c for why that matters more here: the hand-maintained
# tables it replaces had already drifted before this existed - an EMBER
# node in a mermaid chart for a material that no longer exists, a byproduct
# table in Sand-Simulation.md nobody re-derives by hand).
#
# Usage:
#   main/apps/sand/tools/report_reactions.sh            regenerate the doc
#   main/apps/sand/tools/report_reactions.sh --check     verify it is
#                                                         current; exits
#                                                         nonzero on drift
#
# POSIX sh on purpose, same as test/run_tests.sh: works under Git Bash or
# MSYS on Windows, and natively on Linux and macOS.

set -eu

# This file lives at main/apps/sand/tools/, four levels below launcher/ -
# tools -> sand -> apps -> main -> launcher - same layout report_
# performance.sh (this file's sibling) already resolves the same way.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SAND_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
MAIN_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)
REPO_ROOT=$(CDPATH= cd -- "$LAUNCHER_DIR/.." && pwd)

OUT_MD="$REPO_ROOT/docs/Sand/Reaction-Table.md"
BUILD_DIR="$SCRIPT_DIR/build"

# --- find a compiler -------------------------------------------------------
# Sourced, not copied - see tools/find_cc.sh's own top comment for why a
# duplicated block is exactly the drift problem this script itself exists
# to avoid, one level over.
# shellcheck source=../../../../tools/find_cc.sh
. "$LAUNCHER_DIR/tools/find_cc.sh"

if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    echo "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
    echo "  Debian:  sudo apt install build-essential" >&2
    echo "  macOS:   xcode-select --install" >&2
    exit 1
fi

# Same strictness as run_tests.sh: a host build catches mistakes the target
# build misses, and this file has no reason to be less careful than the
# tests are.
CFLAGS="-std=c11 -Wall -Wextra -Werror"

mkdir -p "$BUILD_DIR"
OUT_BIN="$BUILD_DIR/dump_reactions"

# shellcheck disable=SC2086
"$CC_BIN" $CFLAGS -I "$MAIN_DIR" -I "$SAND_DIR" \
    "$SCRIPT_DIR/dump_reactions.c" "$SAND_DIR/material.c" -o "$OUT_BIN"

# MinGW appends .exe; elsewhere the plain name is produced.
[ -x "$OUT_BIN" ] || OUT_BIN="$OUT_BIN.exe"

if [ "${1:-}" = "--check" ]; then
    TMP_MD="$BUILD_DIR/Reaction-Table.generated.md"
    "$OUT_BIN" > "$TMP_MD"
    if ! diff -u "$OUT_MD" "$TMP_MD"; then
        echo >&2
        echo "docs/Sand/Reaction-Table.md is stale - run" \
             "main/apps/sand/tools/report_reactions.sh and commit the" \
             "result." >&2
        exit 1
    fi
    echo "docs/Sand/Reaction-Table.md is current."
    exit 0
fi

"$OUT_BIN" > "$OUT_MD"
echo "Wrote $OUT_MD"
