#!/usr/bin/env bash
# On-demand doc sync: reviews code changes since docs were last touched, asks
# OmniRoute's free "docs-update-free" combo (gpt-oss:120b -> gpt-oss:20b ->
# github/gpt-4o-mini -> kimi-coding -> ollama-local/gemma4:26b, all $0) to
# propose corrected README.md / docs/*.md content, then pushes a branch for
# review. Never touches main directly -- open the printed compare URL
# yourself (or run `gh pr create` if you have it installed).
#
# Usage: scripts/update-docs.sh
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

if [ -n "$(git status --porcelain)" ]; then
  echo "Working tree isn't clean. Commit or stash first." >&2
  exit 1
fi

COMBO="docs-update-free"
DOC_FILES=$(git ls-files README.md docs)

LAST_DOC_COMMIT=$(git log -1 --format=%H -- README.md docs 2>/dev/null || true)
if [ -z "$LAST_DOC_COMMIT" ]; then
  LAST_DOC_COMMIT=$(git rev-list --max-parents=0 HEAD | tail -1)
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

DIFF_STAT="$TMPDIR/diffstat.txt"
DIFF_FULL="$TMPDIR/diff.txt"
PROMPT="$TMPDIR/prompt.txt"
RESPONSE="$TMPDIR/response.txt"

git diff --stat "$LAST_DOC_COMMIT"..HEAD -- . ':!README.md' ':!docs' > "$DIFF_STAT"
git diff "$LAST_DOC_COMMIT"..HEAD -- . ':!README.md' ':!docs' | head -c 60000 > "$DIFF_FULL"

if [ ! -s "$DIFF_STAT" ]; then
  echo "No code changes since the docs were last touched ($LAST_DOC_COMMIT). Nothing to do."
  exit 0
fi

{
  echo "You are updating technical documentation for a firmware repo (ESP32-C6 app shell)."
  echo "Below is a summary of code changes since the docs were last touched, followed by"
  echo "the full current content of each doc file. Identify ONLY the doc files that are now"
  echo "stale or inaccurate because of these code changes, and output their FULL corrected"
  echo "content. Do not rewrite files that don't need changes. Preserve the style, tone, and"
  echo "structure of the existing docs -- make the smallest edit that fixes the drift."
  echo ""
  echo "Output format: for each file that needs changes, output exactly:"
  echo "<<<FILE: relative/path.md>>>"
  echo "...full new file content..."
  echo "<<<END>>>"
  echo "If nothing needs updating, output exactly: NO_CHANGES_NEEDED"
  echo ""
  echo "=== CODE DIFF STAT ==="
  cat "$DIFF_STAT"
  echo ""
  echo "=== CODE DIFF (may be truncated) ==="
  cat "$DIFF_FULL"
  echo ""
  echo "=== CURRENT DOC FILES ==="
  while IFS= read -r f; do
    echo "--- FILE: $f ---"
    cat "$f"
    echo ""
  done <<< "$DOC_FILES"
} > "$PROMPT"

echo "Asking $COMBO to review $(echo "$DOC_FILES" | wc -l) doc file(s) against $(wc -l < "$DIFF_STAT") changed source file(s)..."
if ! omniroute chat -m "$COMBO" --file "$PROMPT" --no-history > "$RESPONSE" 2>&1; then
  echo "omniroute call failed:" >&2
  cat "$RESPONSE" >&2
  exit 1
fi

if grep -q "NO_CHANGES_NEEDED" "$RESPONSE"; then
  echo "Model found no doc drift worth fixing. Nothing to do."
  exit 0
fi

CHANGED=$(node -e '
const fs = require("fs");
const path = require("path");
const text = fs.readFileSync(process.argv[1], "utf8");
const re = /<<<FILE: (.+?)>>>\n([\s\S]*?)\n<<<END>>>/g;
let m, count = 0;
while ((m = re.exec(text))) {
  const [, filePath, content] = m;
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
  fs.writeFileSync(filePath, content.endsWith("\n") ? content : content + "\n");
  console.error("Updated: " + filePath);
  count++;
}
console.log(count);
' "$RESPONSE")

if [ "$CHANGED" -eq 0 ]; then
  echo "Could not parse any file blocks from the model response. Raw output:" >&2
  cat "$RESPONSE" >&2
  exit 1
fi

BRANCH="docs-update-$(date +%Y%m%d-%H%M%S)"
git checkout -b "$BRANCH"
git add README.md docs
git commit -m "docs: sync with recent code changes (via $COMBO)"
git push -u origin "$BRANCH"

REMOTE_URL=$(git remote get-url origin | sed -E 's#git@github.com:#https://github.com/#; s#\.git$##')
echo ""
echo "Pushed $BRANCH. Open a PR here:"
echo "$REMOTE_URL/compare/main...$BRANCH?expand=1"
