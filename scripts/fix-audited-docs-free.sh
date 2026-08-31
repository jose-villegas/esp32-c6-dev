#!/usr/bin/env bash
# One-command launcher for fix-audited-docs.sh --worktree, using its own
# free-tier "docs-update-free" combo (no --review model unless you pass one
# through), inside an isolated git worktree so this never touches whatever's
# currently checked out or in progress here. Siblings: fix-audited-docs-
# local.sh runs the same thing with zero cloud calls, via Ollama instead;
# fix-audited-docs-choose-app.sh is an interactive version of --app below.
#
# --app <name> scopes to just that app's own docs (docs/<Capitalized name>/,
# e.g. --app sand -> docs/Sand/*.md) instead of the default (every tracked
# doc under README.md and docs/). Mirrors fix-audited-code-free.sh's
# --project flag in spirit, but inverted: docs audits are cheap regardless
# of scope (unlike cppcheck/MISRA, there's no 12GB-RAM-run risk in scanning
# everything), so "all docs" stays the sane default and --app is the
# opt-in narrowing, not the other way around. Only apps with a real
# docs/<Name>/ folder can be targeted this way -- today that's just Sand
# (docs/Sand/); cube and diagnostics don't have dedicated doc folders yet.
#
# Any other extra arguments are forwarded to fix-audited-docs.sh as-is, so
# e.g. `fix-audited-docs-free.sh --review claude/claude-sonnet-5` adds a
# review pass on top of whatever scope applies.
#
# Real free-tier model calls happen the moment this runs -- no confirmation
# prompt, since running it IS the confirmation. If anything gets fixed, a
# branch is pushed and a PR compare link is printed; if nothing does, no
# trace is left (see fix-audited-docs.sh's own --worktree cleanup behavior).
#
# Usage: scripts/fix-audited-docs-free.sh [--app <name>] [extra fix-audited-docs.sh args...]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

APP=""
ARGS=()
while [ "$#" -gt 0 ]; do
  case "$1" in
    --app)
      APP="$2"
      shift 2
      ;;
    *)
      ARGS+=("$1")
      shift
      ;;
  esac
done

DOC_FILE_ARGS=()
SCOPE_LABEL="all tracked docs (default)"
if [ -n "$APP" ]; then
  APP_CAP="$(printf '%s' "$APP" | cut -c1 | tr '[:lower:]' '[:upper:]')$(printf '%s' "$APP" | cut -c2-)"
  APP_DOC_DIR="docs/$APP_CAP"
  if [ ! -d "$APP_DOC_DIR" ]; then
    echo "No $APP_DOC_DIR/ folder -- nothing to scope --app $APP to." >&2
    echo "(Only apps with their own docs/<Name>/ folder can be targeted -- today that's just Sand.)" >&2
    exit 1
  fi
  shopt -s nullglob
  DOC_FILE_ARGS=("$APP_DOC_DIR"/*.md)
  shopt -u nullglob
  if [ "${#DOC_FILE_ARGS[@]}" -eq 0 ]; then
    echo "$APP_DOC_DIR/ has no .md files -- nothing to scope --app $APP to." >&2
    exit 1
  fi
  SCOPE_LABEL="$APP_DOC_DIR/"
fi

echo "============================================================"
echo " Free-tier docs audit -- $SCOPE_LABEL"
echo " Isolated worktree -- your current checkout is not touched."
echo "============================================================"
echo ""

scripts/fix-audited-docs.sh --worktree "${DOC_FILE_ARGS[@]}" "${ARGS[@]}"
