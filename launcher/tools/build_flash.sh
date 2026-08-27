#!/usr/bin/env bash
#
# Build the launcher's firmware and flash it to the device.
#
# Usage:
#   tools/build_flash.sh [--diag] [COM_PORT] [IDF_EXPORT]
#
#   --diag      build the DIAGNOSTICS image instead of the release one, and
#               leave it on the board. See below for what that gets you.
#   COM_PORT    serial port the device is on. Default: COM3.
#   IDF_EXPORT  path to ESP-IDF's export script - export.bat on Windows,
#               export.sh elsewhere. Default: this project's usual install.
#
# Run from anywhere (it cds to launcher/ itself); double-click from Explorer
# if .sh is associated with Git Bash, or right-click launcher/tools/ ->
# "Git Bash Here" -> `./build_flash.sh`.
#
# All the logic here is POSIX sh. On Windows the ESP-IDF calls go through
# tools/idf_shim.bat, which exists only to delete MSYSTEM - see tools/idf.sh
# for the full story and the measurements behind it. This script used to
# embed a block of PowerShell that carried its own sequencing and exit-code
# handling; it does not need to.
#
# WHY --diag EXISTS
#
# The Diagnostics app and the on-device test suites are in the SELFTEST build
# and nowhere else - main/CMakeLists.txt excludes apps/diagnostics by folder
# unless CONFIG_LAUNCHER_SELFTEST is set, and adds the suites back only there.
# So using either one means putting that image on the board and leaving it.
#
# Nothing here could do that. test/run_device_tests.sh builds and flashes it,
# but refuses outright under Git Bash (idf.py exits successfully without
# building there, which would silently collect stale results - see its own
# comment), and it exists to read test output back rather than to leave you
# on the image. tools/report_test_results.sh does flash it from Git Bash, and
# then re-flashes build.release in an EXIT trap by design. Both are right
# about their own jobs; neither is "put the diagnostics firmware on the
# device", which is what this flag is.

set -euo pipefail

VARIANT=release

while [ $# -gt 0 ]; do
    case "$1" in
        -d|--diag) VARIANT=diag; shift ;;
        -h|--help) sed -n '2,38p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --)        shift; break ;;
        -*)        echo "unknown option: $1" >&2; exit 2 ;;
        *)         break ;;
    esac
done

COM_PORT="${1:-COM3}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAUNCHER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ -n "${MSYSTEM:-}" ]; then
    DEFAULT_EXPORT='C:\Espressif\esp-idf-v5.5\export.bat'
else
    DEFAULT_EXPORT="${IDF_PATH:-$HOME/esp/esp-idf}/export.sh"
fi
IDF_EXPORT="${2:-$DEFAULT_EXPORT}"

BUILD_DIR="build.$VARIANT"

# So a double-clicked window (which closes the instant the script exits)
# still shows the reason for a failure instead of vanishing on the spot.
trap 'status=$?; if [ $status -ne 0 ]; then echo; echo "=== FAILED (exit $status) ==="; read -r -p "Press Enter to close..." _; fi' EXIT

# shellcheck source=./idf.sh
. "$SCRIPT_DIR/idf.sh"
idf_init "$LAUNCHER_DIR" "$IDF_EXPORT" "$SCRIPT_DIR"

echo "=== Building $BUILD_DIR ==="
if [ "$VARIANT" = diag ]; then
    # BOTH -D flags are needed, and the second is the one that is easy to
    # leave off and hard to notice missing. idf.py's default sdkconfig path
    # is the PROJECT's sdkconfig, not the build directory's, so without
    # -D SDKCONFIG this reads launcher/sdkconfig - which has SELFTEST off -
    # and cheerfully produces a build.diag image with no diagnostics in it.
    # It can even look like it worked, if build.diag/CMakeCache.txt is left
    # over from a run that did pass the flag. SDKCONFIG_DEFAULTS alone does
    # not fix it. (Learned in tools/report_test_results.sh, where the silent
    # version of this cost a capture with no measurements in it.)
    #
    # The semicolon inside SDKCONFIG_DEFAULTS reaches idf.py intact on
    # Windows too: passing arguments through the shim without mangling them
    # is exactly what idf_shim.bat is for, and what ci_check_idf_sh.sh
    # asserts for the POSIX branch.
    idf -B "$BUILD_DIR" \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.diag" \
        -D SDKCONFIG="$BUILD_DIR/sdkconfig" \
        build
else
    idf -B "$BUILD_DIR" build
fi

# A build tool that reports success without producing anything is how this
# project once flashed and measured code it had never built (see
# test/run_device_tests.sh's own note). The exit status is not enough on its
# own, so confirm the artifact is really there.
if [ ! -f "$LAUNCHER_DIR/$BUILD_DIR/launcher.bin" ]; then
    echo "build reported success but produced no binary at" >&2
    echo "  $LAUNCHER_DIR/$BUILD_DIR/launcher.bin" >&2
    exit 1
fi

echo "=== Flashing to $COM_PORT ==="
idf -B "$BUILD_DIR" -p "$COM_PORT" flash

if [ "$VARIANT" = diag ]; then
    echo "=== Done - the DIAGNOSTICS image is on the device ==="
    echo "    It runs the test suites at boot and carries the Diagnostics app."
    echo "    Re-run without --diag to put the release firmware back."
else
    echo "=== Done ==="
fi

# `|| true` because this is the LAST command: with stdin at EOF (piped, or
# redirected from /dev/null in CI) read returns non-zero, which became the
# script's exit status and made the trap above announce a failure over a
# perfectly good flash. The pause is a convenience for double-clickers, not
# a step that can fail.
read -r -p "Press Enter to close..." _ || true
