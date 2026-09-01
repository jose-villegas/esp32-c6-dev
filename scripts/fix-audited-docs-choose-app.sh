#!/usr/bin/env bash
# Interactive single-click launcher: lists every app that actually has its
# own docs/<Name>/ folder (discovered fresh each run by checking
# docs/<Capitalized app> for each entry under launcher/main/apps/ -- so an
# app only appears here once someone gives it a dedicated doc folder; today
# that's just Sand -- cube and diagnostics have no docs/Cube or
# docs/Diagnostics yet) plus a whole-project option (every tracked doc),
# asks which one to scope the free-tier docs-audit-fix pass to, then runs
# it.
#
# Enter either the number or the app's name (case-insensitive) at the
# prompt.
#
# Runs entirely inside an isolated git worktree (fix-audited-docs.sh
# --worktree), so your current checkout is never touched. Any extra
# arguments given on the command line are forwarded to fix-audited-docs.sh
# as-is, e.g. --no-push, --review <model-id>, or --local (routes fixer/
# review through Ollama instead of the free-tier docs-update-free combo).
#
# Usage: scripts/fix-audited-docs-choose-app.sh [extra fix-audited-docs.sh args...]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

APPS=()
for appdir in launcher/main/apps/*/; do
  app="$(basename "$appdir")"
  app_cap="$(printf '%s' "$app" | cut -c1 | tr '[:lower:]' '[:upper:]')$(printf '%s' "$app" | cut -c2-)"
  [ -d "docs/$app_cap" ] && APPS+=("$app")
done

echo "Which scope should the free-tier docs audit run against?"
i=1
for app in ${APPS[@]+"${APPS[@]}"}; do
  echo "  $i) $app"
  i=$((i + 1))
done
PROJECT_CHOICE_NUM=$i
echo "  $PROJECT_CHOICE_NUM) whole project (all tracked docs)"
echo ""
read -r -p "Enter a number or an app name: " CHOICE

CHOICE_LOWER=$(printf '%s' "$CHOICE" | tr '[:upper:]' '[:lower:]')

APP=""
IS_PROJECT=0
if [[ "$CHOICE" =~ ^[0-9]+$ ]]; then
  if [ "$CHOICE" -eq "$PROJECT_CHOICE_NUM" ]; then
    IS_PROJECT=1
  elif [ "$CHOICE" -ge 1 ] && [ "$CHOICE" -lt "$PROJECT_CHOICE_NUM" ]; then
    APP="${APPS[$((CHOICE - 1))]}"
  fi
elif [ "$CHOICE_LOWER" = "project" ] || [ "$CHOICE_LOWER" = "all" ] || [ "$CHOICE_LOWER" = "whole project" ]; then
  IS_PROJECT=1
else
  for a in ${APPS[@]+"${APPS[@]}"}; do
    if [ "$(printf '%s' "$a" | tr '[:upper:]' '[:lower:]')" = "$CHOICE_LOWER" ]; then
      APP="$a"
      break
    fi
  done
fi

if [ "$IS_PROJECT" != "1" ] && [ -z "$APP" ]; then
  echo "Didn't recognize '$CHOICE' as a number or an app name (${APPS[*]+"${APPS[*]}"})." >&2
  exit 1
fi

DOC_FILE_ARGS=()
if [ "$IS_PROJECT" = "1" ]; then
  SCOPE_LABEL="whole project (all tracked docs)"
else
  APP_CAP="$(printf '%s' "$APP" | cut -c1 | tr '[:lower:]' '[:upper:]')$(printf '%s' "$APP" | cut -c2-)"
  shopt -s nullglob
  DOC_FILE_ARGS=("docs/$APP_CAP"/*.md)
  shopt -u nullglob
  SCOPE_LABEL="docs/$APP_CAP/"
fi

echo ""
echo "============================================================"
echo " Free-tier docs audit -- $SCOPE_LABEL"
echo " Isolated worktree -- your current checkout is not touched."
echo "============================================================"
echo ""

scripts/fix-audited-docs.sh --worktree "${DOC_FILE_ARGS[@]}" "$@"
