#!/usr/bin/env bash
# One-command launcher for fix-audited-docs.sh --worktree: the audit
# cross-check (via audit-docs.sh) and the fix/review passes all run through
# Ollama (ollama run), so this makes zero network calls to any cloud
# provider.
#
# --app <name> scopes to just that app's own docs (docs/<name>/, e.g.
# --app sand -> docs/sand/*.md) instead of the default (every tracked doc).
# Only apps with a real docs/<name>/ folder can be targeted this way --
# today that's just sand; cube and diagnostics don't have dedicated doc
# folders yet.
#
# Any other extra arguments are forwarded to fix-audited-docs.sh as-is, e.g.
# --no-push to stop short of pushing the resulting branch.
#
# No confirmation prompt -- running it IS the confirmation. If anything gets
# fixed, a branch is pushed and a PR compare link is printed; if nothing
# does, no trace is left.
#
# Usage: scripts/fix-audited-docs-local.sh [--app <name>] [extra fix-audited-docs.sh args...]
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
  APP_DOC_DIR="docs/$APP"
  if [ ! -d "$APP_DOC_DIR" ]; then
    echo "No $APP_DOC_DIR/ folder -- nothing to scope --app $APP to." >&2
    echo "(Only apps with their own docs/<name>/ folder can be targeted -- today that's just sand.)" >&2
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
echo " Local docs audit -- $SCOPE_LABEL"
echo " No cloud calls -- Isolated worktree, checkout untouched."
echo "============================================================"
echo ""

scripts/fix-audited-docs.sh --worktree "${DOC_FILE_ARGS[@]}" "${ARGS[@]}"
