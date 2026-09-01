#!/usr/bin/env bash
# Interactive single-click launcher: lists every app currently under
# launcher/main/apps/ (discovered fresh each run -- never hardcoded, so a
# new app shows up automatically) plus a whole-project option, asks which
# one to scope the free-tier code-audit-fix pass to, then runs it.
#
# Enter either the number or the app's name (case-insensitive) at the
# prompt.
#
# Same reviewer convention as fix-audited-code-free.sh: a single app is
# narrow enough to fix in one pass, reviewed by deepseek/deepseek-v4-pro;
# the whole project (main/, minus apps/) is reviewed by gemini/gemini-3.1-
# pro-preview instead, and uses file_filter "*/main/*" (never a bare "*" --
# see fix-audited-code.sh's own header comment for why that matters:
# cppcheck's --file-filter controls what gets ANALYZED, not just reported).
#
# Runs entirely inside an isolated git worktree (fix-audited-code.sh
# --worktree), so your current checkout is never touched. Any extra
# arguments given on the command line are forwarded to fix-audited-code.sh
# as-is, e.g. --no-push or --local (local routes fixer+reviewer through
# Ollama instead, but the deepseek/gemini reviewer choice above is then
# ignored in favor of fix-audited-code.sh's own local defaults -- pass
# --review yourself alongside --local if you want a specific local model).
#
# Usage: scripts/fix-audited-code-choose-app.sh [extra fix-audited-code.sh args...]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

mapfile -t APPS < <(find launcher/main/apps -mindepth 1 -maxdepth 1 -type d -exec basename {} \; | sort)

if [ "${#APPS[@]}" -eq 0 ]; then
  echo "No apps found under launcher/main/apps/." >&2
  exit 1
fi

echo "Which scope should the free-tier code audit run against?"
i=1
for app in "${APPS[@]}"; do
  echo "  $i) $app"
  i=$((i + 1))
done
PROJECT_CHOICE_NUM=$i
echo "  $PROJECT_CHOICE_NUM) whole project (main/, minus apps/)"
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
  for a in "${APPS[@]}"; do
    if [ "$(printf '%s' "$a" | tr '[:upper:]' '[:lower:]')" = "$CHOICE_LOWER" ]; then
      APP="$a"
      break
    fi
  done
fi

if [ "$IS_PROJECT" != "1" ] && [ -z "$APP" ]; then
  echo "Didn't recognize '$CHOICE' as a number or an app name (${APPS[*]})." >&2
  exit 1
fi

if [ "$IS_PROJECT" = "1" ]; then
  REVIEW_MODEL="gemini/gemini-3.1-pro-preview"
  SCOPE_LABEL="whole project (main/, minus apps/)"
else
  REVIEW_MODEL="deepseek/deepseek-v4-pro"
  SCOPE_LABEL="apps/$APP"
fi

echo ""
echo "============================================================"
echo " Free-tier code audit -- $SCOPE_LABEL"
echo " Reviewer: $REVIEW_MODEL"
echo " Isolated worktree -- your current checkout is not touched."
echo "============================================================"
echo ""

if [ "$IS_PROJECT" = "1" ]; then
  scripts/fix-audited-code.sh --review "$REVIEW_MODEL" --pool free --worktree \
      --exclude main/apps/ "$@" build.dev "*/main/*"
else
  scripts/fix-audited-code.sh --review "$REVIEW_MODEL" --pool free --worktree \
      "$@" build.dev "*/apps/$APP/*"
fi
