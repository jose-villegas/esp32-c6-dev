#!/usr/bin/env bash
#
# One-click device performance report: build+flash build.diag, capture the
# self-test run, write a markdown table of just the frame-budget tests
# (scenario, budget, measured, headroom, pass/fail) - generated fresh from
# a real capture and the current source, so it can never go stale the way
# a hand-transcribed copy (like the table in docs/Sand/Architecture.md) can.
#
# Usage:
#   main/apps/sand/tools/report_performance.sh [COM_PORT] [OUT.md] [IDF_EXPORT_PS1]
#
#   COM_PORT        serial port the device is on. Default: COM3.
#   OUT.md          markdown report path. Default:
#                   main/apps/sand/tools/results/performance_<timestamp>.md
#   IDF_EXPORT_PS1  path to ESP-IDF's export.ps1. Default: this
#                   project's usual install location.
#
# Checks COM_PORT actually exists before building anything - see the
# check right below. Restores build.release afterward, regardless of
# outcome - see report_test_results.sh's own top comment for why.

set -euo pipefail

COM_PORT="${1:-COM3}"
OUT_MD="${2:-}"
IDF_EXPORT_PS1="${3:-C:\\Espressif\\esp-idf-v5.5\\export.ps1}"

# Fail in seconds, not in twelve minutes. Without this, a vanished COM
# port was only discovered by esptool AFTER a full cold build.diag build
# ("Could not open COM3 ... FileNotFoundError", 2026-09-02) - the port is
# cheap to check and the build is not. GetPortNames() only enumerates the
# registry, so this never opens or otherwise touches the port itself.
AVAILABLE_PORTS="$(powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \
    "[System.IO.Ports.SerialPort]::GetPortNames() -join ','" 2>&1 | tr -d '\r\n')"
case ",$AVAILABLE_PORTS," in
    *",$COM_PORT,"*) ;;
    *)
        echo "ERROR: $COM_PORT not found."
        echo "Ports currently visible to Windows: ${AVAILABLE_PORTS:-(none)}"
        echo "Plug in the device, or pass the right port as the first argument."
        read -r -p "Press Enter to close..." _ || true
        exit 1
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# This file lives at main/apps/sand/tools/, four levels below launcher/ -
# tools -> sand -> apps -> main -> launcher.
#
# report_performance.py sits beside this script, so it is reached through
# SCRIPT_DIR. sweeps/capture_selftest.py did NOT move and is reached through
# LAUNCHER_DIR: it only resets the device and captures serial output, knows
# nothing about any app, and has four callers - two here, two in the shared
# sweeps. A tool with a consumer outside this app belongs outside it.
LAUNCHER_DIR="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
LAUNCHER_DIR_WIN="$(cd "$LAUNCHER_DIR" && pwd -W 2>/dev/null || echo "$LAUNCHER_DIR")"

# The app's own results dir now that this script lives under the app -
# deleting main/apps/sand/ takes its scratch output with it too.
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
    # Piped or backgrounded runs (background task runners, CI) have no
    # stdin, so `read` fails with nothing to read - and under `set -e`
    # that failure was becoming the LAST command's exit status, which bash
    # then uses as the whole script's exit code, silently turning a clean
    # run into a reported failure. `|| true` swallows that; the explicit
    # `exit "$status"` below is what actually decides the exit code now,
    # so a real failure still propagates regardless of what happens here.
    read -r -p "Press Enter to close..." _ || true
    exit "$status"
}
trap cleanup EXIT

# A generated sdkconfig WINS over the defaults fragments: idf.py only applies
# SDKCONFIG_DEFAULTS when it has to create the file, so editing a fragment
# never reaches an existing build.diag/sdkconfig. That has now silently
# defeated two separate flag changes - the diag watchdog, then
# LAUNCHER_SELFTEST_AUTORUN - each time producing a build that looks right
# and measures nothing. Removing it costs one reconfigure per run, which is
# noise beside the build, and makes the fragments authoritative again.
rm -f "$LAUNCHER_DIR/build.diag/sdkconfig"

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
    # A THIRD fragment, sdkconfig.defaults.diag_autorun, is layered on top:
    # CONFIG_LAUNCHER_SELFTEST_AUTORUN defaults to n, so without it the
    # suites compile in but never run at boot, the device just sits in the
    # launcher, and the capture below waits out its full timeout with no
    # SELFTEST_COMPLETE and no measurements. That is exactly what happened
    # on 2026-08-31: c4979fa made the suites opt-in and taught
    # tools/report_test_results.sh to layer this file, but not this script,
    # which a2daa87 had already moved into the sand app's own tools folder.
    # The two scripts do the same job and must be changed together.
    idf.py -B build.diag -D SDKCONFIG_DEFAULTS=\"sdkconfig.defaults;sdkconfig.defaults.diag;sdkconfig.defaults.diag_autorun\" -D SDKCONFIG=build.diag/sdkconfig build
    if (\$LASTEXITCODE -ne 0) { exit \$LASTEXITCODE }
    idf.py -B build.diag -p '$COM_PORT' flash
    exit \$LASTEXITCODE
"

# The generated config is the only honest witness that the fragments took:
# check the flags the capture actually depends on, and fail loudly here
# rather than after a 300-second timeout with an empty raw file.
for flag in CONFIG_LAUNCHER_SELFTEST CONFIG_LAUNCHER_SELFTEST_AUTORUN; do
    if ! grep -q "^${flag}=y" "$LAUNCHER_DIR/build.diag/sdkconfig"; then
        echo "ERROR: ${flag} is not set in the generated build.diag/sdkconfig -"
        echo "the flashed image would boot without running the suites, and the"
        echo "capture below would time out with no measurements. Aborting."
        exit 1
    fi
done

echo "=== Capturing self-test output ==="
# 1500s, not 300. The suite ran in ~168s for as long as its frame-budget
# fixtures were failing to allocate their grids instantly; once the heap
# was freed on 2026-09-01 and the scenes actually ran, a full pass took
# 1,125,726 ms - 18.8 minutes. A 300s window now times out every run and
# produces a capture with no SELFTEST_COMPLETE in it, which the validator
# below correctly rejects but which costs a full capture cycle to learn.
python "$LAUNCHER_DIR/tools/sweeps/capture_selftest.py" "$RAW_CAPTURE" --port "$COM_PORT" --timeout 1500

# A capture that never finished, crashed, or measured nothing (wrong image,
# suites compiled in but not running) still produces a plausible-looking
# report if fed straight to report_performance.py below - that has already
# cost two full capture cycles and once got mistaken for a fresh result.
# Fail loudly here, before spending any time generating a table from it.
echo "=== Validating capture ==="
python "$LAUNCHER_DIR/tools/sweeps/validate_capture.py" "$RAW_CAPTURE"

echo "=== Generating performance report ==="
# Both halves of a frame: the simulation's budgets live in the sand suite,
# the draw's live in the gfx one. Reporting only the first hid the fact
# that a present costs as much as a step - measured 2026-08-28, when a
# scattered scene turned out to spend a full-screen send every frame.
python "$SCRIPT_DIR/report_performance.py" "$RAW_CAPTURE" "$OUT_MD" \
    --source "$LAUNCHER_DIR/main/apps/sand/suite_sand.c" \
    --source "$LAUNCHER_DIR/test/suites/suite_gfx.c"

echo "=== Report:       $OUT_MD ==="
echo "=== Raw capture:  $RAW_CAPTURE ==="
