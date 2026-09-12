#!/bin/sh
#
# Runs Cppcheck's MISRA C:2012 addon (plus its native defect checks) over
# app source, using a real ESP-IDF compile_commands.json so include paths
# and macros resolve correctly - a standalone run without one produces
# mostly noise (unresolved types make almost everything look like a 10.x
# essential-type violation).
#
# This is report-only: it always exits 0. The sand app alone currently
# turns up ~1200 MISRA style findings (dominated by 10.4, 12.1 and 15.5 -
# see below), so gating CI on this before triage would just be a wall no
# one reads. Flip EXIT_ON_FINDINGS below once a rule set has been chosen
# and the backlog triaged.
#
# Usage:
#   tools/misra_check.sh [build_dir] [file_filter]
#
#   build_dir     defaults to build.dev - must have compile_commands.json
#                 (run `idf.py build` first if it doesn't)
#   file_filter   cppcheck --file-filter glob, defaults to the sand app
#                 (pass "*/main/*" for the whole project, minus vendored
#                 dependencies)
#
#   MISRA_JOBS             parallel analyses, defaults to 2 and may not
#                          exceed 4 (each job also runs the Python addon)
#   MISRA_TIMEOUT_SECONDS  whole-scan deadline, defaults to 900 seconds
#
# NEVER pass a bare "*". --file-filter controls what cppcheck actually
# ANALYZES, not just what gets reported -- "*" matches every translation
# unit in compile_commands.json, and on this repo that's ~1800 of them,
# ~1785 vendored (788 alone are LVGL) and none of them main/ code worth a
# finding. That ran cppcheck's MISRA addon over the whole ESP-IDF SDK and
# LVGL for nothing, cost over 12GB of RAM, and had to be killed by hand
# after running for a long time with no end in sight. "*/main/*" scopes
# analysis to the ~28 real translation units under main/ instead -- use
# that for a whole-project scan, not "*".
#
# Output goes to tools/results/misra_<file_filter-ish>.txt (gitignored).

set -eu

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
LAUNCHER_DIR="$(cd "$TOOLS_DIR/.." && pwd)"

BUILD_DIR="${1:-build.dev}"
FILE_FILTER="${2:-*/main/apps/sand/*}"
JOBS="${MISRA_JOBS:-2}"
TIMEOUT_SECONDS="${MISRA_TIMEOUT_SECONDS:-900}"

case "$FILE_FILTER" in
    */main/*) ;;
    *)
        echo "Refusing unsafe file filter '$FILE_FILTER'. It must be scoped under main/." >&2
        exit 2
        ;;
esac

case "$JOBS" in
    ''|*[!0-9]*)
        echo "MISRA_JOBS must be an integer from 1 to 4." >&2
        exit 2
        ;;
esac
if [ "$JOBS" -lt 1 ] || [ "$JOBS" -gt 4 ]; then
    echo "MISRA_JOBS must be from 1 to 4; each job also runs the Python addon." >&2
    exit 2
fi

case "$TIMEOUT_SECONDS" in
    ''|*[!0-9]*)
        echo "MISRA_TIMEOUT_SECONDS must be a positive integer." >&2
        exit 2
        ;;
esac
if [ "$TIMEOUT_SECONDS" -lt 1 ]; then
    echo "MISRA_TIMEOUT_SECONDS must be a positive integer." >&2
    exit 2
fi

COMPILE_COMMANDS="$LAUNCHER_DIR/$BUILD_DIR/compile_commands.json"
if [ ! -f "$COMPILE_COMMANDS" ]; then
    echo "No compile_commands.json in $BUILD_DIR/ - run 'idf.py build' there first." >&2
    echo "  (tools/idf.sh, or: cd $LAUNCHER_DIR && idf.py -B $BUILD_DIR build)" >&2
    exit 1
fi

if ! command -v cppcheck >/dev/null 2>&1; then
    echo "cppcheck not found on PATH. It's free/GPLv3 - install it:" >&2
    echo "  Windows: winget install cppcheck" >&2
    echo "  macOS:   brew install cppcheck" >&2
    echo "  Linux:   apt-get install cppcheck (or your distro's package)" >&2
    exit 1
fi

# The misra.py addon ships inside Cppcheck's install but isn't always on a
# path cppcheck resolves by name alone (e.g. the winget/mingw64 package on
# Windows). Search common share/ locations before giving up on plain
# "misra" and letting cppcheck fail with its own "addon not found" error.
ADDON="misra"
for candidate in \
    "$(command -v cppcheck 2>/dev/null | sed 's|/bin/cppcheck.*||')/share/Cppcheck/addons/misra.py" \
    /usr/share/cppcheck/addons/misra.py \
    /usr/local/share/Cppcheck/addons/misra.py
do
    if [ -f "$candidate" ]; then
        ADDON="$candidate"
        break
    fi
done

RESULTS_DIR="$TOOLS_DIR/results"
mkdir -p "$RESULTS_DIR"
safe_name="$(echo "$FILE_FILTER" | tr -c 'A-Za-z0-9_' '_')"
REPORT="$RESULTS_DIR/misra_${safe_name}.txt"

if command -v timeout >/dev/null 2>&1; then
    TIMEOUT=timeout
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT=gtimeout
else
    echo "GNU timeout is required so a bad addon rule cannot run indefinitely." >&2
    echo "  macOS: brew install coreutils" >&2
    exit 1
fi

echo "Scanning '$FILE_FILTER' against $BUILD_DIR/compile_commands.json ($JOBS jobs, ${TIMEOUT_SECONDS}s timeout)..."
cd "$LAUNCHER_DIR"
set +e
"$TIMEOUT" --signal=TERM --kill-after=10s "${TIMEOUT_SECONDS}s" cppcheck \
    --project="$BUILD_DIR/compile_commands.json" \
    --file-filter="$FILE_FILTER" \
    -D__CPPCHECK__=1 \
    -j "$JOBS" \
    --enable=warning,style,performance,portability \
    --addon="$ADDON" \
    --inline-suppr \
    --suppress=missingIncludeSystem \
    -q \
    --output-file="$REPORT" 2>&1
scan_status=$?
set -e

if [ "$scan_status" -eq 124 ] || [ "$scan_status" -eq 137 ]; then
    echo "Cppcheck exceeded the ${TIMEOUT_SECONDS}s deadline; partial report: $REPORT" >&2
    exit 2
fi
if [ "$scan_status" -ne 0 ]; then
    echo "Cppcheck failed with exit $scan_status; partial report: $REPORT" >&2
    exit "$scan_status"
fi

total="$(wc -l <"$REPORT" | tr -d ' ')"
misra_count="$(grep -c 'misra-c2012-' "$REPORT" 2>/dev/null || true)"
other_count="$(grep -cE ': (error|warning):' "$REPORT" 2>/dev/null || true)"

echo ""
echo "Report: $REPORT ($total lines)"
echo "  MISRA findings:        ${misra_count:-0}"
echo "  native error/warning:  ${other_count:-0}"
echo ""
echo "Top MISRA rules hit:"
grep -oE 'misra-c2012-[0-9.]+' "$REPORT" 2>/dev/null | sort | uniq -c | sort -rn | head -10 || true
