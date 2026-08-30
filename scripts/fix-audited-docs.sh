#!/usr/bin/env bash
# Runs audit-docs.sh, then asks OmniRoute's free "docs-update-free" combo to
# turn each finding into a small, exact find/replace patch (not a full-file
# rewrite -- keeps this cheap and bounded no matter how big the doc is).
# Patches are verified against the real file content before being applied;
# anything that doesn't match exactly is skipped and reported, not forced.
#
# Optionally review the proposed patches with a paid model before applying:
#   scripts/fix-audited-docs.sh --review claude/claude-sonnet-5
#   scripts/fix-audited-docs.sh --review codex/gpt-5.5
# Without --review, every patch the free model proposes (that verifies
# against the file) is applied directly. Pushes a branch for you either way
# -- never touches main.
#
# Usage: scripts/fix-audited-docs.sh [--review <model-id>] [doc-file ...]
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

COMBO="docs-update-free"
REVIEW_MODEL=""
DOC_ARGS=()

while [ "$#" -gt 0 ]; do
  case "$1" in
    --review)
      REVIEW_MODEL="$2"
      shift 2
      ;;
    *)
      DOC_ARGS+=("$1")
      shift
      ;;
  esac
done

if [ -n "$(git status --porcelain)" ]; then
  echo "Working tree isn't clean. Commit or stash first." >&2
  exit 1
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

AUDIT_OUT="$TMPDIR/audit_out.txt"
PATCHES="$TMPDIR/patches.json"
echo "[]" > "$PATCHES"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "Running audit-docs.sh..."
"$SCRIPT_DIR/audit-docs.sh" ${DOC_ARGS[@]+"${DOC_ARGS[@]}"} > "$AUDIT_OUT" 2>&1 || true
cat "$AUDIT_OUT"

if ! grep -q "AUDIT REPORT" "$AUDIT_OUT"; then
  echo "Audit produced no report section; treating as clean. Nothing to do."
  exit 0
fi

# Split the report into per-file finding blocks (## path, optionally
# "## path (content)" for the LLM cross-check section -- merge both under
# the real path).
mapfile -t FILES_WITH_FINDINGS < <(
  sed -n '/AUDIT REPORT/,$p' "$AUDIT_OUT" \
    | grep -E '^## ' | sed -E 's/^## //; s/ \(content\)$//' | sort -u
)

if [ "${#FILES_WITH_FINDINGS[@]}" -eq 0 ]; then
  echo "No findings to fix. Nothing to do."
  exit 0
fi

MAX_DOC_CHARS=80000

# Pulls choices[0].message.content out of an `omniroute --output json chat`
# response. Finding the first "{" / last "}" is safe here because the only
# other thing on that line (the "Loaded env" banner) is plain text plus
# ANSI color codes, and ANSI codes use square brackets, never braces.
extract_content_js='
  const fs = require("fs");
  const text = fs.readFileSync(process.argv[1], "utf8");
  const start = text.indexOf("{");
  const end = text.lastIndexOf("}");
  if (start === -1 || end === -1) { process.exit(1); }
  const envelope = JSON.parse(text.slice(start, end + 1));
  process.stdout.write(envelope.choices[0].message.content);
'

# A model-written find/replace string can itself contain "[" or "]" (e.g.
# markdown link syntax), so naive indexOf/lastIndexOf on brackets is not
# reliable. This scans from the first "[" and tracks bracket depth while
# skipping over string literals (respecting escapes), to find the true
# matching close of the outer array. Written to a file so both parsing
# steps below can require() it without re-embedding the same logic twice.
cat > "$TMPDIR/extract-array.js" << "JSEOF"
function extractJsonArray(text) {
  const startIdx = text.indexOf("[");
  if (startIdx === -1) return null;
  let depth = 0, inStr = false, esc = false;
  for (let i = startIdx; i < text.length; i++) {
    const c = text[i];
    if (inStr) {
      if (esc) esc = false;
      else if (c === "\\") esc = true;
      else if (c === "\"") inStr = false;
      continue;
    }
    if (c === "\"") { inStr = true; continue; }
    if (c === "[") depth++;
    else if (c === "]") {
      depth--;
      if (depth === 0) return text.slice(startIdx, i + 1);
    }
  }
  return null;
}
module.exports = { extractJsonArray };
JSEOF

