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
# --worktree runs the whole thing in a fresh `git worktree` (sibling
# directory) instead of checking out the new branch in place, so your
# current checkout's HEAD and any uncommitted work never move -- useful if
# you want to keep working while this runs, or don't want the "working tree
# isn't clean" gate to apply at all (it's skipped in this mode: nothing here
# touches your real checkout, so there's nothing to protect it from). If
# nothing ends up changing, the worktree and its branch are removed
# automatically; if something does, both are left behind for you to inspect
# before opening the PR.
#
# --no-push commits on the branch (worktree or not) but stops short of
# `git push` -- the branch/worktree is left for you to inspect and push
# yourself when ready.
#
# --local skips OmniRoute (including its own "docs-update-free" combo used
# by audit-docs.sh's tier-2 cross-check, forwarded --local there too) and
# calls Ollama directly (`ollama run`) for every model call this makes, so
# the whole run makes zero network calls to any cloud provider. See Model-
# Delegation-Workflow.md's "Route local through the Ollama CLI directly, not
# OmniRoute" -- OmniRoute's own ollama-local provider has no working
# connection pool. Uses gemma4:26b by default (LOCAL_MODEL env var to
# override); if you also pass --review with --local, REVIEW_MODEL is treated
# as a local Ollama tag too, not a cloud model id.
#
# Usage: scripts/fix-audited-docs.sh [--review <model-id>] [--worktree] [--local] [--no-push] [doc-file ...]
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

COMBO="docs-update-free"
REVIEW_MODEL=""
USE_WORKTREE=0
LOCAL_MODE=0
LOCAL_MODEL="${LOCAL_MODEL:-gemma4:26b}"
NO_PUSH=0
DOC_ARGS=()

while [ "$#" -gt 0 ]; do
  case "$1" in
    --review)
      REVIEW_MODEL="$2"
      shift 2
      ;;
    --worktree)
      USE_WORKTREE=1
      shift
      ;;
    --local)
      LOCAL_MODE=1
      shift
      ;;
    --no-push)
      NO_PUSH=1
      shift
      ;;
    *)
      DOC_ARGS+=("$1")
      shift
      ;;
  esac
done
[ "$LOCAL_MODE" = "1" ] && COMBO="$LOCAL_MODEL"

BRANCH="docs-audit-fix-$(date +%Y%m%d-%H%M%S)"
WORKTREE_DIR=""
HAS_CHANGES=0
TMPDIR=$(mktemp -d)

cleanup() {
  rm -rf "$TMPDIR"
  # Only torn down if nothing was ever approved to commit -- see HAS_CHANGES
  # below. A worktree that made it to real changes is left for inspection
  # even if a later step (add/commit/push) fails.
  if [ -n "$WORKTREE_DIR" ] && [ "$HAS_CHANGES" != "1" ]; then
    git -C "$REPO_ROOT" worktree remove "$WORKTREE_DIR" --force 2>/dev/null || true
    git -C "$REPO_ROOT" branch -D "$BRANCH" 2>/dev/null || true
  fi
}
trap cleanup EXIT

if [ "$USE_WORKTREE" = "1" ]; then
  WORKTREE_DIR="$(dirname "$REPO_ROOT")/$(basename "$REPO_ROOT")-$BRANCH"
  echo "Creating worktree at $WORKTREE_DIR..."
  git worktree add -b "$BRANCH" "$WORKTREE_DIR" >&2
  cd "$WORKTREE_DIR"
else
  if [ -n "$(git status --porcelain)" ]; then
    echo "Working tree isn't clean. Commit or stash first." >&2
    exit 1
  fi
fi

AUDIT_OUT="$TMPDIR/audit_out.txt"
PATCHES="$TMPDIR/patches.json"
echo "[]" > "$PATCHES"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AUDIT_EXTRA_ARGS=()
[ "$LOCAL_MODE" = "1" ] && AUDIT_EXTRA_ARGS+=(--local)
echo "Running audit-docs.sh..."
"$SCRIPT_DIR/audit-docs.sh" "${AUDIT_EXTRA_ARGS[@]}" ${DOC_ARGS[@]+"${DOC_ARGS[@]}"} > "$AUDIT_OUT" 2>&1 || true
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

