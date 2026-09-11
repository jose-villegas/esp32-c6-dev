#!/usr/bin/env bash
# Reports C/C++ comments longer than a character limit (300 by default), so
# the "keep a comment short enough to still be read" rule can be measured
# rather than argued. Exits 1 if anything is over the limit.
#
# Usage:
#   check-comment-length.sh                     # whole repo, 20 worst listed
#   check-comment-length.sh --files             # per-file counts instead
#   check-comment-length.sh --limit 400         # try a different limit
#   check-comment-length.sh --no-banners        # skip each file's header
#   check-comment-length.sh path/to/file.c ...  # just these files
#
# As a gate, on the comments a change actually touches - which is what makes
# the rule enforceable while the existing backlog is still there:
#   check-comment-length.sh --changed main      # vs a branch point
#   check-comment-length.sh --staged            # what is about to be committed
#
# Proving a bulk trim moved no code (a diff of thousands of reflowed comments
# is unreadable; this is checkable instead):
#   check-comment-length.sh --comments-only main
#
# See check_comment_length.py --help for the full option list.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

for py in python3 python py; do
    if command -v "$py" >/dev/null 2>&1; then
        exec "$py" "$here/check_comment_length.py" "$@"
    fi
done

echo "python 3 was not found on PATH (tried python3, python, py)." >&2
exit 1
