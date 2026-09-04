#!/bin/sh
#
# One-time setup: clone and activate the Emscripten SDK into ./emsdk, right
# beside this script. Nothing here touches the system - no global install, no
# PATH edit outside this one checkout - so removing the emsdk/ folder undoes
# it completely. Run once, before build_web.sh's first use:
#
#   main/apps/sand/tools/web/setup_emsdk.sh
#
# POSIX sh on purpose, same as test/run_tests.sh: works under Git Bash or
# MSYS on Windows, and natively on Linux and macOS.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
EMSDK_DIR="$SCRIPT_DIR/emsdk"

if [ -d "$EMSDK_DIR" ]; then
    echo "emsdk already present at $EMSDK_DIR - updating instead of cloning."
    git -C "$EMSDK_DIR" pull --ff-only
else
    git clone https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
fi

cd "$EMSDK_DIR"
./emsdk install latest
./emsdk activate latest

echo
echo "emsdk ready. build_web.sh sources $EMSDK_DIR/emsdk_env.sh itself, so"
echo "no shell setup is needed beyond running that script."