# chat_call PROMPT_FILE OUT_FILE MODEL_ID MAX_TOKENS [REASONING_EFFORT] --
# writes the model's raw text response (unwrapped from any provider
# envelope) to OUT_FILE. LOCAL_MODE=1 calls Ollama directly instead of
# OmniRoute -- see the --local comment near the top of this file. Returns
# nonzero on failure.
#
# --think=false is load-bearing, not decoration: confirmed live that
# reasoning-capable local models (gemma4:26b included, not just the
# obviously-named deepseek-r1 ones) print a full "Thinking... ...done
# thinking." preamble to stdout by default in plain text, no <think> tags to
# strip -- left in, it would corrupt the find/replace JSON array this
# script scans for. See fix-audited-code.sh's chat_call for the fuller
# writeup.
chat_call() {
  local prompt_file="$1" out_file="$2" model_id="$3" max_tokens="$4" effort="${5:-}"
  if [ "$LOCAL_MODE" = "1" ]; then
    ollama run "$model_id" --think=false < "$prompt_file" > "$out_file" 2>/dev/null
    return $?
  fi
  local envelope="$out_file.envelope"
  local effort_args=()
  [ -n "$effort" ] && effort_args=(--reasoning-effort "$effort")
  if ! omniroute --output json chat -m "$model_id" "${effort_args[@]}" --max-tokens "$max_tokens" \
        --file "$prompt_file" --no-history > "$envelope" 2>&1; then
    return 1
  fi
  node -e "$extract_content_js" "$envelope" > "$out_file" 2>/dev/null
}

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
  FIX_CONTENT="$TMPDIR/fix_content.txt"

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

  if ! chat_call "$FIX_PROMPT" "$FIX_CONTENT" "$COMBO" 2000 low; then
    echo "  model call failed" >&2
    continue
  fi

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
  REVIEW_CONTENT_FILE="$TMPDIR/review_content.txt"
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

  if ! chat_call "$REVIEW_PROMPT" "$REVIEW_CONTENT_FILE" "$REVIEW_MODEL" 4000 low; then
    echo "Review call failed (keeping all patches unreviewed)." >&2
    echo "[]" > "$REVIEW_CONTENT_FILE"
  fi

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

HAS_CHANGES=1
if [ "$USE_WORKTREE" != "1" ]; then
  git checkout -b "$BRANCH"
fi
git add $CHANGED_FILES
REVIEW_NOTE=""
[ -n "$REVIEW_MODEL" ] && REVIEW_NOTE=", reviewed by $REVIEW_MODEL"
git commit -m "docs: fix audit findings (via $COMBO$REVIEW_NOTE)"

if [ "$NO_PUSH" = "1" ]; then
  echo ""
  echo "Committed on branch $BRANCH (--no-push: not pushed)."
  if [ -n "$WORKTREE_DIR" ]; then
    echo "Worktree left at $WORKTREE_DIR for inspection. When ready:"
    echo "  cd \"$WORKTREE_DIR\" && git push -u origin $BRANCH"
    echo "  git worktree remove \"$WORKTREE_DIR\"   # when done with it"
  else
    echo "Push it yourself when ready: git push -u origin $BRANCH"
  fi
else
  git push -u origin "$BRANCH"

  REMOTE_URL=$(git remote get-url origin | sed -E 's#git@github.com:#https://github.com/#; s#\.git$##')
  echo ""
  echo "Pushed $BRANCH. Open a PR here:"
  echo "$REMOTE_URL/compare/main...$BRANCH?expand=1"
  if [ -n "$WORKTREE_DIR" ]; then
    echo ""
    echo "Worktree left at $WORKTREE_DIR for inspection -- remove when done:"
    echo "  git worktree remove \"$WORKTREE_DIR\""
  fi
fi
