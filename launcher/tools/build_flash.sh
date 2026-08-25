#!/usr/bin/env bash
#
# Build the launcher's release firmware and flash it to the device.
#
# Usage:
#   tools/build_flash.sh [COM_PORT] [IDF_EXPORT]
#
#   COM_PORT    serial port the device is on. Default: COM3.
#   IDF_EXPORT  path to ESP-IDF's export script - export.bat on Windows,
#               export.sh elsewhere. Default: this project's usual install.
#
# Run from anywhere (it cds to launcher/ itself); double-click from Explorer
# if .sh is associated with Git Bash, or right-click launcher/tools/ ->
# "Git Bash Here" -> `./build_flash.sh`.
#
# All the logic here is POSIX sh. On Windows the ESP-IDF calls go through
# tools/idf_shim.bat, which exists only to delete MSYSTEM - see tools/idf.sh
# for the full story and the measurements behind it. This script used to
# embed a block of PowerShell that carried its own sequencing and exit-code
# handling; it does not need to.

set -euo pipefail

COM_PORT="${1:-COM3}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAUNCHER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ -n "${MSYSTEM:-}" ]; then
    DEFAULT_EXPORT='C:\Espressif\esp-idf-v5.5\export.bat'
else
    DEFAULT_EXPORT="${IDF_PATH:-$HOME/esp/esp-idf}/export.sh"
fi
IDF_EXPORT="${2:-$DEFAULT_EXPORT}"

# So a double-clicked window (which closes the instant the script exits)
# still shows the reason for a failure instead of vanishing on the spot.
trap 'status=$?; if [ $status -ne 0 ]; then echo; echo "=== FAILED (exit $status) ==="; read -r -p "Press Enter to close..." _; fi' EXIT

# shellcheck source=./idf.sh
. "$SCRIPT_DIR/idf.sh"
idf_init "$LAUNCHER_DIR" "$IDF_EXPORT" "$SCRIPT_DIR"

echo "=== Building build.release ==="
idf -B build.release build

echo "=== Flashing to $COM_PORT ==="
idf -B build.release -p "$COM_PORT" flash

echo "=== Done ==="

# `|| true` because this is the LAST command: with stdin at EOF (piped, or
# redirected from /dev/null in CI) read returns non-zero, which became the
# script's exit status and made the trap above announce a failure over a
# perfectly good flash. The pause is a convenience for double-clickers, not
# a step that can fail.
read -r -p "Press Enter to close..." _ || true
