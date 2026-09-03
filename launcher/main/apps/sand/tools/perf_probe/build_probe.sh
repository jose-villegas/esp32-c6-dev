#!/bin/sh
#
# Build the host attribution probe - the canonical DEVICE_BUILD-on-host
# harness for sand's frame-budget scenes (bd esp32c6-o2s). Compiles the
# repo's own suite_sand.c (unmodified except for the SAND_HOST_PROBE
# wrapper functions living right beside the real test bodies they call -
# see suite_sand.c itself) with -DDEVICE_BUILD, so the actual official
# scenes run, not a hand-copy of them.
#
# This replaces two per-round copies of the same harness
# (launcher/main/apps/sand/tools/perf_probe/ from the cross-flow round and
# tools/perf_probe_reactions/ from the mask round) that drifted apart over
# a couple of days for no reason other than each round not knowing about
# the other's worktree. One harness, every scene: run `probe --list` (built
# below) for the current names, or read the table at the top of
# probe_main.c.
#
# Usage:
#   ./build_probe.sh <output-binary> [extra -D flags...]
#
# Examples:
#   ./build_probe.sh out/probe
#   ./build_probe.sh out/probe_counters -DSAND_PERF_COUNTERS
#
# -DSAND_PERF_COUNTERS is accepted here as plumbing only - nothing in this
# tree defines sand_perf_counters.h or reads the macro from suite_sand.c
# right now, so passing it is currently a no-op. It exists so a round that
# needs host-only call counters (like the cross-flow round did) can add
# that instrumentation - inert, #ifdef-guarded, additive to the test bodies
# it measures - without also having to reinvent this build script's flag
# plumbing.
#
# For an interleaved best-of-N run across several scenes, see run_probe.py
# in this same directory rather than looping this script by hand.
#
# CODEGEN FIDELITY: the flags that change what code GCC emits for portable
# C - most importantly -fno-jump-tables -fno-tree-switch-conversion, which
# mean a switch can NEVER become a jump table on device - come from the one
# device profile (launcher/tools/device_profiles/esp32c6.sh, selected by
# $DEVICE_PROFILE), not a literal list here. A host build that omits them
# can compile a switch to a jump table where the device compiles a compare
# chain, and a dispatcher-shaped change gets mispredicted in the wrong
# direction - attempt 19's regression was partly this. DO NOT "simplify"
# these back into a literal -O2 here; the whole point of reading them from
# the profile is that they can never drift from the device's own compile
# command again. -Wall/-Wextra/-Werror/-Wno-unused-parameter stay this
# script's own concern - they are strictness, not codegen, and the device
# build's own warning set differs (see esp32c6.sh's DP_CODEGEN_SOURCE).
#
# WHAT THIS DID AND DID NOT BUY, measured 2026-09-03, so nobody re-runs the
# experiment expecting a win: on the tree as it stands the two switch flags
# change nothing at all. Compiling every portable sand source (including
# suite_sand.c) at -O2 with and without them emits ZERO indirect jumps
# either way - there is currently no switch dense enough for GCC to convert,
# and sand_reactions.c has no switch left in it. An interleaved best-of-N
# A/B over the reaction scenes found no consistent magnitude change either,
# which is exactly what that disassembly predicts. This is insurance that
# costs nothing today: it starts mattering the moment a dense switch lands -
# the pair-matrix dispatcher (bd esp32c6-iu5) being the obvious candidate,
# and the one whose device regression started this.
#
# POSIX sh, same portability reasoning as launcher/test/run_tests.sh.

set -eu

HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
APP_SAND="$(CDPATH= cd -- "$HERE/../.." && pwd)"
MAIN_DIR="$(CDPATH= cd -- "$APP_SAND/../.." && pwd)"
TEST_DIR="$(CDPATH= cd -- "$MAIN_DIR/../test" && pwd)"

OUT="${1:?usage: build_probe.sh <output-binary> [extra -D flags...]}"
shift
EXTRA_DEFS="$*"

. "$MAIN_DIR/../tools/find_cc.sh"
if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    exit 1
fi

# DEVICE_PROFILE picks the chip (default esp32c6) - see device_profile.sh's
# own header for why the profiles directory is passed explicitly rather
# than discovered.
. "$MAIN_DIR/../tools/device_profile.sh"
device_profile_load "" "$MAIN_DIR/../tools/device_profiles"

# DP_CODEGEN_FLAGS is the device's codegen-shaping set (includes -O2).
# DP_ARCH_FLAGS is the ISA-targeting set (-march=...) for the cross build
# ONLY - it is deliberately never read here. Mixing "which instructions"
# into a host x86/ARM compile would either be rejected outright or silently
# ignored, and conflating it with "how a switch compiles" is exactly the
# mistake this profile split exists to prevent.
CODEGEN_FLAGS=$(device_profile_require DP_CODEGEN_FLAGS) || exit 1
STD_FLAG=$(device_profile_require DP_STD_FLAG) || exit 1

CFLAGS="$STD_FLAG -Wall -Wextra -Werror -Wno-unused-parameter $CODEGEN_FLAGS -g"
DEFS="-DDEVICE_BUILD -DSAND_HOST_PROBE -DCONFIG_LAUNCHER_DEVELOPMENT=1 $EXTRA_DEFS"

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
