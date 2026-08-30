#!/usr/bin/env bash
# One-command launcher for fix-audited-code.sh --pool free.
#
# Runs the free-tier fixer pool (ollama-cloud/deepseek/gemini/nvidia free
# tiers, local as final fallback) reviewed by deepseek/deepseek-v4-pro
# (free-tier, code-focused, GA -- picked over a "preview" Gemini build after
# checking OmniRoute had no real quality-ranking data to decide with), inside
# an isolated git worktree so this never touches whatever's currently
# checked out or in progress here. Any extra arguments are forwarded as-is,
# so e.g. `run-code-audit-free.sh --exclude main/apps/ build.dev "*"` widens
# scope to the whole project instead of fix-audited-code.sh's own default
# (build.dev, */apps/sand/* file_filter).
#
# Real free-tier model calls happen the moment this runs -- no confirmation
# prompt, since running it IS the confirmation. If anything gets fixed, a
# branch is pushed and a PR compare link is printed; if nothing does, no
# trace is left (see fix-audited-code.sh's own --worktree cleanup behavior).
#
# Usage: scripts/run-code-audit-free.sh [extra fix-audited-code.sh args...]
set -euo pipefail

REVIEW_MODEL="deepseek/deepseek-v4-pro"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

echo "============================================================"
echo " Free-tier code audit -- fix-audited-code.sh --pool free"
echo " Reviewer: $REVIEW_MODEL"
echo " Isolated worktree -- your current checkout is not touched."
echo "============================================================"
echo ""

scripts/fix-audited-code.sh --review "$REVIEW_MODEL" --pool free --worktree "$@"
