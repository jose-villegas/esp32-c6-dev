#!/usr/bin/env bash
#
# One-click device performance report: build+flash build.diag, capture the
# self-test run, write a markdown table of just the frame-budget tests
# (scenario, budget, measured, headroom, pass/fail) - generated fresh from
# a real capture and the current source, so it can never go stale the way
# a hand-transcribed copy (like the table in docs/Sand/Architecture.md) can.
#
# Usage:
#   main/apps/sand/tools/report_performance.sh [--no-restore] \
#       [--baseline REPORT.md] [COM_PORT] [OUT.md] [IDF_EXPORT_PS1]
#
#   COM_PORT        serial port the device is on. Default: COM3.
#   OUT.md          markdown report path. Default:
#                   main/apps/sand/tools/results/performance_<timestamp>.md
#   IDF_EXPORT_PS1  path to ESP-IDF's export.ps1. Default: this
#                   project's usual install location.
#   --no-restore    skip rebuilding/reflashing build.release afterward -
#                   the device is left on build.diag. Restoring costs a
#                   ~3-4 minute build+flash plus a second ~90s ESP-IDF
#                   activation on EVERY run; back-to-back candidate
#                   captures only need it once, at the end of a session.
#   --baseline REPORT.md
#                   after generating the report, run compare_reports.py
#                   --verdict against this earlier report and print its
#                   verdict line - the fourth command of the read-a-
#                   capture ritual below, run for you.
#
# Checks COM_PORT actually exists before building anything - see the
# check right below. Restores build.release afterward unless told not
# to, regardless of outcome otherwise - see report_test_results.sh's own
# top comment for why.

set -euo pipefail

NO_RESTORE=0
BASELINE=""
ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --no-restore)
            NO_RESTORE=1
            shift
            ;;
        --baseline)
            if [ $# -lt 2 ]; then
                echo "ERROR: --baseline requires a path" >&2
                exit 1
            fi
            BASELINE="$2"
            shift 2
            ;;
        --baseline=*)
            BASELINE="${1#--baseline=}"
            shift
            ;;
        -*)
            echo "ERROR: unknown flag: $1" >&2
            exit 1
            ;;
        *)
            ARGS+=("$1")
            shift
            ;;
    esac
done

COM_PORT="${ARGS[0]:-COM3}"
OUT_MD="${ARGS[1]:-}"
IDF_EXPORT_PS1="${ARGS[2]:-C:\\Espressif\\esp-idf-v5.5\\export.ps1}"

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
    if [ "$NO_RESTORE" -eq 1 ]; then
        echo "=== --no-restore: leaving the device on build.diag ==="
        echo "Reminder: the device is still running the self-test image, not"
        echo "build.release. Run this script once without --no-restore, or"
        echo "reflash build.release yourself, before treating it as normal again."
    else
        echo "=== Restoring build.release ==="
        powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
            Remove-Item Env:\MSYSTEM -ErrorAction SilentlyContinue
            & '$IDF_EXPORT_PS1' | Out-Null
            Set-Location '$LAUNCHER_DIR_WIN'
            idf.py -B build.release build
            idf.py -B build.release -p '$COM_PORT' flash
        " || echo "WARNING: could not restore build.release - device may still be on build.diag"
    fi
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

# The measured (not budget) column of one row of report_performance.py's
# table: "| `name` | budget | measured | headroom | status |". Anchored
# at the start of the line so it only matches an actual table row, not
# `name` appearing in one of the report's prose bullet lists (a control
# that did not run this capture is named there instead, with no pipes at
# all - this correctly prints nothing for that case, not the wrong field).
extract_measured() {
    local name="$1" report="$2"
    # `|| true`: awk exits nonzero if $report can't even be opened, and
    # that failure inside a `var=$(...)` assignment would otherwise abort
    # the whole script under `set -e` - the same false-failure class as
    # the heap parsing above, just triggered by a missing file instead of
    # an unexpected log line.
    awk -F'|' -v name="$name" '
        $0 ~ "^\\| *`" name "`" { v = $4; gsub(/^[ \t]+|[ \t]+$/, "", v); print v; exit }
    ' "$report" 2>/dev/null || true
}

# The four-command ritual from docs/Sand/Perf-Round-Guide.md's "Reading a
# capture" section, run here instead of left to the operator - it was
# already being typed by hand five times in two days. Free heap first
# (a short heap means every frame-budget fixture failed to allocate and
# the whole capture measured nothing, see the guide's table), then the
# two liquid-free controls (their value-pair tells a real regression from
# ordinary flash-layout noise before reading anything else).
print_summary() {
    local raw="$1" report="$2"
    echo "=== Summary ==="
    local heap_line heap
    heap_line="$(grep -m1 "free heap after framebuffer" "$raw" || true)"
    if [ -z "$heap_line" ]; then
        echo "WARNING: no 'free heap after framebuffer' line found in $raw"
    else
        # `|| true`: under `set -e`, a `grep -o` that matches nothing
        # inside a `var=$(...)` assignment aborts the whole script right
        # here, mid-summary - a log-line wording change would then read
        # as a capture failure (cleanup() sees a nonzero status) even
        # though the capture and report both genuinely succeeded. Exactly
        # the false-failure this script's exit code was just fixed for.
        heap="$(printf '%s\n' "$heap_line" | grep -o '[0-9]\+ bytes' | grep -o '[0-9]\+' || true)"
        echo "free heap after framebuffer: ${heap:-?} bytes"
        if [ -n "$heap" ] && [ "$heap" -lt 50000 ]; then
            echo "WARNING: free heap ($heap bytes) is below ~50,000 - frame-budget"
            echo "fixtures likely failed to allocate their grids and measured"
            echo "nothing this run. See docs/Sand/Perf-Round-Guide.md's free-heap table."
        fi
    fi
    local ctrl v
    for ctrl in test_a_full_size_step_fits_in_the_frame_budget \
                test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget; do
        v="$(extract_measured "$ctrl" "$report")"
        echo "control $ctrl: ${v:-not measured this capture} us"
    done
}

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

print_summary "$RAW_CAPTURE" "$OUT_MD"

if [ -n "$BASELINE" ]; then
    echo "=== Comparing against baseline: $BASELINE ==="
    # A non-win verdict (NO, or an error like a missing control row) is a
    # normal outcome to print and read, not a failure of THIS script -
    # `|| true` keeps `set -e` from treating compare_reports.py's exit 1
    # as fatal, same reasoning as report_test_results.sh's own `|| true`
    # around a report generator whose exit code means something else.
    python "$SCRIPT_DIR/compare_reports.py" --verdict "$BASELINE" "$OUT_MD" || true
fi
