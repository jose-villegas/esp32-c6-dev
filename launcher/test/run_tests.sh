#!/bin/sh
#
# Build and run the portable test suites on this machine.
#
#   ./test/run_tests.sh
#   CC=clang ./test/run_tests.sh
#
# This is the fast loop: it compiles for THIS machine, not the ESP32, and runs
# in well under a second. Red-green-refactor is only practical with instant
# feedback, and a build-and-flash cycle is about ninety seconds.
#
# It runs only the PORTABLE suites. The hardware ones need real framebuffer
# memory, DMA and I2C, so they live in the firmware and run at boot on the
# device - see main/selftest.c. The suite sources are shared, so what passes
# here is the same set of assertions the board makes.
#
# POSIX sh on purpose: works under Git Bash or MSYS on Windows, and natively
# on Linux and macOS.

set -eu

TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MAIN_DIR=$(CDPATH= cd -- "$TEST_DIR/../main" && pwd)
BUILD_DIR="$TEST_DIR/build"

# --- find a compiler -------------------------------------------------------
# Sourced rather than defined here, so that report_reactions.sh (main/apps/
# sand/tools/) can find a compiler the same way without a hand-copied twin -
# see tools/find_cc.sh's own top comment.
# shellcheck source=../tools/find_cc.sh
. "$TEST_DIR/../tools/find_cc.sh"

if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    echo "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
    echo "  Debian:  sudo apt install build-essential" >&2
    echo "  macOS:   xcode-select --install" >&2
    exit 1
fi

# Warnings are errors: a host build catches mistakes the target build misses,
# and strictness costs nothing in tests.
CFLAGS="-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -g -O1"

# The shell's own portable units and their suites. Hardware suites are absent
# by design - suite_gfx.c would not compile here, which is the point.
#
# gfx_dirty.h has no matching .c: it is header-only by necessity (see its
# own file comment - mark_band() has to stay inlinable into gfx.c), so
# suite_gfx_dirty.c pulls in its own copy of the whole thing just by
# including the header, with nothing extra to add to SOURCES for it.
SOURCES="
$TEST_DIR/host_main.c
$TEST_DIR/suites.c
$TEST_DIR/framework/unity.c
$TEST_DIR/suites/suite_touch_fsm.c
$TEST_DIR/suites/suite_gesture.c
$TEST_DIR/suites/suite_button_fsm.c
$TEST_DIR/suites/suite_rng.c
$TEST_DIR/suites/suite_gfx_dirty.c
$TEST_DIR/suites/suite_gfx_color.c
$TEST_DIR/suites/suite_gfx_font.c
$TEST_DIR/suites/suite_icons.c
$TEST_DIR/suites/suite_ui_style.c
$TEST_DIR/suites/suite_ui_transform.c
$MAIN_DIR/touch_fsm.c
$MAIN_DIR/gesture.c
$MAIN_DIR/button_fsm.c
"

# App-owned sources, discovered rather than listed, so adding or deleting an
# app needs no change here.
#
# The convention: inside main/apps/<name>/, the file named app_*.c is the
# hardware-facing entry point - it talks to gfx, the IMU and the frame loop, so
# it cannot link on a host. Everything else in the folder is portable logic and
# is compiled in, along with any suite_*.c beside it.
#
# That split is not bureaucracy: it is what forces an app's logic to be
# separable from its wiring, which is the only reason a falling-sand automaton
# can be tested on a laptop at all.
#
# The glob below is one level deep (apps/*/*.c), so an app's own
# apps/<name>/tools/*.{sh,ps1,py} - its sweep scripts, report generators -
# is already invisible to it without any special-casing. Worth saying so
# explicitly: the next reader hitting a two-level-deep tools/ folder that
# this loop skips should be able to tell that is deliberate, not an
# oversight this script just hasn't caught up to yet.
for f in "$MAIN_DIR"/apps/*/*.c; do
    [ -e "$f" ] || continue
    case "$(basename "$f")" in
        app_*.c) continue ;;
    esac
    SOURCES="$SOURCES
$f"
done

# The hardware-facing app_*.c files are excluded from SOURCES above because
# they cannot link here - which also meant nothing compiled them at all
# until a full device build. Compile-check them first, so a change that
# does not build is caught here rather than on the board.
"$TEST_DIR/check_app_sources.sh"

mkdir -p "$BUILD_DIR"
OUT="$BUILD_DIR/host_tests"

# components/microui/include is on the path for ui_style.h's sake: it needs
# mu_Rect and mu_Color, and those are plain declarations in microui.h with no
# library behind them. Nothing here links microui.c - see suite_ui_style.c on
# why a style's geometry was kept free of it.
# shellcheck disable=SC2086
"$CC_BIN" $CFLAGS -I "$MAIN_DIR" -I "$TEST_DIR" -I "$TEST_DIR/framework" \
    -I "$TEST_DIR/../components/microui/include" \
    $SOURCES -o "$OUT"

# MinGW appends .exe; elsewhere the plain name is produced.
[ -x "$OUT" ] || OUT="$OUT.exe"

exec "$OUT"
