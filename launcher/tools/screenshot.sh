#!/bin/sh
#
# Capture whatever the device currently has on screen, over its console's
# serial connection - no SD card, no button on the device: this script sends
# the request and receives the image itself, together with a same-named
# .json snapshot of device state at that exact frame (sensors, memory,
# clock - see screenshot_dump()'s own comment in main/util/screenshot.c).
#
#   ./tools/screenshot.sh                  # auto-detected port, timestamped files
#   ./tools/screenshot.sh -p /dev/ttyACM0
#   ./tools/screenshot.sh -o mine.bmp      # writes mine.bmp and mine.json
#
# The board has no other channel to a host - see main/util/screenshot.h's own
# top comment - so this rides the exact same serial connection monitor.sh and
# idf_monitor use for logs. The firmware must already be running: this does
# not build or flash anything, and it does not reset the board either (see
# screenshot.py's own comment on why that matters here specifically). If
# idf_monitor or another program already has the port open, close it first -
# only one process can hold a serial port at a time.
#
# POSIX sh so this works under Git Bash or MSYS on Windows, and natively on
# Linux and macOS. The actual serial I/O is Python + pyserial (screenshot.py)
# - the same split test/run_device_tests.sh uses (see its own comment, and
# collect_device_results.py's): pyserial ships inside ESP-IDF's environment
# and behaves the same on every platform, which a shell script reading a
# serial port directly does not.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT_DIR="$SCRIPT_DIR/screenshots"

PORT="${ESPPORT:-}"
OUT=""
BAUD=115200
TIMEOUT=90

while [ $# -gt 0 ]; do
    case "$1" in
        -p|--port)    PORT="$2"; shift 2 ;;
        -o|--out)     OUT="$2"; shift 2 ;;
        -b|--baud)    BAUD="$2"; shift 2 ;;
        -t|--timeout) TIMEOUT="$2"; shift 2 ;;
        -h|--help)    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)            echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# Serial devices are named differently on every platform, so guess only when
# the caller has not said - same list monitor.sh and run_device_tests.sh use.
if [ -z "$PORT" ]; then
    for candidate in /dev/ttyACM0 /dev/ttyUSB0 /dev/cu.usbmodem* COM3; do
        if [ -e "$candidate" ]; then PORT="$candidate"; break; fi
    done
    [ -z "$PORT" ] && PORT="COM3"     # Windows COM ports are not filesystem entries
fi

mkdir -p "$OUT_DIR"
if [ -z "$OUT" ]; then
    OUT="$OUT_DIR/screenshot_$(date +%Y%m%d_%H%M%S).bmp"
fi

# pyserial lives in ESP-IDF's environment, so use that interpreter rather
# than whatever python happens to be on PATH - same search
# test/run_device_tests.sh uses for collect_device_results.py.
PYTHON=$(command -v python3 || command -v python || true)
for candidate in "$HOME/.espressif/python_env"/idf*_env/bin/python \
                 "$HOME/.espressif/python_env"/idf*_env/Scripts/python.exe; do
    [ -x "$candidate" ] && PYTHON="$candidate"
done
if [ -z "${PYTHON:-}" ]; then
    echo "no Python found (need pyserial - ESP-IDF's own environment has it)" >&2
    exit 1
fi

echo "==> requesting a screenshot from $PORT"
exec "$PYTHON" "$SCRIPT_DIR/screenshot.py" \
    --port "$PORT" --baud "$BAUD" --timeout "$TIMEOUT" --out "$OUT"
