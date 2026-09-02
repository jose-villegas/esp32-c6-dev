#!/bin/sh
#
# Build and run dump_reactions.c, and splice its markdown into the
# BEGIN/END GENERATED region of docs/Sand/Reaction-Table.md - generated
# fresh from material.c's own tables so it can never go stale the way a
# hand-transcribed copy can (see report_performance.sh's own top comment,
# and this file's sibling dump_reactions.c for why that matters more here:
# the hand-maintained tables it replaces had already drifted before this
# existed - an EMBER node in a mermaid chart for a material that no longer
# exists, a byproduct table in Sand-Simulation.md nobody re-derives by
# hand).
#
# A SPLICE, NOT A WHOLE-FILE OVERWRITE (bd esp32c6-3mu)
#
# Some real sand mechanics - lava's cool-off chaining, the covered-lava
# burst that replaced the deleted vent code, water/acid draining stone and
# glass ~8x faster - live entirely at a read site in sand_reactions.c, on
# no reaction_t field dump_reactions.c can walk. A human documents them by
# hand, outside dump_reactions.c's reach, directly in the doc. An earlier
# version of this script wrote the generator's output over the WHOLE file
# ("$OUT_BIN" > "$OUT_MD"), which deleted that hand-written material on
# every regenerate and left docs/Sand/Reaction-Table.md permanently unable
# to pass --check (either the check fails on the hand-written lines, or
# regenerating silently destroys them - see the issue for the full story).
#
# The fix: dump_reactions.c wraps its entire output in a
# "<!-- BEGIN GENERATED -->" / "<!-- END GENERATED -->" pair (see its
# main()). Everything below replaces ONLY the text between those two
# markers in the checked-in doc, byte for byte - the title above them, the
# doc's own top note, and the hand-added appendix below them, are never
# touched. --check compares only that same delimited span, so it still
# fails on genuine drift out of material.c/sand_reactions.c, but can never
# fail merely because a human documented something the generator does not
# know about.
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

# Literal marker text - must match dump_reactions.c's main() exactly
# (matched by exact line equality below, not a regex), since that is the
# only thing telling this script where the generated region starts and
# ends inside a file that also carries hand-written prose.
BEGIN_MARK='<!-- BEGIN GENERATED -->'
END_MARK='<!-- END GENERATED -->'

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

TMP_MD="$BUILD_DIR/Reaction-Table.generated.md"
"$OUT_BIN" > "$TMP_MD"

# The doc must already exist and already carry both markers - on either
# path below. Silently creating a fresh file, or silently doing nothing,
# would both hide a real problem (a hand-deleted marker, a doc moved
# without its markers) behind what looks like success; failing loudly here
# is what "fail with a clear message rather than silently doing nothing"
# (this script's own design brief) means in practice.
if [ ! -f "$OUT_MD" ] || ! grep -qF "$BEGIN_MARK" "$OUT_MD" ||
   ! grep -qF "$END_MARK" "$OUT_MD"; then
    echo "docs/Sand/Reaction-Table.md is missing, or has no" \
         "'$BEGIN_MARK' / '$END_MARK' markers." >&2
    echo "This script only ever replaces the text BETWEEN those two" \
         "markers - restore them (see dump_reactions.c's main() for the" \
         "exact generated span they should wrap) before running this" \
         "again, on --check or otherwise." >&2
    exit 1
fi

# The delimited region as it stands in the checked-in doc today - the
# BEGIN/END marker lines themselves included, exactly like $TMP_MD's own
# first/last lines, so the two compare (or splice) as equals.
CUR_REGION="$BUILD_DIR/Reaction-Table.current-region.md"
awk -v b="$BEGIN_MARK" -v e="$END_MARK" '
    $0 == b { inside = 1 }
    inside  { print }
    $0 == e { if (inside) exit }
' "$OUT_MD" > "$CUR_REGION"

if [ "${1:-}" = "--check" ]; then
    if ! diff -u "$CUR_REGION" "$TMP_MD"; then
        echo >&2
        echo "docs/Sand/Reaction-Table.md's generated region (between" \
             "$BEGIN_MARK and $END_MARK) is stale - run" \
             "main/apps/sand/tools/report_reactions.sh and commit the" \
             "result. Hand-written material outside that region is never" \
             "touched by either command." >&2
        exit 1
    fi
    echo "docs/Sand/Reaction-Table.md's generated region is current."
    exit 0
fi

# Regenerate: splice $TMP_MD in place of the old region, leaving every
# line before BEGIN_MARK and every line after END_MARK exactly as they
# were. Three pieces, concatenated in order - never the whole file
# rewritten from $TMP_MD alone, which is the mistake this script used to
# make (see this file's own top comment).
BEFORE_MD="$BUILD_DIR/Reaction-Table.before.md"
AFTER_MD="$BUILD_DIR/Reaction-Table.after.md"
awk -v b="$BEGIN_MARK" '$0 == b { exit } { print }' "$OUT_MD" > "$BEFORE_MD"
awk -v e="$END_MARK" 'seen { print } $0 == e { seen = 1 }' "$OUT_MD" \
    > "$AFTER_MD"

SPLICED_MD="$BUILD_DIR/Reaction-Table.spliced.md"
cat "$BEFORE_MD" "$TMP_MD" "$AFTER_MD" > "$SPLICED_MD"
mv "$SPLICED_MD" "$OUT_MD"

echo "Wrote $OUT_MD (generated region only - hand-written material left" \
     "untouched)"
