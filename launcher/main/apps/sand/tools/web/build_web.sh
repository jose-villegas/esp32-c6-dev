#!/bin/sh
#
# Compile the real sand simulation - the same portable sources the host
# test runner links (see test/run_tests.sh's own top comment) - plus this
# folder's own web_sand.c shim, to WebAssembly. Output lands in dist/,
# alongside index.html/app.js/style.css - copy that whole folder anywhere
# static files can be served (GitHub Pages, Netlify, itch.io, a plain
# `python -m http.server`) and the demo runs with no server-side code at
# all.
#
#   main/apps/sand/tools/web/setup_emsdk.sh    (once)
#   main/apps/sand/tools/web/build_web.sh
#
# POSIX sh on purpose, same as test/run_tests.sh: works under Git Bash or
# MSYS on Windows, and natively on Linux and macOS.

set -eu

# This file lives at main/apps/sand/tools/web/, five levels below launcher/ -
# web -> tools -> sand -> apps -> main -> launcher - same resolution style
# report_reactions.sh (one level up) already uses.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SAND_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
MAIN_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)

EMSDK_DIR="$SCRIPT_DIR/emsdk"
BUILD_DIR="$SCRIPT_DIR/build"
DIST_DIR="$SCRIPT_DIR/dist"

# --- find emcc ---------------------------------------------------------
if ! command -v emcc >/dev/null 2>&1; then
    if [ -f "$EMSDK_DIR/emsdk_env.sh" ]; then
        # shellcheck source=/dev/null
        . "$EMSDK_DIR/emsdk_env.sh"
    fi
fi

if ! command -v emcc >/dev/null 2>&1; then
    echo "emcc not found. Run main/apps/sand/tools/web/setup_emsdk.sh" \
         "first (one-time, clones the Emscripten SDK into ./emsdk)." >&2
    exit 1
fi

# Same portable sources test/run_tests.sh links for the host test runner -
# see that script's own SOURCES comment. row_runs.c and palette.c are not
# needed here: this build has no dirty-row tracking and no microui panel to
# hit-test - see web_sand.c's own top comment for why.
SOURCES="
$SAND_DIR/material.c
$SAND_DIR/sand.c
$SAND_DIR/sand_liquid.c
$SAND_DIR/sand_gas.c
$SAND_DIR/sand_reactions.c
$SAND_DIR/tilt.c
$SCRIPT_DIR/web_sand.c
"

mkdir -p "$BUILD_DIR" "$DIST_DIR"

# -O3 for a real build: this ships to a browser, not a laptop test run, so
# optimization time is not on anyone's critical path the way run_tests.sh's
# is. EXPORTED_RUNTIME_METHODS gives app.js ccall()/cwrap() (calling the
# exported web_* functions by name) and HEAPU8 (reading web_render()'s pixel
# buffer directly out of wasm memory without a copy).
# shellcheck disable=SC2086
emcc -O3 -std=c11 -DNDEBUG -Wall -Wextra \
    -I "$MAIN_DIR" -I "$SAND_DIR" \
    $SOURCES \
    -o "$DIST_DIR/sand.js" \
    -s MODULARIZE=1 \
    -s EXPORT_NAME=SandModule \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPU8"]' \
    -s ENVIRONMENT=web

cp "$SCRIPT_DIR/index.html" "$SCRIPT_DIR/app.js" "$SCRIPT_DIR/style.css" "$DIST_DIR/"

echo "Built $DIST_DIR/sand.wasm - serve $DIST_DIR/ with any static file server."
