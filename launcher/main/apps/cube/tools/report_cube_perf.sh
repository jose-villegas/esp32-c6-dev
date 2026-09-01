#!/usr/bin/env bash
#
# One-click cube perf report: build+flash build.diag, capture the self-test
# run, write a markdown comparison of suite_cube_perf.c's with_hud/no_hud/
# full_clear/interlaced variants. tools/report_performance.sh's convenience
# (see its own top comment), pointed at cube_perf's report generator instead
# of sand's frame-budget one - the two suites' output shapes are different
# enough (a multi-line breakdown per run vs. one TEST_ASSERT_LESS_THAN
# budget per test) that they need their own parser, not a shared one.
#
# Usage:
#   main/apps/cube/tools/report_cube_perf.sh [COM_PORT] [OUT.md] [IDF_EXPORT_PS1]
#
#   COM_PORT        serial port the device is on. Default: COM3.
#   OUT.md          markdown report path. Default:
#                   main/apps/cube/tools/results/cube_perf_<timestamp>.md
#   IDF_EXPORT_PS1  path to ESP-IDF's export.ps1. Default: this
#                   project's usual install location.
#
# Restores build.release afterward, regardless of outcome - see
# tools/report_test_results.sh's own top comment for why.
#
# Uses sdkconfig.defaults.diag_autorun (unlike sand's report_performance.sh,
# which does not): without it CONFIG_LAUNCHER_SELFTEST_AUTORUN stays off and
# the suites compile in but never run at boot, so capture_selftest.py's
# 300s wait for SELFTEST_COMPLETE just times out with nothing captured -
# see tools/report_test_results.sh's own comment on this exact trap.

set -euo pipefail

COM_PORT="${1:-COM3}"
OUT_MD="${2:-}"
IDF_EXPORT_PS1="${3:-C:\\Espressif\\esp-idf-v5.5\\export.ps1}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# This file lives at main/apps/cube/tools/, four levels below launcher/ -
# tools -> cube -> apps -> main -> launcher.
LAUNCHER_DIR="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
LAUNCHER_DIR_WIN="$(cd "$LAUNCHER_DIR" && pwd -W 2>/dev/null || echo "$LAUNCHER_DIR")"

# The app's own results dir - deleting main/apps/cube/ takes its scratch
# output with it too, same as sand's.
RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
if [ -z "$OUT_MD" ]; then
    OUT_MD="$RESULTS_DIR/cube_perf_$TIMESTAMP.md"
fi
RAW_CAPTURE="$RESULTS_DIR/cube_perf_${TIMESTAMP}_raw.txt"

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
    idf.py -B build.diag -D SDKCONFIG_DEFAULTS=\"sdkconfig.defaults;sdkconfig.defaults.diag;sdkconfig.defaults.diag_autorun\" -D SDKCONFIG=build.diag/sdkconfig build
    if (\$LASTEXITCODE -ne 0) { exit \$LASTEXITCODE }
    idf.py -B build.diag -p '$COM_PORT' flash
    exit \$LASTEXITCODE
"

echo "=== Capturing self-test output ==="
# cube_perf alone runs three 10s captures plus a quick interlaced one - well
# past run_device_tests.sh's normal ~1s, so this needs real headroom rather
# than the 90s default.
python "$LAUNCHER_DIR/tools/sweeps/capture_selftest.py" "$RAW_CAPTURE" --port "$COM_PORT" --timeout 300

echo "=== Generating cube perf report ==="
python "$SCRIPT_DIR/report_cube_perf.py" "$RAW_CAPTURE" "$OUT_MD"

echo "=== Report:       $OUT_MD ==="
echo "=== Raw capture:  $RAW_CAPTURE ==="
