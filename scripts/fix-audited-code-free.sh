#!/usr/bin/env bash
# One-command launcher for fix-audited-code.sh --pool free --worktree.
# Siblings: fix-audited-code-local.sh (same two scopes, zero cloud calls via
# Ollama instead) and fix-audited-code-choose-app.sh (interactively pick any
# app under launcher/main/apps/ instead of just sand-or-whole-project).
#
# Two scopes, selected by --project:
#
#   (default)  apps/sand only (fix-audited-code.sh's own defaults: build.dev,
#              */apps/sand/* file_filter), reviewed by deepseek/deepseek-v4-pro
#              (free-tier, code-focused, GA -- picked over a "preview" Gemini
#              build after checking OmniRoute had no real quality-ranking
#              data to decide with).
#
#   --project  whole project (main/, minus apps/), reviewed by
#              gemini/gemini-3.1-pro-preview. Uses file_filter "*/main/*",
#              never a bare "*": cppcheck's --file-filter controls what gets
#              ANALYZED, not just what gets reported, so "*" makes cppcheck
#              fully parse the whole ESP-IDF SDK and every vendored component
#              (managed_components/) too -- ~1800 translation units when only
#              ~28 are actually main/ code. That's what produced a dump-file
#              flood and a 12GB+ RAM run that had to be killed by hand.
#              "*/main/*" scopes analysis to just main/ (still minus apps/,
#              via --exclude) and is what was actually wanted.
#
# Runs entirely inside an isolated git worktree either way (fix-audited-
# code.sh --worktree), by design: without it, misra_check.sh runs cppcheck
# against THIS checkout, and cppcheck writes a *.dump file next to any source
# file its MISRA addon chokes on -- dozens of untracked files landing in your
# working tree mid-scan, not something a checker should be able to do to your
# git status. --worktree isn't just "don't touch my branch" here, it also
# means misra_check.sh itself runs against the worktree's own source instead
# of this one (via the rewritten compile_commands.json fix-audited-code.sh
# sets up), so those dump files -- and everything else the scan produces --
# land there instead, not here.
#
# Any extra arguments are forwarded to fix-audited-code.sh as-is.
#
# Real free-tier model calls happen the moment this runs -- no confirmation
# prompt, since running it IS the confirmation. If anything gets fixed, a
# branch is pushed and a PR compare link is printed; if nothing does, no
# trace is left (see fix-audited-code.sh's own --worktree cleanup behavior).
#
# Usage: scripts/fix-audited-code-free.sh [--project] [extra fix-audited-code.sh args...]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

PROJECT_SCOPE=0
ARGS=()
while [ "$#" -gt 0 ]; do
  case "$1" in
    --project)
      PROJECT_SCOPE=1
      shift
      ;;
    *)
      ARGS+=("$1")
      shift
      ;;
  esac
done

if [ "$PROJECT_SCOPE" = "1" ]; then
  REVIEW_MODEL="gemini/gemini-3.1-pro-preview"
  SCOPE_LABEL="whole project (main/, minus apps/)"
else
  REVIEW_MODEL="deepseek/deepseek-v4-pro"
  SCOPE_LABEL="apps/sand (default scope)"
fi

echo "============================================================"
echo " Free-tier code audit -- $SCOPE_LABEL"
echo " Reviewer: $REVIEW_MODEL"
echo " Isolated worktree -- your current checkout is not touched."
echo "============================================================"
echo ""

if [ "$PROJECT_SCOPE" = "1" ]; then
  scripts/fix-audited-code.sh --review "$REVIEW_MODEL" --pool free --worktree \
      --exclude main/apps/ "${ARGS[@]}" build.dev "*/main/*"
else
  scripts/fix-audited-code.sh --review "$REVIEW_MODEL" --pool free --worktree "${ARGS[@]}"
fi
