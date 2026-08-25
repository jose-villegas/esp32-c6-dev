#!/usr/bin/env bash
#
# Build the launcher's release firmware and flash it to the device - the
# exact idf.py sequence run by hand throughout this session: source
# ESP-IDF's environment, build build.release, flash it.
#
# Delegates the actual ESP-IDF work to PowerShell, not bash - ESP-IDF's
# own export.sh refuses to run under Git Bash at all ("MSys/Mingw is not
# supported. Please follow the getting started guide..."), checked via a
# flat `'MSYSTEM' in os.environ` in idf_tools.py. That is not just a
# bash-vs-PowerShell problem, either: `MSYSTEM` rides along even into a
# `powershell.exe` child process launched from this script (confirmed -
# `env -u MSYSTEM powershell.exe ...` still sees it set inside), so the
# PowerShell command below clears it with `Remove-Item Env:\MSYSTEM`
# FIRST, from inside PowerShell itself, before ever sourcing export.ps1 -
# clearing it in bash before the launch does not survive the handoff.
# This matches the repo's own existing convention once you look at *why*
# it is that way: launcher/tools/sweeps/*.ps1 use PowerShell for the
# idf.py parts for the exact same underlying reason, and only shell out
# to bash for test/run_tests.sh, which needs a host C compiler, not the
# ESP-IDF venv/toolchain. The entry point here is still bash, as asked -
# click this file - it is only the ESP-IDF-specific inner work that has
# to happen in PowerShell.
#
# Usage:
#   tools/build_flash.sh [COM_PORT] [IDF_EXPORT_PS1]
#
#   COM_PORT        serial port the device is on. Default: COM3.
#   IDF_EXPORT_PS1  path to ESP-IDF's export.ps1. Default: this
#                   project's usual install location - pass your own if
#                   it differs.
#
# Run from anywhere (cds to the launcher/ directory itself); double-click
# from Explorer if .sh is associated with Git Bash, or right-click the
# launcher/tools/ folder -> "Git Bash Here" -> `./build_flash.sh`.

set -euo pipefail

COM_PORT="${1:-COM3}"
IDF_EXPORT_PS1="${2:-C:\\Espressif\\esp-idf-v5.5\\export.ps1}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAUNCHER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LAUNCHER_DIR_WIN="$(cd "$LAUNCHER_DIR" && pwd -W 2>/dev/null || echo "$LAUNCHER_DIR")"

# So a double-clicked window (which closes the instant the script exits)
# still shows the reason for a failure instead of vanishing on the spot.
trap 'status=$?; if [ $status -ne 0 ]; then echo; echo "=== FAILED (exit $status) ==="; read -r -p "Press Enter to close..." _; fi' EXIT

echo "=== Building and flashing build.release to $COM_PORT ==="
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
    Remove-Item Env:\MSYSTEM -ErrorAction SilentlyContinue
    & '$IDF_EXPORT_PS1' | Out-Null
    Set-Location '$LAUNCHER_DIR_WIN'
    idf.py -B build.release build
    if (\$LASTEXITCODE -ne 0) { exit \$LASTEXITCODE }
    idf.py -B build.release -p '$COM_PORT' flash
    exit \$LASTEXITCODE
"

echo "=== Done ==="
read -r -p "Press Enter to close..." _
