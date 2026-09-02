#!/bin/sh
#
# Build the host timing probe for the reactions-pass pair-matrix restructure
# (bd esp32c6-iu5). Compiles the repo's own suite_sand.c (unmodified except
# for ten thin SAND_HOST_PROBE wrapper functions living right beside the
# ten real test bodies they call - see suite_sand.c itself) with
# -DDEVICE_BUILD, so the actual official scenes run, not a hand-copy of
# them.
#
# Adapted from launcher/main/apps/sand/tools/perf_probe/build_probe.sh on
# worktree-agent-a167459b17fb4db71 (commit 9de21aa) - that branch is not
# merged here, so this is a copy, not a shared script, and drops the
# SAND_PERF_COUNTERS plumbing that round's probe needed and this one does
# not (this round measures wall time only, not per-mechanism call counts).
#
# Usage:
#   ./build_probe.sh <output-binary>
#
# POSIX sh, same portability reasoning as launcher/test/run_tests.sh.

set -eu

HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
APP_SAND="$(CDPATH= cd -- "$HERE/../.." && pwd)"
MAIN_DIR="$(CDPATH= cd -- "$APP_SAND/../.." && pwd)"
TEST_DIR="$(CDPATH= cd -- "$MAIN_DIR/../test" && pwd)"

OUT="${1:?usage: build_probe.sh <output-binary>}"

. "$MAIN_DIR/../tools/find_cc.sh"
if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    exit 1
fi

CFLAGS="-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -O2 -g"
DEFS="-DDEVICE_BUILD -DSAND_HOST_PROBE -DCONFIG_LAUNCHER_DEVELOPMENT=1"

INCS="-I $MAIN_DIR -I $TEST_DIR -I $TEST_DIR/framework -I $TEST_DIR/stubs"

SOURCES="
$HERE/probe_main.c
$HERE/gfx_probe_stub.c
$HERE/esp_timer_host.c
$TEST_DIR/suites.c
$TEST_DIR/timing.c
$APP_SAND/suite_sand.c
$APP_SAND/sand.c
$APP_SAND/sand_liquid.c
$APP_SAND/sand_gas.c
$APP_SAND/sand_reactions.c
$APP_SAND/material.c
$APP_SAND/palette.c
$APP_SAND/row_runs.c
$APP_SAND/sand_ui.c
$APP_SAND/tilt.c
"

mkdir -p "$(dirname "$OUT")"

# unity.c compiled alone, without -include timing.h, for the same reason
# run_tests.sh does it - see that script's own comment.
UNITY_OBJ="$(dirname "$OUT")/unity_$$.o"
# shellcheck disable=SC2086
"$CC_BIN" $CFLAGS $INCS -c "$TEST_DIR/framework/unity.c" -o "$UNITY_OBJ"

# shellcheck disable=SC2086
"$CC_BIN" $CFLAGS $DEFS $INCS -include "$TEST_DIR/timing.h" \
    $SOURCES "$UNITY_OBJ" -o "$OUT"
rm -f "$UNITY_OBJ"

[ -x "$OUT" ] || OUT="$OUT.exe"
echo "built: $OUT"
