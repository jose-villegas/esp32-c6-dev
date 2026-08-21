#!/bin/sh
#
# Build and run the host test suite.
#
#   ./test/run_tests.sh              # everything
#   ./test/run_tests.sh touch        # only suites matching "touch"
#   ./test/run_tests.sh -q           # just the summaries
#   CC=clang ./test/run_tests.sh     # a specific compiler
#
# These compile for THIS machine, not the ESP32, and run in milliseconds. That
# is the point: red-green-refactor is only practical with instant feedback, and
# building plus flashing firmware takes about ninety seconds.
#
# Only hardware-free logic can be tested this way. Anything touching the panel,
# I2C or FreeRTOS belongs behind an interface with the logic extracted into a
# pure module - main/touch_fsm.c is the pattern to copy.
#
# POSIX sh on purpose: this runs under Git Bash or MSYS on Windows, and
# natively on Linux and macOS.

set -eu

TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MAIN_DIR=$(CDPATH= cd -- "$TEST_DIR/../main" && pwd)
BUILD_DIR="$TEST_DIR/build"

FILTER=""
QUIET=0
for arg in "$@"; do
    case "$arg" in
        -q|--quiet) QUIET=1 ;;
        -h|--help)  sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*)         echo "unknown option: $arg" >&2; exit 2 ;;
        *)          FILTER="$arg" ;;
    esac
done

# --- find a compiler -------------------------------------------------------
# $CC wins if set. Otherwise take whatever is on PATH. The last candidate is
# where winget puts MinGW on Windows, which is not added to PATH for shells
# that were already open when it was installed.
find_cc() {
    if [ -n "${CC:-}" ]; then echo "$CC"; return; fi
    for c in cc gcc clang; do
        if command -v "$c" >/dev/null 2>&1; then echo "$c"; return; fi
    done
    winlibs="$LOCALAPPDATA/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin/gcc.exe"
    if [ -n "${LOCALAPPDATA:-}" ] && [ -x "$winlibs" ]; then echo "$winlibs"; return; fi
    return 1
}

if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    echo "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
    echo "  Debian:  sudo apt install build-essential" >&2
    echo "  macOS:   xcode-select --install" >&2
    exit 1
fi

# Warnings are errors here. A host build catches mistakes the target build will
# happily miss, and being strict costs nothing in tests.
CFLAGS="-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -g -O1"

mkdir -p "$BUILD_DIR"

failed=""
ran=0

for suite in "$TEST_DIR"/src/test_*.c; do
    [ -e "$suite" ] || { echo "No test suites found in $TEST_DIR/src" >&2; exit 1; }

    name=$(basename "$suite" .c)
    case "$name" in
        *"$FILTER"*) ;;
        *) continue ;;
    esac

    # Convention: test_<unit>.c exercises main/<unit>.c. Linking only that unit
    # keeps a suite from quietly depending on unrelated code, and means adding
    # a suite needs no edit here.
    unit_name=${name#test_}
    unit="$MAIN_DIR/$unit_name.c"

    sources="$suite $TEST_DIR/framework/unity.c"
    if [ -f "$unit" ]; then
        sources="$sources $unit"
    else
        echo "note: $name has no matching $unit_name.c; building header-only" >&2
    fi

    out="$BUILD_DIR/$name"
    # shellcheck disable=SC2086
    if ! "$CC_BIN" $CFLAGS -I "$MAIN_DIR" -I "$TEST_DIR/framework" $sources -o "$out"; then
        echo "COMPILE FAILED: $name" >&2
        failed="$failed $name"
        continue
    fi

    # MinGW appends .exe; everywhere else the plain name is produced.
    [ -x "$out" ] || out="$out.exe"

    ran=$((ran + 1))
    if [ "$QUIET" -eq 1 ]; then
        "$out" | tail -n 3 || failed="$failed $name"
    else
        "$out" || failed="$failed $name"
    fi
    echo ""
done

if [ "$ran" -eq 0 ]; then
    echo "No suites matched '$FILTER'" >&2
    exit 1
fi

if [ -n "$failed" ]; then
    echo "FAILING SUITES:$failed" >&2
    exit 1
fi

echo "All suites passed."
