#!/usr/bin/env bash
#
# One-click boot_anim perf report: build+flash build.diag (autorun OFF, so
# the shell - and its RUNSUITE listener, see main/util/screenshot.c - comes
# up in seconds instead of after a full alphabetical suite run), trigger
# just suite_boot_anim_perf.c via RUNSUITE, capture its output, and write a
# markdown report of the six-checkpoint breakdown.
#
# Needs the RUNSUITE console command - see main/util/screenshot.c and
# test/suites.c's suites_run_one(). Without it this has nothing to send and
# would just capture a plain shell boot with no perf data in it.
#
# Usage:
#   tools/report_boot_anim_perf.sh [COM_PORT] [OUT.md] [IDF_EXPORT_PS1]
#
#   COM_PORT        serial port the device is on. Default: COM3.
#   OUT.md          markdown report path. Default:
#                   tools/results/boot_anim_perf_<timestamp>.md
#   IDF_EXPORT_PS1  path to ESP-IDF's export.ps1. Default: this
#                   project's usual install location.
#
# Restores build.release afterward, regardless of outcome - see
# tools/report_test_results.sh's own top comment for why.

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
    OUT_MD="$RESULTS_DIR/boot_anim_perf_$TIMESTAMP.md"
fi
RAW_CAPTURE="$RESULTS_DIR/boot_anim_perf_${TIMESTAMP}_raw.txt"

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

echo "=== Building and flashing build.diag (autorun OFF) to $COM_PORT ==="
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
    Remove-Item Env:\MSYSTEM -ErrorAction SilentlyContinue
    & '$IDF_EXPORT_PS1' | Out-Null
    Set-Location '$LAUNCHER_DIR_WIN'
    idf.py -B build.diag -D SDKCONFIG_DEFAULTS=\"sdkconfig.defaults;sdkconfig.defaults.diag\" -D SDKCONFIG=build.diag/sdkconfig build
    if (\$LASTEXITCODE -ne 0) { exit \$LASTEXITCODE }
    idf.py -B build.diag -p '$COM_PORT' flash
    exit \$LASTEXITCODE
"

echo "=== Triggering RUNSUITE run_boot_anim_perf_suite and capturing output ==="
python "$LAUNCHER_DIR/tools/sweeps/capture_runsuite.py" run_boot_anim_perf_suite \
    "$RAW_CAPTURE" --port "$COM_PORT" --timeout 60

echo "=== Generating boot_anim perf report ==="
python "$SCRIPT_DIR/report_boot_anim_perf.py" "$RAW_CAPTURE" "$OUT_MD"

echo "=== Report:       $OUT_MD ==="
echo "=== Raw capture:  $RAW_CAPTURE ==="
