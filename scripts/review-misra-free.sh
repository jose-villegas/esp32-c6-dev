#!/usr/bin/env bash
# One-command launcher for a whole-project (main/, minus apps) MISRA/cppcheck
# audit-fix pass on the free-tier pool, reviewed by gemini/gemini-3.1-pro-
# preview.
#
# Always runs in an isolated git worktree (fix-audited-code.sh --worktree),
# by design: without it, misra_check.sh runs cppcheck against THIS checkout,
# and cppcheck writes a *.dump file next to any source file its MISRA addon
# chokes on -- dozens of untracked files landing in your working tree mid-
# scan, not something a checker should be able to do to your git status.
# --worktree isn't just "don't touch my branch" here, it also means
# misra_check.sh itself runs against the worktree's own source instead of
# this one (via the symlinked build dir fix-audited-code.sh sets up), so
# those dump files -- and everything else the scan produces -- land there
# instead, not here.
#
# file_filter is "*/main/*", never a bare "*": cppcheck's --file-filter
# controls what gets ANALYZED, not just what gets reported, so "*" makes
# cppcheck fully parse the whole ESP-IDF SDK and every vendored component
# (managed_components/) too -- ~1800 translation units when only ~28 are
# actually main/ code. That's what produced the dump-file flood in the
# first place, on top of costing 12GB+ of RAM for a run that had to be
# killed by hand. "*/main/*" scopes analysis to just main/ (still minus
# apps/, via --exclude below) and is what was actually wanted.
#
# Any extra arguments are forwarded to fix-audited-code.sh as-is.
#
# Usage: scripts/review-misra-free.sh [extra fix-audited-code.sh args...]
set -euo pipefail

REVIEW_MODEL="gemini/gemini-3.1-pro-preview"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

scripts/fix-audited-code.sh --review "$REVIEW_MODEL" --pool free --worktree \
    --exclude main/apps/ "$@" build.dev "*/main/*"
