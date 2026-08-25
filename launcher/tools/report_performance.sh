#!/usr/bin/env bash
#
# One-click device performance report: build+flash build.diag, capture the
# self-test run, write a markdown table of just the frame-budget tests
# (scenario, budget, measured, headroom, pass/fail) - generated fresh from
# a real capture and the current source, so it can never go stale the way
# a hand-transcribed copy (like the table in docs/Sand/Architecture.md) can.
#
# Usage:
#   tools/report_performance.sh [COM_PORT] [OUT.md] [IDF_EXPORT_PS1]
#
#   COM_PORT        serial port the device is on. Default: COM3.
#   OUT.md          markdown report path. Default:
#                   tools/results/performance_<timestamp>.md
#   IDF_EXPORT_PS1  path to ESP-IDF's export.ps1. Default: this
#                   project's usual install location.
#
# Restores build.release afterward, regardless of outcome - see
# report_test_results.sh's own top comment for why.

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
    OUT_MD="$RESULTS_DIR/performance_$TIMESTAMP.md"
fi
RAW_CAPTURE="$RESULTS_DIR/performance_${TIMESTAMP}_raw.txt"

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
    idf.py -B build.diag build
    if (\$LASTEXITCODE -ne 0) { exit \$LASTEXITCODE }
    idf.py -B build.diag -p '$COM_PORT' flash
    exit \$LASTEXITCODE
"

echo "=== Capturing self-test output ==="
python "$SCRIPT_DIR/sweeps/capture_selftest.py" "$RAW_CAPTURE" --port "$COM_PORT"

echo "=== Generating performance report ==="
python "$SCRIPT_DIR/report_performance.py" "$RAW_CAPTURE" "$OUT_MD" \
    --source "$LAUNCHER_DIR/main/apps/sand/suite_sand.c"

echo "=== Report:       $OUT_MD ==="
echo "=== Raw capture:  $RAW_CAPTURE ==="
