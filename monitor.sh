#!/bin/sh
#
# Attach to the device's serial console, skipping idf.py's ~90 s environment
# activation.
#
#   ./monitor.sh                       # launcher build, auto-detected port
#   ./monitor.sh -p /dev/ttyACM0       # explicit port
#   ./monitor.sh -e path/to/other.elf  # a different build
#
# Passing the .elf matters: it carries the debug symbols that turn a crash
# address into a file and line number. Ctrl+] quits.
#
# POSIX sh so this works under Git Bash or MSYS on Windows and natively
# elsewhere. Anything machine-specific can be overridden by environment:
# IDF_PATH, IDF_PYTHON, ESPPORT.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ELF="$ROOT/launcher/build/launcher.elf"
PORT="${ESPPORT:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        -p|--port) PORT="$2"; shift 2 ;;
        -e|--elf)  ELF="$2";  shift 2 ;;
        -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)         echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# --- locate ESP-IDF --------------------------------------------------------
if [ -z "${IDF_PATH:-}" ]; then
    for candidate in "$HOME/esp/esp-idf" "/c/Espressif/esp-idf-v5.5" \
                     "C:/Espressif/esp-idf-v5.5" "/opt/esp-idf"; do
        if [ -d "$candidate" ]; then IDF_PATH="$candidate"; break; fi
    done
fi
if [ -z "${IDF_PATH:-}" ] || [ ! -d "$IDF_PATH" ]; then
    echo "ESP-IDF not found. Set IDF_PATH to your installation." >&2
    exit 1
fi
export IDF_PATH

# --- locate the IDF python -------------------------------------------------
# idf_monitor needs the environment IDF created, not the system interpreter.
# Reverse-sorted so the newest environment wins: a machine with several IDF
# versions installed would otherwise get idf5.4 ahead of idf5.5 alphabetically,
# and silently monitor with the wrong toolchain.
if [ -z "${IDF_PYTHON:-}" ]; then
    IDF_PYTHON=$(ls -d "$HOME/.espressif/python_env"/idf*_env/bin/python \
                       "$HOME/.espressif/python_env"/idf*_env/Scripts/python.exe \
                 2>/dev/null | sort -r | head -n 1 || true)
fi
if [ -z "${IDF_PYTHON:-}" ] || [ ! -x "$IDF_PYTHON" ]; then
    echo "ESP-IDF's Python environment not found. Set IDF_PYTHON, or run" >&2
    echo "the IDF install script to create it." >&2
    exit 1
fi

# --- pick a port -----------------------------------------------------------
# Serial devices are named differently on every platform, so guess only when
# the caller has not said.
if [ -z "$PORT" ]; then
    for candidate in /dev/ttyACM0 /dev/ttyUSB0 /dev/cu.usbmodem* COM3; do
        if [ -e "$candidate" ]; then PORT="$candidate"; break; fi
    done
    # Windows COM ports are not filesystem entries, so fall back to COM3.
    [ -z "$PORT" ] && PORT="COM3"
fi

if [ ! -f "$ELF" ]; then
    echo "No ELF at $ELF" >&2
    echo "Build it first: cd launcher && idf.py build" >&2
    exit 1
fi

# The toolchain provides addr2line, which turns crash addresses into source
# locations. Without it the monitor still runs, just less usefully.
TOOLCHAIN=$(find "$HOME/.espressif/tools/riscv32-esp-elf" -maxdepth 4 -type d \
            -name bin 2>/dev/null | head -n 1 || true)
[ -n "$TOOLCHAIN" ] && PATH="$TOOLCHAIN:$PATH" && export PATH

exec "$IDF_PYTHON" "$IDF_PATH/tools/idf_monitor.py" \
    -p "$PORT" -b 115200 \
    --toolchain-prefix riscv32-esp-elf- \
    --target esp32c6 \
    "$ELF"
