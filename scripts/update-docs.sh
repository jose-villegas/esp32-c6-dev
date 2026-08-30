#!/usr/bin/env bash
# On-demand doc sync: reviews code changes since docs were last touched, asks
# OmniRoute's free "docs-update-free" combo (gpt-oss:120b -> gpt-oss:20b ->
# github/gpt-4o-mini -> kimi-coding -> ollama-local/gemma4:26b, all $0) which
# doc files are plausibly affected, then sends only those files' full content
# for a rewrite, and pushes a branch for review. Two-pass by design: this
# repo's docs run ~650KB total (one file alone is 200KB+), so dumping every
# doc into every prompt blows past the smallest fallback model's context
# window. Never touches main directly -- open the printed compare URL
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
DOC_INDEX="$TMPDIR/docindex.txt"
SELECT_PROMPT="$TMPDIR/select_prompt.txt"
SELECT_RESPONSE="$TMPDIR/select_response.txt"
UPDATE_PROMPT="$TMPDIR/update_prompt.txt"
UPDATE_RESPONSE="$TMPDIR/update_response.txt"

git diff --stat "$LAST_DOC_COMMIT"..HEAD -- . ':!README.md' ':!docs' > "$DIFF_STAT"
git diff "$LAST_DOC_COMMIT"..HEAD -- . ':!README.md' ':!docs' | head -c 20000 > "$DIFF_FULL"

if [ ! -s "$DIFF_STAT" ]; then
  echo "No code changes since the docs were last touched ($LAST_DOC_COMMIT). Nothing to do."
  exit 0
fi

# Pass 1: cheap index (path + first line) so the model can pick candidates
# without paying for ~650KB of doc content up front.
: > "$DOC_INDEX"
while IFS= read -r f; do
  first_line=$(head -n 1 "$f" 2>/dev/null || true)
  printf '%s | %s\n' "$f" "$first_line" >> "$DOC_INDEX"
done <<< "$DOC_FILES"

{
  echo "A firmware repo's code changed since its docs were last reviewed. Below is a"
  echo "diff summary/excerpt, then an index of every doc file (path | first line)."
  echo "List ONLY the paths (one per line, exactly as given) of doc files that are"
  echo "plausibly now stale or inaccurate because of this diff. If none, output"
  echo "exactly: NONE. Output nothing else -- no explanation, no markdown."
  echo ""
  echo "=== DIFF STAT ==="
  cat "$DIFF_STAT"
  echo ""
  echo "=== DIFF EXCERPT ==="
  cat "$DIFF_FULL"
  echo ""
  echo "=== DOC INDEX ==="
  cat "$DOC_INDEX"
} > "$SELECT_PROMPT"

echo "Pass 1/2: asking $COMBO which of $(wc -l < "$DOC_INDEX") doc file(s) look affected..."
if ! omniroute chat -m "$COMBO" --file "$SELECT_PROMPT" --no-history > "$SELECT_RESPONSE" 2>&1; then
  echo "omniroute call failed:" >&2
  cat "$SELECT_RESPONSE" >&2
  exit 1
fi

if grep -qx "NONE" "$SELECT_RESPONSE"; then
  echo "Model found no doc drift worth fixing. Nothing to do."
  exit 0
fi

# Keep only lines that exactly match a known doc path -- ignore any
# hallucinated or malformed paths rather than trying to write them.
mapfile -t SELECTED < <(grep -Fxf <(echo "$DOC_FILES") "$SELECT_RESPONSE" | sort -u | head -5)

if [ "${#SELECTED[@]}" -eq 0 ]; then
  echo "Model didn't name any known doc file. Raw response:" >&2
  cat "$SELECT_RESPONSE" >&2
  exit 0
fi

echo "Pass 2/2: requesting rewrites for: ${SELECTED[*]}"

{
  echo "You are updating technical documentation for a firmware repo (ESP32-C6 app shell)."
  echo "Below is the code diff, followed by the full current content of the doc file(s)"
  echo "identified as stale. Output their FULL corrected content. Preserve the style,"
  echo "tone, and structure of the existing docs -- make the smallest edit that fixes"
  echo "the drift. If, after reading the full file, it turns out no change is actually"
  echo "needed, omit it from the output entirely."
  echo ""
  echo "Output format: for each file that needs changes, output exactly:"
  echo "<<<FILE: relative/path.md>>>"
  echo "...full new file content..."
  echo "<<<END>>>"
  echo "If none of them need changes after all, output exactly: NO_CHANGES_NEEDED"
  echo ""
  echo "=== DIFF STAT ==="
  cat "$DIFF_STAT"
  echo ""
  echo "=== DIFF EXCERPT ==="
  cat "$DIFF_FULL"
  echo ""
  echo "=== DOC FILES TO REVIEW ==="
  for f in "${SELECTED[@]}"; do
    echo "--- FILE: $f ---"
    cat "$f"
    echo ""
  done
} > "$UPDATE_PROMPT"

if ! omniroute chat -m "$COMBO" --file "$UPDATE_PROMPT" --no-history > "$UPDATE_RESPONSE" 2>&1; then
  echo "omniroute call failed:" >&2
  cat "$UPDATE_RESPONSE" >&2
  exit 1
fi

if grep -q "NO_CHANGES_NEEDED" "$UPDATE_RESPONSE"; then
  echo "Model found no doc drift worth fixing on closer inspection. Nothing to do."
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
' "$UPDATE_RESPONSE")

if [ "$CHANGED" -eq 0 ]; then
  echo "Could not parse any file blocks from the model response. Raw output:" >&2
  cat "$UPDATE_RESPONSE" >&2
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
