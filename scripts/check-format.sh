#!/usr/bin/env bash
# Checks that clang-format is installed and new enough, then formats
# (or, with --check, verifies formatting of) the given C/C++ files
# using this repository's .clang-format rules (or the nearest one
# found by walking up from the file's directory).
#
# Usage:
#   check-format.sh <file.c> [file.h ...]        # format files in place
#   check-format.sh --check <file.c> [file.h ...] # verify only, no writes; exits 1 if not compliant

set -euo pipefail

# Upstream (MaJerle/c-code-style) requires >=20; this repo pins to 19 because
# that's what ships bundled with the ESP-IDF toolchain (esp-clang), verified
# compatible with this repo's .clang-format (clean --dry-run against a
# compliant sample file).
MIN_MAJOR=19

# Fall back to the ESP-IDF-bundled esp-clang if clang-format isn't on PATH
# (true unless the IDF export script has been sourced in this shell).
CLANG_FORMAT="clang-format"
if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    esp_clang="$(ls -d "$HOME"/.espressif/tools/esp-clang/*/esp-clang/bin/clang-format* 2>/dev/null | sort -V | tail -n1)"
    if [ -n "$esp_clang" ]; then
        CLANG_FORMAT="$esp_clang"
    fi
fi

print_install_help() {
    echo "clang-format was not found (or is too old) on PATH." >&2
    echo "" >&2
    echo "Install it:" >&2
    case "$(uname -s)" in
        Darwin)
            echo "  macOS:   brew install llvm" >&2
            echo "           then add \$(brew --prefix llvm)/bin to PATH" >&2
            ;;
        Linux)
            echo "  Linux:   install the clang-format package from your distribution" >&2
            echo "           (e.g. apt-get install clang-format), or install LLVM from" >&2
            echo "           the official LLVM apt repository for a newer version" >&2
            ;;
        *)
            echo "  Other:   install LLVM from https://releases.llvm.org/ and ensure" >&2
            echo "           clang-format(.exe) is on PATH" >&2
            ;;
    esac
    echo "" >&2
    echo "This repository requires clang-format >= ${MIN_MAJOR}.x." >&2
}

if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    print_install_help
    exit 1
fi

version_line="$("$CLANG_FORMAT" --version)"
major="$(echo "$version_line" | grep -oE '[0-9]+' | head -n1)"

if [ -z "$major" ] || [ "$major" -lt "$MIN_MAJOR" ]; then
    echo "Found: $version_line" >&2
    print_install_help
    exit 1
fi

mode="format"
if [ "${1:-}" = "--check" ]; then
    mode="check"
    shift
fi

if [ "$#" -eq 0 ]; then
    echo "Usage: $0 [--check] <file.c|file.h> [...]" >&2
    exit 1
fi

if [ "$mode" = "check" ]; then
    "$CLANG_FORMAT" --dry-run --Werror "$@"
else
    "$CLANG_FORMAT" -i "$@"
    echo "Formatted with clang-format $major: $*"
fi
