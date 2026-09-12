#!/bin/sh
#
# Prints the first-party C and header files the formatting rules apply to -
# the single definition of that set, so the pre-commit hook and the CI gate
# cannot disagree about what is checked. docs/C-Style-Guide.md is the prose;
# this is the file list.
#
# Usage:
#   scripts/format-file-list.sh                  # every tracked first-party file
#   scripts/format-file-list.sh <path> [...]     # only those paths that qualify
#   ... | scripts/format-file-list.sh --stdin    # same, paths one per line
#
# The filtering forms print nothing (and still succeed) when no path
# qualifies, which is what lets a caller run it over an arbitrary commit's
# file list and act on the result.
#
# Two kinds of file are excluded, for the reason the style guide gives: they
# keep their upstream or generator-owned form.
#
#   Vendored, by path. A short list, because a vendored tree arrives whole
#   and is noticed.
#
#   Generated, by the "GENERATED FILE" marker every generator in this repo
#   already writes into its first few lines. Deliberately not a path list: a
#   header that starts being generated tomorrow is excluded the day it
#   appears, not the day someone remembers to come back here. The cost is
#   that a generator which forgets the marker is silently held to the style
#   rules - which is the safe direction to fail.
#
# POSIX sh, like the other scripts here: development happens in Git Bash on
# Windows, and the hook has to run there too.

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

qualifies() {
    path="$1"

    case "$path" in
        *.c | *.h) ;;
        *) return 1 ;;
    esac

    # Vendored trees, checked in as-is. Patterns are spelled out in the case
    # itself rather than looped over a variable: an unquoted pattern in a
    # loop is subject to pathname expansion, which silently turns
    # "launcher/test/framework/*" into the three paths it matches today and
    # lets everything else through. A case pattern is never expanded.
    case "$path" in
        launcher/components/microui/* | \
            launcher/components/small3dlib/* | \
            launcher/test/framework/*)
            return 1
            ;;
    esac

    # A path can name a file that no longer exists (a deletion in the commit
    # being checked, or a stale argument). Nothing to format, so drop it
    # rather than failing - callers pass whole commit file lists.
    [ -f "$REPO_ROOT/$path" ] || return 1

    if head -5 "$REPO_ROOT/$path" | grep -q 'GENERATED FILE'; then
        return 1
    fi

    return 0
}

emit_if_qualifies() {
    if qualifies "$1"; then
        printf '%s\n' "$1"
    fi
}

if [ "${1:-}" = "--stdin" ]; then
    while IFS= read -r path; do
        [ -n "$path" ] || continue
        emit_if_qualifies "$path"
    done
    exit 0
fi

if [ "$#" -gt 0 ]; then
    for path in "$@"; do
        emit_if_qualifies "$path"
    done
    exit 0
fi

git -C "$REPO_ROOT" ls-files '*.c' '*.h' | while IFS= read -r path; do
    emit_if_qualifies "$path"
done
