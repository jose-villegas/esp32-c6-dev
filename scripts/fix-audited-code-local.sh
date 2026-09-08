#!/usr/bin/env bash
# One-command launcher for fix-audited-code.sh --worktree: fixer and
# reviewer both run through Ollama (ollama run), so this makes zero network
# calls to any cloud provider. Two scopes, selected by --project:
#   (default)  apps/sand only
#   --project  whole project (main/, minus apps/)
#
# Runs entirely inside an isolated git worktree (fix-audited-code.sh
# --worktree), so your current checkout is never touched. Any extra
# arguments are forwarded to fix-audited-code.sh as-is, e.g. --no-push to
# stop short of pushing the resulting branch.
#
# No confirmation prompt -- running it IS the confirmation. If anything gets
# fixed, a branch is pushed and a PR compare link is printed; if nothing
# does, no trace is left.
#
# Usage: scripts/fix-audited-code-local.sh [--project] [extra fix-audited-code.sh args...]
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
  SCOPE_LABEL="whole project (main/, minus apps/)"
else
  SCOPE_LABEL="apps/sand (default scope)"
fi

echo "============================================================"
echo " Local code audit -- $SCOPE_LABEL"
echo " Fixer/reviewer: Ollama, no cloud calls"
echo " Isolated worktree -- your current checkout is not touched."
echo "============================================================"
echo ""

if [ "$PROJECT_SCOPE" = "1" ]; then
  scripts/fix-audited-code.sh --worktree \
      --exclude main/apps/ "${ARGS[@]}" build.dev "*/main/*"
else
  scripts/fix-audited-code.sh --worktree "${ARGS[@]}"
fi
