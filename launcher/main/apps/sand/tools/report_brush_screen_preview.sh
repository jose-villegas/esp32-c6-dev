#!/bin/sh
#
# Build and run brush_screen_preview.c: render the sand app's brush screen
# through the real firmware drawing code, at both real canvas sizes, so its
# composition (the leftover space above the header panel and below the size
# panel - see brush_screen_preview.c's own top comment) can be judged
# without a device build or a flash.
#
# Also prints, to stdout, the top/bottom gap in px at each orientation -
# read straight off brush_screen_layout(), not eyeballed from the image.
#
# Usage:
#   main/apps/sand/tools/report_brush_screen_preview.sh
#
# Writes brush_screen_portrait.png (368x448) and brush_screen_landscape.png
# (448x368) into this script's own build/ directory.

set -eu

# This file lives at main/apps/sand/tools/, four levels below launcher/ -
# tools -> sand -> apps -> main -> launcher - same layout report_
# fingerprint.sh (this file's sibling) already resolves the same way.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SAND_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
MAIN_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
LAUNCHER_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)

BUILD_DIR="$SCRIPT_DIR/build"

# --- find a compiler -------------------------------------------------------
# Sourced, not copied - see tools/find_cc.sh's own top comment.
# shellcheck source=../../../../tools/find_cc.sh
. "$LAUNCHER_DIR/tools/find_cc.sh"

if ! CC_BIN=$(find_cc); then
    echo "No C compiler found." >&2
    echo "  Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
    echo "  Debian:  sudo apt install build-essential" >&2
    echo "  macOS:   xcode-select --install" >&2
    exit 1
fi

# No -Werror, unlike report_fingerprint.sh beside this: gfx.c (linked below,
# for real drawing - see brush_screen_preview.c's own top comment) carries
# present-path statics that a host build never calls, the same reason
# tools/boot_anim_editor_server.py's own compile of gfx.c drops these two.
CFLAGS="-std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -g -O1"

mkdir -p "$BUILD_DIR"
OUT_BIN="$BUILD_DIR/brush_screen_preview"

# gfx.c for real drawing (see brush_screen_preview.c's own top comment),
# brush_screen.c/material.c/material_palette.c for the layout and swatch
# colours it draws with - the same portable half of the app
# report_fingerprint.sh (this file's sibling) pulls in, plus the three this
# tool alone needs.
# shellcheck disable=SC2086
"$CC_BIN" $CFLAGS \
    -I "$MAIN_DIR" -I "$SAND_DIR" \
    -I "$LAUNCHER_DIR/components/microui/include" \
    "$SCRIPT_DIR/brush_screen_preview.c" \
    "$MAIN_DIR/gfx/gfx.c" \
    "$SAND_DIR/brush_screen.c" \
    "$SAND_DIR/material.c" \
    "$SAND_DIR/material_palette.c" \
    -o "$OUT_BIN"

# MinGW appends .exe; elsewhere the plain name is produced.
[ -x "$OUT_BIN" ] || OUT_BIN="$OUT_BIN.exe"

"$OUT_BIN" "$BUILD_DIR"

# pyserial lives in ESP-IDF's environment (and, on this machine, the system
# interpreter too) - screenshot.py imports it at module scope even though
# bmp_bytes_to_png() itself never touches a serial port, so the search below
# mirrors tools/screenshot.sh's own: prefer whatever's on PATH, but let the
# ESP-IDF env win if the bare PATH lookup has no pyserial.
PYTHON=$(command -v python3 || command -v python || true)
for candidate in "$HOME/.espressif/python_env"/idf*_env/bin/python \
                 "$HOME/.espressif/python_env"/idf*_env/Scripts/python.exe; do
    [ -x "$candidate" ] && ! "$PYTHON" -c "import serial" >/dev/null 2>&1 && PYTHON="$candidate"
done
if [ -z "${PYTHON:-}" ]; then
    echo "no Python found (need pyserial, for screenshot.py's bmp_bytes_to_png - ESP-IDF's own environment has it)" >&2
    exit 1
fi

"$PYTHON" - "$LAUNCHER_DIR/tools" "$BUILD_DIR" <<'PY'
import sys

sys.path.insert(0, sys.argv[1])
from screenshot import bmp_bytes_to_png  # noqa: E402

build_dir = sys.argv[2]
for name in ("brush_screen_portrait", "brush_screen_landscape"):
    bmp_path = f"{build_dir}/{name}.bmp"
    png_path = f"{build_dir}/{name}.png"
    with open(bmp_path, "rb") as f:
        bmp = f.read()
    with open(png_path, "wb") as f:
        f.write(bmp_bytes_to_png(bmp))
    print(f"wrote {png_path}")
PY
