#!/bin/sh
#
# Locate a C compiler for a host build, on any platform this project's
# tooling runs on.
#
# Pulled out of test/run_tests.sh, which needed it first, so that a second
# host-side C build (main/apps/sand/tools/report_reactions.sh) can find a
# compiler the same way without a hand-copied twin quietly drifting out of
# sync with the original the way two independent copies of anything here
# eventually do. The standing idiom for this in the repo is a sourced
# helper rather than a duplicated block - see tools/idf.sh, sourced by both
# build_flash.sh and tools/ci_check_idf_sh.sh.
#
# Usage - source this file and call find_cc():
#
#   . "$TOOLS_DIR/find_cc.sh"
#   CC_BIN=$(find_cc) || { echo "no compiler found" >&2; exit 1; }
#
# $CC wins if the caller has already set it (CC=clang ./whatever.sh), then
# whatever is on PATH. The last candidate is where winget puts MinGW on
# Windows, which is not on PATH for a shell opened before it was installed.

find_cc() {
    if [ -n "${CC:-}" ]; then echo "$CC"; return 0; fi
    for c in cc gcc clang; do
        if command -v "$c" >/dev/null 2>&1; then echo "$c"; return 0; fi
    done
    winlibs="${LOCALAPPDATA:-}/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin/gcc.exe"
    if [ -n "${LOCALAPPDATA:-}" ] && [ -x "$winlibs" ]; then echo "$winlibs"; return 0; fi
    return 1
}
