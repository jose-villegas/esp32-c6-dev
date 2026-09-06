#!/usr/bin/env bash
# Shortens over-long C comments with a LOCAL Ollama model, one comment at a
# time, and writes a review report to scripts/results/comment-trim.md.
#
# The model is handed a comment's prose and nothing else - never code, never a
# whole file - and the rewrite goes back into that comment's own span, so a bad
# generation can only produce a bad sentence. Every file is checked afterwards
# to differ from what it was in comments alone; one that does not is discarded.
# A comment the model cannot get under the limit keeps its original text.
#
# Slow by design: roughly 45 s per comment, so scope a run and expect to leave
# it going. Read scripts/results/comment-trim.md afterwards - the rewrites need
# a human, since a local model will occasionally drop a WHY.
#
# Reviewing a finished run (deterministic fact checks, then a reviewer model
#   of your choice - or a packet for one this script cannot call):
#   trim-comments-local.sh --review
#   trim-comments-local.sh --review --review-model none        # checks only
#   trim-comments-local.sh --review-packet scripts/results/pairs
#
# Usage:
#   trim-comments-local.sh --max 5 --dry-run          # try it, write nothing
#   trim-comments-local.sh launcher/main/apps/sand    # one app (the default)
#   trim-comments-local.sh --model qwen3-coder:30b <path>
#
# Verify a finished run before committing:
#   ./scripts/check-comment-length.sh --comments-only HEAD
#
# See trim_comments_local.py --help for the full option list.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v ollama >/dev/null 2>&1; then
    echo "ollama was not found on PATH." >&2
    exit 1
fi

for py in python3 python py; do
    if command -v "$py" >/dev/null 2>&1; then
        exec "$py" "$here/trim_comments_local.py" "$@"
    fi
done

echo "python 3 was not found on PATH (tried python3, python, py)." >&2
exit 1
