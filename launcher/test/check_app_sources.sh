#!/usr/bin/env bash
#
# Compile-check the hardware-facing app_*.c files on the host.
#
# WHY THIS EXISTS
#
# run_tests.sh deliberately skips every apps/<name>/app_*.c: those are the
# files that talk to gfx, the IMU and the frame loop, so they cannot link
# on a laptop and there is nothing to run. That is the right call for
# TESTING and it left a hole in CHECKING - the files were not compiled at
# all, by anything, until a full ESP-IDF build on the device.
#
# The hole was not theoretical. A rendering change referred to three
# identifiers declared further down the file; the host suite passed, a
# -fsyntax-only pass over suite_sand.c passed, and the error only appeared
# on the device, where it stopped an unrelated performance run.
#
# So: syntax-only, with stand-in headers (stubs/) for the handful of IDF
# and BSP things these files include. Nothing is linked and nothing runs.
# It catches what a compiler catches - undeclared identifiers, bad types,
# wrong format strings, unused statics - which is exactly the class that
# was getting through.
#
# Not a substitute for building on device. A stub only declares what the
# real header is used for, so a new IDF call needs a line adding here
# before this can see it, which is deliberate: it should be a decision.

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
MAIN_DIR="$HERE/../main"
STUBS="$HERE/stubs"

CC_BIN=""
for c in cc gcc clang; do
    if command -v "$c" >/dev/null 2>&1; then CC_BIN="$c"; break; fi
done
winlibs="${LOCALAPPDATA:-}/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin/gcc.exe"
if [ -z "$CC_BIN" ] && [ -x "$winlibs" ]; then CC_BIN="$winlibs"; fi
if [ -z "$CC_BIN" ]; then
    echo "check_app_sources: no C compiler found, skipping" >&2
    exit 0
fi

CFLAGS="-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -fsyntax-only"

# CONFIG_LAUNCHER_DEVELOPMENT on, so the development-only branches are
# compiled too - they are the ones nobody looks at until they break.
DEFS="-DCONFIG_LAUNCHER_DEVELOPMENT=1"

# Vendored components the apps include directly. Discovered rather than
# listed, so a new one needs no change here - the same reasoning
# run_tests.sh uses for finding app sources.
INCS="-I $STUBS -I $MAIN_DIR"
for inc in "$HERE"/../components/*/include; do
    [ -d "$inc" ] || continue
    INCS="$INCS -I $inc"
done

status=0
found=0
for f in "$MAIN_DIR"/apps/*/app_*.c; do
    [ -e "$f" ] || continue
    found=$((found + 1))
    # shellcheck disable=SC2086
    if "$CC_BIN" $CFLAGS $DEFS $INCS "$f"; then
        echo "  ok   $(basename "$f")"
    else
        echo "  FAIL $(basename "$f")"
        status=1
    fi
done

if [ "$found" -eq 0 ]; then
    echo "check_app_sources: found no app_*.c to check" >&2
    exit 1
fi

exit "$status"
