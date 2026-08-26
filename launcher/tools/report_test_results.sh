#!/usr/bin/env bash
#
# One-click device self-test report: build+flash build.diag, capture the
# self-test run, write a markdown results report. build_flash.sh's
# release-firmware convenience, for the diagnostics build instead.
#
# Usage:
#   tools/report_test_results.sh [COM_PORT] [OUT.md] [IDF_EXPORT_PS1]
#
#   COM_PORT        serial port the device is on. Default: COM3.
#   OUT.md          markdown report path. Default:
#                   tools/results/test_results_<timestamp>.md
#   IDF_EXPORT_PS1  path to ESP-IDF's export.ps1. Default: this
#                   project's usual install location.
#
# Restores build.release afterward, regardless of outcome, in a trap -
# same discipline the sweep scripts already use, so the device is never
# left stuck on the self-test build just because this script failed
# partway through.
#
# Same Git-Bash/idf.py limitation as build_flash.sh applies here too -
# see that script's own top comment, or docs/Sand/Architecture.md's
# "The Git Bash trap" section, for the full story.

set -euo pipefail

COM_PORT="${1:-COM3}"
OUT_MD="${2:-}"
IDF_EXPORT_PS1="${3:-C:\\Espressif\\esp-idf-v5.5\\export.ps1}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAUNCHER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LAUNCHER_DIR_WIN="$(cd "$LAUNCHER_DIR" && pwd -W 2>/dev/null || echo "$LAUNCHER_DIR")"

RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
if [ -z "$OUT_MD" ]; then
    OUT_MD="$RESULTS_DIR/test_results_$TIMESTAMP.md"
fi
RAW_CAPTURE="$RESULTS_DIR/test_results_${TIMESTAMP}_raw.txt"

cleanup() {
    local status=$?
    echo "=== Restoring build.release ==="
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
        Remove-Item Env:\MSYSTEM -ErrorAction SilentlyContinue
        & '$IDF_EXPORT_PS1' | Out-Null
        Set-Location '$LAUNCHER_DIR_WIN'
        idf.py -B build.release build
        idf.py -B build.release -p '$COM_PORT' flash
    " || echo "WARNING: could not restore build.release - device may still be on build.diag"
    if [ "$status" -ne 0 ]; then
        echo
        echo "=== FAILED (exit $status) ==="
    fi
    read -r -p "Press Enter to close..." _
}
trap cleanup EXIT

echo "=== Building and flashing build.diag to $COM_PORT ==="
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
    Remove-Item Env:\MSYSTEM -ErrorAction SilentlyContinue
    & '$IDF_EXPORT_PS1' | Out-Null
    Set-Location '$LAUNCHER_DIR_WIN'
    # idf.py's default sdkconfig path is the project's own sdkconfig, not
    # the build directory's, so a bare build here reads launcher/sdkconfig
    # - which has CONFIG_LAUNCHER_SELFTEST off - and produces an image
    # with no self-test in it. This only looked like it worked because
    # build.diag/CMakeCache.txt, left behind by run_device_tests.sh,
    # already remembered the right path; a fresh checkout has no such
    # cache, and the failure is silent - the capture below just comes
    # back with no measurements in it. SDKCONFIG_DEFAULTS alone would not
    # fix this; SDKCONFIG is the flag that actually points the build at
    # build.diag/sdkconfig. Kept to ONE line on purpose: this PowerShell
    # block is a bash double-quoted string, where PowerShell backtick
    # line-continuations are bash command substitution - a parse error.
    idf.py -B build.diag -D SDKCONFIG_DEFAULTS=\"sdkconfig.defaults;sdkconfig.defaults.diag\" -D SDKCONFIG=build.diag/sdkconfig build
    if (\$LASTEXITCODE -ne 0) { exit \$LASTEXITCODE }
    idf.py -B build.diag -p '$COM_PORT' flash
    exit \$LASTEXITCODE
"

echo "=== Capturing self-test output ==="
python "$SCRIPT_DIR/sweeps/capture_selftest.py" "$RAW_CAPTURE" --port "$COM_PORT"

echo "=== Generating report ==="
# report_test_results.py exits 1 when the capture contains ANY failing
# test - a normal, expected outcome (this project has a known baseline
# of 2 pre-existing failures, see docs/Sand/Architecture.md), not a
# failure of THIS script. Read the report to judge that, not this exit
# code - `|| true` keeps `set -e` from treating it as fatal.
python "$SCRIPT_DIR/report_test_results.py" "$RAW_CAPTURE" "$OUT_MD" || true

echo "=== Report:       $OUT_MD ==="
echo "=== Raw capture:  $RAW_CAPTURE ==="
