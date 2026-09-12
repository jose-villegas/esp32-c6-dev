#!/usr/bin/env bash
# Finds a clang-format of the pinned version, then formats (or, with
# --check, verifies formatting of) the given C/C++ files using this
# repository's .clang-format rules (or the nearest one found by walking up
# from the file's directory).
#
# Usage:
#   check-format.sh <file.c> [file.h ...]         # format files in place
#   check-format.sh --check <file.c> [file.h ...] # verify only, no writes; exits 1 if not compliant
#   check-format.sh --which                       # print the clang-format this repo would use
#
# Pass files, never directories, and never the whole repository -
# scripts/format-file-list.sh decides which files these rules apply to.

set -euo pipefail

# ONE major version, not a floor. .clang-format does not define a formatting
# by itself; a version of clang-format reading it does, and the versions
# disagree about this config on real files in this tree. Measured against
# the committed baseline: 19 reformats nothing, 20 reformats
# suite_sand_scenes.c, 21 also reformats material.c and gfx.c, 18 reformats
# seven files. Accepting a range would mean every contributor's toolchain
# quietly votes on the tree.
#
# 19 because it is what the ESP-IDF toolchain bundles (esp-clang), so the
# common case needs no extra install at all. CI pins the same major - see
# .github/workflows/format.yml.
PINNED_MAJOR=19

# Escape hatch for a one-off run on a machine that genuinely cannot get 19.
# Deliberately not a config file: an override that survives a session would
# just be the unpinned behaviour back again.
ALLOW_ANY="${CLANG_FORMAT_ANY_VERSION:-0}"

clang_format_major() {
    local version_line
    version_line="$("$1" --version 2>/dev/null)" || return 1
    printf '%s' "$version_line" | grep -oE '[0-9]+' | head -n1
}

# In preference order: the explicitly versioned binary, whatever is on PATH,
# then the ESP-IDF-bundled esp-clang (present unless the IDF export script
# has been sourced in this shell, in which case PATH already found it).
# Newest esp-clang first, since a machine can carry several IDF versions.
candidate_binaries() {
    if command -v "clang-format-$PINNED_MAJOR" >/dev/null 2>&1; then
        echo "clang-format-$PINNED_MAJOR"
    fi
    if command -v clang-format >/dev/null 2>&1; then
        echo "clang-format"
    fi
    ls -d "$HOME"/.espressif/tools/esp-clang/*/esp-clang/bin/clang-format 2>/dev/null | sort -Vr || true
}

print_install_help() {
    echo "" >&2
    echo "This repository formats with clang-format ${PINNED_MAJOR}.x and no other major version." >&2
    echo "" >&2
    echo "Get it:" >&2
    echo "  any platform:  pip install 'clang-format==${PINNED_MAJOR}.1.7'" >&2
    echo "  ESP-IDF:       already bundled - source the IDF export script, or let this" >&2
    echo "                 script find ~/.espressif/tools/esp-clang/*/esp-clang/bin/" >&2
    case "$(uname -s)" in
        Darwin)
            echo "  macOS:         brew install llvm@${PINNED_MAJOR}" >&2
            ;;
        Linux)
            echo "  Linux:         apt-get install clang-format-${PINNED_MAJOR}" >&2
            echo "                 (from the LLVM apt repository if your distribution is older)" >&2
            ;;
        *)
            echo "  Other:         https://releases.llvm.org/" >&2
            ;;
    esac
    echo "" >&2
    echo "To run anyway with a different major (it may reformat files you did not touch):" >&2
    echo "  CLANG_FORMAT_ANY_VERSION=1 $0 ..." >&2
}

# Resolves CLANG_FORMAT, preferring a binary whose major is the pinned one.
# Anything else is only a fallback so the error message can name what was
# actually found rather than claiming nothing exists.
resolve_clang_format() {
    local candidate major
    local fallback="" fallback_major=""

    while IFS= read -r candidate; do
        [ -n "$candidate" ] || continue
        major="$(clang_format_major "$candidate")" || continue
        [ -n "$major" ] || continue
        if [ "$major" = "$PINNED_MAJOR" ]; then
            CLANG_FORMAT="$candidate"
            CLANG_FORMAT_MAJOR="$major"
            return 0
        fi
        if [ -z "$fallback" ]; then
            fallback="$candidate"
            fallback_major="$major"
        fi
    done < <(candidate_binaries)

    if [ -z "$fallback" ]; then
        echo "No clang-format found on PATH or in ~/.espressif/tools/esp-clang/." >&2
        print_install_help
        exit 1
    fi

    CLANG_FORMAT="$fallback"
    CLANG_FORMAT_MAJOR="$fallback_major"

    if [ "$ALLOW_ANY" != "1" ]; then
        echo "Found clang-format $fallback_major ($fallback), but this repository pins ${PINNED_MAJOR}.x." >&2
        print_install_help
        exit 1
    fi

    echo "WARNING: using clang-format $fallback_major, not the pinned ${PINNED_MAJOR}.x." >&2
    echo "WARNING: it may reformat files you did not touch. CI checks with ${PINNED_MAJOR}.x." >&2
    return 0
}

mode="format"
case "${1:-}" in
    --check)
        mode="check"
        shift
        ;;
    --which)
        mode="which"
        shift
        ;;
esac

resolve_clang_format

if [ "$mode" = "which" ]; then
    command -v "$CLANG_FORMAT" || printf '%s\n' "$CLANG_FORMAT"
    exit 0
fi

if [ "$#" -eq 0 ]; then
    echo "Usage: $0 [--check] <file.c|file.h> [...]" >&2
    echo "       $0 --which" >&2
    exit 1
fi

if [ "$mode" = "check" ]; then
    "$CLANG_FORMAT" --dry-run --Werror "$@"
else
    "$CLANG_FORMAT" -i "$@"
    echo "Formatted with clang-format $CLANG_FORMAT_MAJOR: $*"
fi