for doc in "${FILES_WITH_FINDINGS[@]}"; do
  [ -f "$doc" ] || continue
  echo ""
  echo "=== fixing: $doc ==="

  FINDINGS=$(awk -v want="$doc" '
    BEGIN { keep = 0 }
    /^## / {
      line = $0
      sub(/^## /, "", line)
      sub(/ \(content\)$/, "", line)
      keep = (line == want)
      next
    }
    keep { print }
  ' "$AUDIT_OUT")

  if [ -z "$(echo "$FINDINGS" | tr -d '[:space:]')" ]; then
    continue
  fi

  FIX_PROMPT="$TMPDIR/fix_prompt.txt"
  FIX_RESPONSE="$TMPDIR/fix_response.txt"

  {
    echo "The following audit findings were raised against one documentation file."
    echo "For each finding you can resolve as a single exact text substitution,"
    echo "output a JSON object: {\"find\": \"<exact substring, verbatim, from the doc"
    echo "text below>\", \"replace\": \"<corrected substring>\"}. The \"find\" string"
    echo "MUST appear character-for-character in the doc text below -- do not"
    echo "paraphrase it. Skip any finding that needs more than a simple substitution"
    echo "(rewriting a paragraph, something subjective, anything you're not fully"
    echo "confident about)."
    echo ""
    echo "Output ONLY a JSON array of such objects, nothing else -- no markdown"
    echo "fences, no commentary. If none apply, output exactly: []"
    echo ""
    echo "=== FINDINGS ==="
    echo "$FINDINGS"
    echo ""
    echo "=== DOC: $doc ==="
    head -c "$MAX_DOC_CHARS" "$doc"
  } > "$FIX_PROMPT"

  if ! omniroute --output json chat -m "$COMBO" --reasoning-effort low --max-tokens 2000 \
        --file "$FIX_PROMPT" --no-history > "$FIX_RESPONSE" 2>&1; then
    echo "  omniroute call failed:" >&2
    cat "$FIX_RESPONSE" >&2
    continue
  fi

  CONTENT=$(node -e "$extract_content_js" "$FIX_RESPONSE") || { echo "  no response envelope found" >&2; continue; }
  echo "$CONTENT" > "$TMPDIR/fix_content.txt"

  node -e '
    const fs = require("fs");
    const [, extractorPath, contentFile, docPath, patchesFile] = process.argv;
    const { extractJsonArray } = require(extractorPath);
    const text = fs.readFileSync(contentFile, "utf8");
    const slice = extractJsonArray(text);
    if (!slice) { console.error("  no JSON array found in response"); process.exit(0); }
    let arr;
    try { arr = JSON.parse(slice); }
    catch (e) { console.error("  could not parse JSON: " + e.message); process.exit(0); }

    const docContent = fs.readFileSync(docPath, "utf8");
    const all = JSON.parse(fs.readFileSync(patchesFile, "utf8"));
    for (const p of arr) {
      if (!p || typeof p.find !== "string" || typeof p.replace !== "string") continue;
      if (!docContent.includes(p.find)) {
        console.error(`  SKIP (no exact match in file): "${p.find.slice(0, 60)}"`);
        continue;
      }
      console.error(`  patch: "${p.find.slice(0, 60)}" -> "${p.replace.slice(0, 60)}"`);
      all.push({ file: docPath, find: p.find, replace: p.replace });
    }
    fs.writeFileSync(patchesFile, JSON.stringify(all));
  ' "$TMPDIR/extract-array.js" "$TMPDIR/fix_content.txt" "$doc" "$PATCHES"
done

PATCH_COUNT=$(node -e 'console.log(JSON.parse(require("fs").readFileSync(process.argv[1], "utf8")).length)' "$PATCHES")
echo ""
echo "$PATCH_COUNT verified patch(es) proposed."
if [ "$PATCH_COUNT" -eq 0 ]; then
  echo "Nothing to apply."
  exit 0
fi

if [ -n "$REVIEW_MODEL" ]; then
  echo "Sending patches to $REVIEW_MODEL for review..."
  REVIEW_PROMPT="$TMPDIR/review_prompt.txt"
  REVIEW_RESPONSE="$TMPDIR/review_response.txt"
  {
    echo "Here is a JSON array of proposed documentation fixes, each a find/replace"
    echo "patch responding to an audit finding. For each item (by its 0-based index"
    echo "in the array), decide VALID (the replacement correctly and minimally fixes"
    echo "what 'find' names, without introducing an error) or INVALID."
    echo ""
    echo "Output ONLY a JSON array of {\"index\": N, \"verdict\": \"VALID\"|\"INVALID\","
    echo "\"reason\": \"...\"}, one entry per patch, nothing else."
    echo ""
    cat "$PATCHES"
  } > "$REVIEW_PROMPT"

  REVIEW_OK=1
  if ! omniroute --output json chat -m "$REVIEW_MODEL" --reasoning-effort low --max-tokens 4000 \
        --file "$REVIEW_PROMPT" --no-history > "$REVIEW_RESPONSE" 2>&1; then
    echo "Review call failed (keeping all patches unreviewed):" >&2
    cat "$REVIEW_RESPONSE" >&2
    REVIEW_OK=0
  fi

  REVIEW_CONTENT="[]"
  if [ "$REVIEW_OK" -eq 1 ]; then
    REVIEW_CONTENT=$(node -e "$extract_content_js" "$REVIEW_RESPONSE") || { echo "no response envelope found, keeping all patches unreviewed" >&2; REVIEW_CONTENT="[]"; }
  fi
  echo "$REVIEW_CONTENT" > "$TMPDIR/review_content.txt"

  node -e '
    const fs = require("fs");
    const [, extractorPath, contentFile, patchesFile] = process.argv;
    const { extractJsonArray } = require(extractorPath);
    const text = fs.readFileSync(contentFile, "utf8");
    const slice = extractJsonArray(text);
    const all = JSON.parse(fs.readFileSync(patchesFile, "utf8"));
    if (!slice) { console.error("no JSON verdicts found, keeping all patches unreviewed"); process.exit(0); }
    let verdicts;
    try { verdicts = JSON.parse(slice); }
    catch (err) { console.error("could not parse verdicts, keeping all patches unreviewed"); process.exit(0); }
    const byIndex = new Map(verdicts.map(v => [v.index, v]));
    const kept = [];
    all.forEach((p, i) => {
      const v = byIndex.get(i);
      if (v && v.verdict === "INVALID") {
        console.error(`  REJECTED [${p.file}] "${p.find.slice(0,50)}" -> "${p.replace.slice(0,50)}": ${v.reason || "no reason given"}`);
      } else {
        kept.push(p);
      }
    });
    fs.writeFileSync(patchesFile, JSON.stringify(kept));
    console.error(`${kept.length}/${all.length} patch(es) passed review.`);
  ' "$TMPDIR/extract-array.js" "$TMPDIR/review_content.txt" "$PATCHES"

  PATCH_COUNT=$(node -e 'console.log(JSON.parse(require("fs").readFileSync(process.argv[1], "utf8")).length)' "$PATCHES")
  if [ "$PATCH_COUNT" -eq 0 ]; then
    echo "No patches survived review. Nothing to apply."
    exit 0
  fi
fi

CHANGED_FILES=$(node -e '
  const fs = require("fs");
  const patches = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
  const byFile = new Map();
  for (const p of patches) {
    if (!byFile.has(p.file)) byFile.set(p.file, []);
    byFile.get(p.file).push(p);
  }
  for (const [file, patchList] of byFile) {
    let content = fs.readFileSync(file, "utf8");
    for (const p of patchList) {
      if (!content.includes(p.find)) {
        console.error(`  SKIP at apply time (no longer matches) [${file}]: "${p.find.slice(0,60)}"`);
        continue;
      }
      content = content.split(p.find).join(p.replace);
    }
    fs.writeFileSync(file, content);
    console.log(file);
  }
' "$PATCHES")

if [ -z "$CHANGED_FILES" ]; then
  echo "No files actually changed. Nothing to commit."
  exit 0
fi

BRANCH="docs-audit-fix-$(date +%Y%m%d-%H%M%S)"
git checkout -b "$BRANCH"
git add $CHANGED_FILES
REVIEW_NOTE=""
[ -n "$REVIEW_MODEL" ] && REVIEW_NOTE=", reviewed by $REVIEW_MODEL"
git commit -m "docs: fix audit findings (via $COMBO$REVIEW_NOTE)"
git push -u origin "$BRANCH"

REMOTE_URL=$(git remote get-url origin | sed -E 's#git@github.com:#https://github.com/#; s#\.git$##')
echo ""
echo "Pushed $BRANCH. Open a PR here:"
echo "$REMOTE_URL/compare/main...$BRANCH?expand=1"
