#!/bin/sh
#
# Run the test suites ON the device, triggered from here.
#
#   ./test/run_device_tests.sh                 # build, flash, collect
#   ./test/run_device_tests.sh -p /dev/ttyACM0
#   ./test/run_device_tests.sh --no-flash      # collect from what is already on the board
#
# Builds the DIAGNOSTICS variant - the firmware plus the test suites - into its
# own directory, flashes it, and reads the results back over the console. Exits
# non-zero if any test failed or if the run never completed.
#
# The normal build is untouched by this. It lives in build/ and contains no
# test code at all; this one lives in build.diag/ with CONFIG_LAUNCHER_SELFTEST
# enabled. Keeping them apart is the point: a release image should not carry
# test scaffolding, and the two never overwrite each other's config.
#
# For the fast loop during development use ./test/run_tests.sh instead, which
# runs the portable suites on this machine in under a second.

set -eu

TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$TEST_DIR/.." && pwd)
BUILD_DIR="$PROJECT_DIR/build.diag"

PORT="${ESPPORT:-}"
DO_FLASH=1
TIMEOUT=60

while [ $# -gt 0 ]; do
    case "$1" in
        -p|--port)   PORT="$2"; shift 2 ;;
        --no-flash)  DO_FLASH=0; shift ;;
        -t|--timeout) TIMEOUT="$2"; shift 2 ;;
        -h|--help)   sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)           echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# Only the build/flash half needs idf.py. Collection is plain Python and
# pyserial (collect_device_results.py, below), so --no-flash must not be
# gated on ESP-IDF being available - that is the entire point of
# --no-flash, and on Windows it is what makes this script usable from Git
# Bash at all.
#
# This check used to run unconditionally, before the --no-flash branch it
# should have been inside. The result was that collecting results failed
# for want of a tool it never invokes, which read as "the whole script
# needs ESP-IDF" and sent people looking for a wrapper they did not need.
if [ "$DO_FLASH" -eq 1 ] && ! command -v idf.py >/dev/null 2>&1; then
    echo "idf.py not on PATH. Source ESP-IDF's export script first:" >&2
    echo "  . \$IDF_PATH/export.sh" >&2
    echo "(or use --no-flash to collect from what is already on the board)" >&2
    exit 1
fi

# ESP-IDF does not support being driven from MSys/Git Bash, and - worse - its
# idf.py prints a warning and then exits SUCCESSFULLY without doing anything.
# Trusting that would flash nothing and collect results from whatever firmware
# happened to already be on the board, reporting a confident pass for code that
# was never built. Refuse instead.
if [ -n "${MSYSTEM:-}" ] && [ "$DO_FLASH" -eq 1 ]; then
    cat >&2 <<MSG
idf.py cannot be driven from MSys/Git Bash: ESP-IDF does not support it, and it
exits successfully without building, which would silently collect stale results.

On Windows, build and flash from PowerShell or cmd:

  idf.py -B build.diag \
         -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.diag" \
         -D SDKCONFIG=build.diag/sdkconfig build
  idf.py -B build.diag -p $PORT flash

then collect the results from here:

  ./test/run_device_tests.sh --no-flash -p $PORT

On Linux and macOS this script runs the whole flow itself.
MSG
    exit 2
fi

# Guess a port only if the caller has not named one. Serial devices are named
# differently on every platform.
if [ -z "$PORT" ]; then
    for candidate in /dev/ttyACM0 /dev/ttyUSB0 /dev/cu.usbmodem* COM3; do
        if [ -e "$candidate" ]; then PORT="$candidate"; break; fi
    done
    [ -z "$PORT" ] && PORT="COM3"     # Windows COM ports are not filesystem entries
fi

cd "$PROJECT_DIR"

if [ "$DO_FLASH" -eq 1 ]; then
    echo "==> building diagnostics firmware (tests included)"
    idf.py -B "$BUILD_DIR" \
           -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.diag" \
           -D SDKCONFIG="$BUILD_DIR/sdkconfig" \
           build

    # Never trust the exit code alone. A build tool that no-ops successfully is
    # exactly how this script once reported a pass for code it never built, so
    # confirm the artifact is really there and really fresh.
    if [ ! -f "$BUILD_DIR/launcher.bin" ]; then
        echo "build reported success but produced no binary at" >&2
        echo "  $BUILD_DIR/launcher.bin" >&2
        exit 1
    fi
    newest_src=$(find main components -name '*.c' -o -name '*.h' 2>/dev/null                  | while read -r f; do [ "$f" -nt "$BUILD_DIR/launcher.bin" ] && echo "$f"; done | head -n 1)
    if [ -n "$newest_src" ]; then
        echo "build is stale: $newest_src is newer than the binary" >&2
        exit 1
    fi

    echo "==> flashing to $PORT"
    idf.py -B "$BUILD_DIR" -p "$PORT" flash
fi

# pyserial lives in ESP-IDF's environment, so use that interpreter rather than
# whatever python happens to be on PATH.
PYTHON=$(command -v python3 || command -v python || true)
for candidate in "$HOME/.espressif/python_env"/idf*_env/bin/python \
                 "$HOME/.espressif/python_env"/idf*_env/Scripts/python.exe; do
    [ -x "$candidate" ] && PYTHON="$candidate"
done
if [ -z "${PYTHON:-}" ]; then
    echo "no Python found for reading the console" >&2
    exit 1
fi

echo "==> collecting results from $PORT"
exec "$PYTHON" "$TEST_DIR/collect_device_results.py" \
    --port "$PORT" --timeout "$TIMEOUT"
