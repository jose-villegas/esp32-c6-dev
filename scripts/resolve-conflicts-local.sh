#!/usr/bin/env bash
# Auto-resolve a git merge's conflicts with local Ollama models, verify the
# result with THIS repo's real build/test gate, and only ever commit if that
# gate is green. Zero cloud calls -- see docs/Model-Delegation-Workflow.md's
# "Route local through the Ollama CLI directly" section, which this follows
# exactly (--think=false, stderr routed to a log not merged into stdout,
# qwen2.5-coder:32b-instruct-q4_K_M / gemma4:26b as fixer/reviewer).
#
# WHY A HARD TEST GATE, NOT JUST A REVIEWER MODEL
#
# A reviewer model is a second opinion, not proof. The one thing that
# actually verifies a conflict resolution is correct is the same thing that
# verifies any other change here: ./launcher/test/run_tests.sh and
# check_app_sources.sh (the latter compile-checks the hardware-facing
# app_*.c files run_tests.sh cannot link), plus report_reactions.sh --check
# when it exists (sand's reaction-doc generator can compile clean and pass
# every host test while still leaving docs/Sand/Reaction-Table.md stale --
# a real regression this script's own first end-to-end replay test against
# an actual historical conflict caught escaping the first two checks). A
# resolution that reads plausibly but breaks the build, a behavioral
# assertion, or a generated doc is exactly the failure mode "looks done but
# isn't" that must never reach a commit here -- see Model-Delegation-
# Workflow.md's own "treat a test failure as the process working, not
# failing".
#
# HOW A CONFLICT IS SPLIT INTO CALLS
#
# One hunk, one fixer call, one reviewer call -- never a whole file, let
# alone a whole merge, in one prompt. Model-Delegation-Workflow.md step 3
# found that large prompts to local models reliably time out where small,
# single-purpose ones come back in seconds; a conflict hunk plus a few
# lines of surrounding context is already about as small as a real edit
# gets. parse-conflicts.js (embedded below, same pattern as fix-audited-
# code.sh's extract-content.js/extract-array.js) walks each conflicted
# file's <<<<<<</=======/>>>>>>> markers (and the diff3 ||||||| base
# section, when present) into one (before, ours, base, theirs, after) set
# of files per hunk, then re-assembles the file afterward by swapping each
# hunk's span for its own hunk-NNNN.resolved file -- or leaving that span's
# original markers untouched if no .resolved file exists, so a hunk the
# review loop never approved stays a real, visible conflict rather than
# silently vanishing.
#
# REVIEW LOOP, PER HUNK
#
# gemma4:26b (a different model family from the qwen2.5-coder fixer, so
# this is a genuine second opinion rather than the same model checking its
# own work -- see the workflow doc's "pick a genuinely different model
# family" note) gets the same context plus the fixer's proposed resolution
# and replies VALID or INVALID: <reason>. INVALID sends the reason back to
# the fixer for another attempt, up to --rounds times; still-INVALID after
# that leaves the hunk unresolved rather than force an unreviewed answer
# in. A file with ANY unresolved hunk is left conflicted (not staged) --
# the script only proceeds to the test gate once EVERY conflicted file
# fully resolved, and refuses to commit otherwise.
#
# WHAT HAPPENS ON FAILURE, AT EITHER GATE
#
# Never `git merge --abort`, never force a bad commit. If hunks are left
# unresolved, the repo sits exactly where a normal failed auto-merge would:
# mid-merge, conflicts visible via `git status`, ready for you to finish by
# hand or re-run this script (already-resolved files stay staged, so a
# re-run only redoes what's left -- parse-conflicts.js re-derives hunk
# files fresh each run, it does not remember a previous attempt). If every
# hunk resolves but the test gate goes red, the resolution is left staged
# but UNCOMMITTED for you to inspect -- `git diff --cached` shows exactly
# what would have been committed.
#
# Usage:
#   scripts/resolve-conflicts-local.sh [--target <branch>] [--worktree]
#       [--no-push] [--rounds N] [--context N]
#
# --target <branch>   what to merge in if not already mid-merge (default
#                      main). Ignored if a merge is already in progress
#                      (.git's MERGE_HEAD exists) -- this script then just
#                      finishes resolving whatever is already conflicted.
# --worktree           do all of this in a fresh sibling worktree on a new
#                       temp branch instead of the current checkout, then
#                       (on success) `git push` that temp branch straight to
#                       `origin/<your-current-branch>` -- the git mechanic
#                       Model-Delegation-Workflow.md documents as the correct
#                       way to land work on a branch checked out elsewhere
#                       from inside a worktree. Left behind for inspection
#                       if anything was resolved but the run did not reach a
#                       clean push (mirrors fix-audited-code.sh --worktree).
# --no-push            commit (if the gate is green) but stop short of
#                       `git push` -- inspect first, push yourself.
# --rounds N           max fixer attempts per hunk after a reviewer INVALID
#                       (default 2, same default as fix-audited-code.sh).
# --context N          lines of surrounding context shown to the models on
#                       each side of a hunk (default 6).
#
# Env overrides (same override convention as fix-audited-code.sh's
# LOCAL_FIXER_MODEL/LOCAL_REVIEW_MODEL, kept as distinct names so setting
# one script's models never silently changes the other's):
#   CONFLICT_FIXER_MODEL   default qwen2.5-coder:32b-instruct-q4_K_M
#   CONFLICT_REVIEW_MODEL  default gemma4:26b
# See docs/Model-Delegation-Workflow.md's "Which local model for which job"
# for why these two and what to fall back to if either is not pulled
# locally (`ollama list`) -- mistral-nemo:latest for the reviewer is the
# documented safe fallback if gemma4:26b's --think=false ever proves
# unreliable on a given Ollama version.
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

TARGET="main"
USE_WORKTREE=0
NO_PUSH=0
MAX_ROUNDS=2
CONTEXT=6

while [ "$#" -gt 0 ]; do
  case "$1" in
    --target)
      TARGET="$2"
      shift 2
      ;;
    --worktree)
      USE_WORKTREE=1
      shift
      ;;
    --no-push)
      NO_PUSH=1
      shift
      ;;
    --rounds)
      MAX_ROUNDS="$2"
      shift 2
      ;;
    --context)
      CONTEXT="$2"
      shift 2
      ;;
    -h|--help)
      sed -n '2,90p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

FIXER_MODEL="${CONFLICT_FIXER_MODEL:-qwen2.5-coder:32b-instruct-q4_K_M}"
REVIEW_MODEL="${CONFLICT_REVIEW_MODEL:-gemma4:26b}"

for m in "$FIXER_MODEL" "$REVIEW_MODEL"; do
  if ! ollama list | awk '{print $1}' | grep -qxF "$m"; then
    echo "Model '$m' is not in \`ollama list\`. Pull it first, or override" >&2
    echo "via CONFLICT_FIXER_MODEL / CONFLICT_REVIEW_MODEL env vars." >&2
    echo "Currently pulled:" >&2
    ollama list >&2
    exit 1
  fi
done

ORIG_BRANCH="$(git symbolic-ref --short -q HEAD || true)"
if [ -z "$ORIG_BRANCH" ]; then
  echo "HEAD is detached -- checkout a real branch first." >&2
  exit 1
fi

TMPDIR=$(mktemp -d)
WORKTREE_DIR=""
TMP_BRANCH=""
HAS_CHANGES=0

cleanup() {
  rm -rf "$TMPDIR"
  # Same rule as fix-audited-code.sh: only auto-removed if nothing was ever
  # resolved. Anything that made it to a real change is left for you to
  # inspect, whether the run finished clean or not.
  if [ -n "$WORKTREE_DIR" ] && [ "$HAS_CHANGES" != "1" ]; then
    git -C "$REPO_ROOT" worktree remove "$WORKTREE_DIR" --force 2>/dev/null || true
    git -C "$REPO_ROOT" branch -D "$TMP_BRANCH" 2>/dev/null || true
  fi
}
trap cleanup EXIT

if [ "$USE_WORKTREE" = "1" ]; then
  TMP_BRANCH="conflict-resolve-$(date +%Y%m%d-%H%M%S)"
  WORKTREE_DIR="$(dirname "$REPO_ROOT")/$(basename "$REPO_ROOT")-$TMP_BRANCH"
  echo "Creating worktree at $WORKTREE_DIR (branch $TMP_BRANCH, from $ORIG_BRANCH)..."
  git worktree add -b "$TMP_BRANCH" "$WORKTREE_DIR" "$ORIG_BRANCH" >&2
  cd "$WORKTREE_DIR"
else
  # Not a worktree run: refuse to touch a checkout that has unrelated
  # uncommitted work sitting in it, so a merge commit here can never
  # silently absorb something you were still in the middle of. A merge
  # already in progress is fine -- that IS the expected starting state.
  if ! git rev-parse -q --verify MERGE_HEAD >/dev/null 2>&1; then
    if [ -n "$(git status --porcelain)" ]; then
      echo "Working tree isn't clean and no merge is in progress -- commit," >&2
      echo "stash, or pass --worktree to run this in an isolated copy instead." >&2
      exit 1
    fi
  fi
fi

# --- parse-conflicts.js: extract each hunk to files, or re-assemble a file
# from resolved hunks afterward. Same embedded-script pattern as fix-
# audited-code.sh's extract-content.js/extract-array.js.
cat > "$TMPDIR/parse-conflicts.js" << "JSEOF"
const fs = require("fs");

function splitLines(text) {
  // Keep it simple: trailing-newline handling matches what join("\n") + a
  // final "\n" reproduces exactly, which apply() relies on.
  const lines = text.split("\n");
  if (lines.length && lines[lines.length - 1] === "") lines.pop();
  return lines;
}

const OURS_RE = /^<<<<<<< /;
const BASE_RE = /^\|\|\|\|\|\|\| /;
const SEP_RE  = /^=======$/;
const THEIRS_RE = /^>>>>>>> /;

function mode() { return process.argv[2]; }

if (mode() === "extract") {
  const [, , , file, outdir, ctxArg] = process.argv;
  const ctx = parseInt(ctxArg, 10);
  const lines = splitLines(fs.readFileSync(file, "utf8"));

  let state = "NORMAL";
  let before = [];               // rolling window of the last `ctx` NORMAL lines
  let ours = [], base = [], theirs = [];
  let hunk = 0;
  let afterRemaining = 0;        // lines still owed to the PREVIOUS hunk's .after

  for (const line of lines) {
    if (state === "NORMAL" && OURS_RE.test(line)) {
      hunk++;
      fs.writeFileSync(`${outdir}/hunk-${String(hunk).padStart(4, "0")}.before`,
                        before.join("\n"));
      ours = []; base = []; theirs = [];
      state = "OURS";
      continue;
    }
    if (state === "OURS" && BASE_RE.test(line)) { state = "BASE"; continue; }
    if ((state === "OURS" || state === "BASE") && SEP_RE.test(line)) {
      state = "THEIRS";
      continue;
    }
    if (state === "THEIRS" && THEIRS_RE.test(line)) {
      fs.writeFileSync(`${outdir}/hunk-${String(hunk).padStart(4, "0")}.ours`,
                        ours.join("\n"));
      fs.writeFileSync(`${outdir}/hunk-${String(hunk).padStart(4, "0")}.base`,
                        base.join("\n"));
      fs.writeFileSync(`${outdir}/hunk-${String(hunk).padStart(4, "0")}.theirs`,
                        theirs.join("\n"));
      state = "NORMAL";
      afterRemaining = ctx;
      before = [];              // this hunk's own content must not leak into
                                 // the NEXT hunk's "before" window
      continue;
    }
    if (state === "OURS") { ours.push(line); continue; }
    if (state === "BASE") { base.push(line); continue; }
    if (state === "THEIRS") { theirs.push(line); continue; }

    // state === NORMAL
    if (afterRemaining > 0) {
      const f = `${outdir}/hunk-${String(hunk).padStart(4, "0")}.after`;
      fs.appendFileSync(f, (fs.existsSync(f) ? "\n" : "") + line);
      afterRemaining--;
    }
    before.push(line);
    if (before.length > ctx) before.shift();
  }
  // Hunks whose .after never got any lines appended (closed right at EOF)
  // still need an (empty) file so the bash side can read it unconditionally.
  for (let h = 1; h <= hunk; h++) {
    const f = `${outdir}/hunk-${String(h).padStart(4, "0")}.after`;
    if (!fs.existsSync(f)) fs.writeFileSync(f, "");
  }
  fs.writeFileSync(`${outdir}/hunk-count`, String(hunk));
  process.exit(0);
}

if (mode() === "apply") {
  const [, , , file, outdir] = process.argv;
  const lines = splitLines(fs.readFileSync(file, "utf8"));
  const out = [];
  let state = "NORMAL";
  let hunk = 0;
  let span = [];                // the original conflict block, kept verbatim
                                 // as a fallback if no .resolved file exists

  for (const line of lines) {
    if (state === "NORMAL" && OURS_RE.test(line)) {
      hunk++;
      state = "IN_CONFLICT";
      span = [line];
      continue;
    }
    if (state === "IN_CONFLICT") {
      span.push(line);
      if (THEIRS_RE.test(line)) {
        const resolved = `${outdir}/hunk-${String(hunk).padStart(4, "0")}.resolved`;
        if (fs.existsSync(resolved)) {
          const text = fs.readFileSync(resolved, "utf8").replace(/\n+$/, "");
          if (text.length) out.push(...text.split("\n"));
        } else {
          out.push(...span);   // leave this hunk genuinely conflicted
        }
        state = "NORMAL";
      }
      continue;
    }
    out.push(line);
  }
  fs.writeFileSync(file, out.join("\n") + "\n");
  process.exit(0);
}

console.error(`Unknown mode '${mode()}' (expected extract|apply)`);
process.exit(1);
JSEOF

# chat_call PROMPT_FILE OUT_FILE MODEL_ID LOG_FILE
# --think=false and stderr routed to a log (never merged into stdout) --
# see this file's own top comment and Model-Delegation-Workflow.md step 4
# for why both are load-bearing, not stylistic: without --think=false a
# reasoning-capable model's "Thinking... ...done thinking." preamble lands
# on stdout ahead of the real answer, and merging stderr in adds the
# spinner's raw ANSI cursor-control bytes on top of that -- both would
# corrupt the "=== RESOLVED ===" extraction below.
chat_call() {
  local prompt_file="$1" out_file="$2" model_id="$3" log_file="$4"
  ollama run "$model_id" --think=false < "$prompt_file" > "$out_file" 2>>"$log_file"
}

# extract_marked FILE START_MARKER END_MARKER -- prints whatever text sits
# strictly between the first line equal to START_MARKER and the next line
# equal to END_MARKER. Used for both "=== RESOLVED ===" and the reviewer's
# verdict line, so a stray unrelated line elsewhere in the model's answer
# (should --think=false ever leak something anyway) can't be mistaken for
# the real payload.
extract_marked() {
  awk -v s="$2" -v e="$3" '
    $0 == s { grabbing = 1; next }
    $0 == e { grabbing = 0 }
    grabbing { print }
  ' "$1"
}

build_hunk_prompt() {
  local hunkdir="$1" n="$2" file="$3" out="$4" extra_reason="$5"
  local base_section=""
  if [ -s "$hunkdir/hunk-$n.base" ]; then
    base_section="--- COMMON ANCESTOR (before either side changed it) ---
$(cat "$hunkdir/hunk-$n.base")

"
  fi
  {
    echo "You are resolving ONE git merge conflict hunk in a C/C++ embedded"
    echo "firmware project (ESP32, no OS heap to spare, house style leans on"
    echo "long WHY-focused comments -- match whatever comment style the"
    echo "surrounding context already uses, don't invent a terser one)."
    echo ""
    echo "File: $file"
    echo ""
    echo "--- CONTEXT BEFORE ---"
    cat "$hunkdir/hunk-$n.before"
    echo ""
    echo "--- OUR SIDE ($ORIG_BRANCH) ---"
    cat "$hunkdir/hunk-$n.ours"
    echo ""
    printf '%s' "$base_section"
    echo "--- THEIR SIDE ($TARGET) ---"
    cat "$hunkdir/hunk-$n.theirs"
    echo ""
    echo "--- CONTEXT AFTER ---"
    cat "$hunkdir/hunk-$n.after"
    echo ""
    if [ -n "$extra_reason" ]; then
      echo "A previous attempt at this exact hunk was rejected on review for:"
      echo "\"$extra_reason\""
      echo "Fix that specific problem this time."
      echo ""
    fi
    echo "Produce the correct merged replacement for ONLY the conflicting"
    echo "region (do not repeat the context before/after)."
    echo ""
    echo "For CODE (declarations, statements, struct entries): keep the real"
    echo "intent of BOTH sides wherever they are independent additions that"
    echo "do not truly collide -- two unrelated new fields/functions should"
    echo "usually both survive. Where they genuinely conflict (e.g. the same"
    echo "line changed two different ways), keep whichever is correct given"
    echo "the surrounding code."
    echo ""
    echo "For COMMENTS/PROSE explaining the same fact or decision: do NOT"
    echo "concatenate both sides' explanations into one merged paragraph --"
    echo "that produces a redundant, run-on comment nobody would write on"
    echo "purpose. Pick whichever single explanation is more accurate and"
    echo "complete and use ONLY that one, the way a human editor would"
    echo "choose one draft over stitching two together. This applies even"
    echo "when the underlying code line itself does need both sides merged."
    echo ""
    echo "No conflict markers, no markdown code fences, no explanation of"
    echo "your own -- just the replacement code/comment text itself."
    echo ""
    echo "Output ONLY the replacement between these exact marker lines:"
    echo "=== RESOLVED ==="
    echo "<replacement>"
    echo "=== END RESOLVED ==="
  } > "$out"
}

build_review_prompt() {
  local hunkdir="$1" n="$2" file="$3" resolved_file="$4" out="$5"
  local base_section=""
  if [ -s "$hunkdir/hunk-$n.base" ]; then
    base_section="--- COMMON ANCESTOR ---
$(cat "$hunkdir/hunk-$n.base")

"
  fi
  {
    echo "You are reviewing a proposed resolution to a git merge conflict in"
    echo "a C/C++ embedded firmware project. Check that it keeps both"
    echo "sides' real intent (or correctly favors one side where they truly"
    echo "conflict), doesn't duplicate logic, doesn't leave any conflict"
    echo "markers behind, and is syntactically plausible C given the"
    echo "surrounding context."
    echo ""
    echo "REJECT it as INVALID if it merges two competing COMMENT/PROSE"
    echo "explanations of the same fact into one redundant, run-on"
    echo "paragraph instead of picking the single better one -- that is a"
    echo "real, common failure mode, not a style nitpick. A good resolution"
    echo "reads like ONE person wrote it, not two drafts taped together."
    echo ""
    echo "File: $file"
    echo ""
    echo "--- CONTEXT BEFORE ---"
    cat "$hunkdir/hunk-$n.before"
    echo ""
    echo "--- OUR SIDE ($ORIG_BRANCH) ---"
    cat "$hunkdir/hunk-$n.ours"
    echo ""
    printf '%s' "$base_section"
    echo "--- THEIR SIDE ($TARGET) ---"
    cat "$hunkdir/hunk-$n.theirs"
    echo ""
    echo "--- CONTEXT AFTER ---"
    cat "$hunkdir/hunk-$n.after"
    echo ""
    echo "--- PROPOSED RESOLUTION ---"
    cat "$resolved_file"
    echo ""
    echo "Reply with EXACTLY one line: either"
    echo "=== VERDICT ==="
    echo "VALID"
    echo "=== END VERDICT ==="
    echo "or"
    echo "=== VERDICT ==="
    echo "INVALID: <one-sentence reason>"
    echo "=== END VERDICT ==="
  } > "$out"
}

echo "============================================================"
echo " Local conflict resolution -- $ORIG_BRANCH vs $TARGET"
echo " Fixer:    $FIXER_MODEL"
echo " Reviewer: $REVIEW_MODEL"
echo " No cloud calls."
echo "============================================================"
echo ""

MERGE_ALREADY_RUNNING=0
if git rev-parse -q --verify MERGE_HEAD >/dev/null 2>&1; then
  MERGE_ALREADY_RUNNING=1
  echo "A merge is already in progress -- resolving what's already conflicted."
else
  echo "Merging $TARGET (diff3 style, so hunks carry the common ancestor)..."
  set +e
  git -c merge.conflictstyle=diff3 merge --no-commit --no-ff "$TARGET"
  MERGE_RC=$?
  set -e
  if [ "$MERGE_RC" != "0" ] && [ "$MERGE_RC" != "1" ]; then
    echo "git merge failed for a reason other than conflicts (exit $MERGE_RC)." >&2
    exit "$MERGE_RC"
  fi
fi

mapfile -t CONFLICTED < <(git diff --name-only --diff-filter=U)

if [ "${#CONFLICTED[@]}" -eq 0 ]; then
  if git diff --cached --quiet; then
    echo "Nothing to merge and nothing staged -- already up to date."
    exit 0
  fi
  echo "No conflicts -- merge applied cleanly. Skipping straight to the test gate."
else
  echo "${#CONFLICTED[@]} file(s) with conflicts:"
  printf '  %s\n' "${CONFLICTED[@]}"
  echo ""
fi

UNRESOLVED_FILES=()
RESOLVED_FILES=()

for file in "${CONFLICTED[@]:-}"; do
  [ -z "$file" ] && continue
  echo "--- $file ---"
  HUNKDIR="$TMPDIR/$(echo "$file" | tr '/' '_')"
  mkdir -p "$HUNKDIR"
  node "$TMPDIR/parse-conflicts.js" extract "$file" "$HUNKDIR" "$CONTEXT"
  COUNT="$(cat "$HUNKDIR/hunk-count")"
  echo "  $COUNT hunk(s)"

  FILE_FULLY_RESOLVED=1
  for i in $(seq 1 "$COUNT"); do
    N=$(printf '%04d' "$i")
    LOG="$HUNKDIR/hunk-$N.log"
    REASON=""
    VALID=0
    for round in $(seq 1 "$MAX_ROUNDS"); do
      build_hunk_prompt "$HUNKDIR" "$N" "$file" "$HUNKDIR/hunk-$N.prompt" "$REASON"
      if ! chat_call "$HUNKDIR/hunk-$N.prompt" "$HUNKDIR/hunk-$N.raw" "$FIXER_MODEL" "$LOG"; then
        echo "    hunk $i: fixer call failed (round $round) -- see $LOG" >&2
        continue
      fi
      extract_marked "$HUNKDIR/hunk-$N.raw" "=== RESOLVED ===" "=== END RESOLVED ===" \
        > "$HUNKDIR/hunk-$N.candidate"
      if [ ! -s "$HUNKDIR/hunk-$N.candidate" ]; then
        echo "    hunk $i: fixer produced no parseable resolution (round $round)" >&2
        REASON="Your previous reply did not contain a === RESOLVED === / === END RESOLVED === block with real content."
        continue
      fi

      build_review_prompt "$HUNKDIR" "$N" "$file" "$HUNKDIR/hunk-$N.candidate" "$HUNKDIR/hunk-$N.review_prompt"
      if ! chat_call "$HUNKDIR/hunk-$N.review_prompt" "$HUNKDIR/hunk-$N.review_raw" "$REVIEW_MODEL" "$LOG"; then
        echo "    hunk $i: reviewer call failed (round $round) -- see $LOG" >&2
        continue
      fi
      VERDICT="$(extract_marked "$HUNKDIR/hunk-$N.review_raw" "=== VERDICT ===" "=== END VERDICT ===" \
                 | tr -d '\r' | sed '/^$/d' | head -n1)"

      case "$VERDICT" in
        VALID)
          cp "$HUNKDIR/hunk-$N.candidate" "$HUNKDIR/hunk-$N.resolved"
          echo "    hunk $i: resolved (round $round)"
          VALID=1
          break
          ;;
        INVALID:*)
          REASON="${VERDICT#INVALID: }"
          echo "    hunk $i: reviewer rejected (round $round): $REASON"
          ;;
        *)
          REASON="Your reply must be exactly VALID or INVALID: <reason>, inside the === VERDICT === markers. Got: $VERDICT"
          echo "    hunk $i: reviewer gave an unparseable verdict (round $round): $VERDICT"
          ;;
      esac
    done
    if [ "$VALID" != "1" ]; then
      echo "    hunk $i: NOT resolved after $MAX_ROUNDS round(s) -- left conflicted."
      FILE_FULLY_RESOLVED=0
    fi
  done

  node "$TMPDIR/parse-conflicts.js" apply "$file" "$HUNKDIR"

  if [ "$FILE_FULLY_RESOLVED" = "1" ]; then
    git add "$file"
    RESOLVED_FILES+=("$file")
    HAS_CHANGES=1
    echo "  all hunks resolved and staged."
  else
    UNRESOLVED_FILES+=("$file")
    echo "  left conflicted -- needs a human (or a re-run of this script)."
  fi
  echo ""
done

if [ "${#UNRESOLVED_FILES[@]}" -gt 0 ]; then
  echo "============================================================"
  echo " ${#UNRESOLVED_FILES[@]} file(s) still conflicted -- stopping here."
  printf '  %s\n' "${UNRESOLVED_FILES[@]}"
  echo ""
  echo " Resolved files are staged; unresolved ones still show real"
  echo " conflict markers. Fix the rest by hand, or re-run this script"
  echo " (it only re-does what's still marked)."
  if [ -n "$WORKTREE_DIR" ]; then
    echo " Worktree left at: $WORKTREE_DIR"
  fi
  echo "============================================================"
  exit 1
fi

if [ "${#RESOLVED_FILES[@]}" -gt 0 ]; then
  echo "Formatting resolved files with check-format.sh..."
  C_FILES=()
  for f in "${RESOLVED_FILES[@]}"; do
    case "$f" in *.c|*.h) C_FILES+=("$f") ;; esac
  done
  if [ "${#C_FILES[@]}" -gt 0 ]; then
    if scripts/check-format.sh "${C_FILES[@]}"; then
      git add "${C_FILES[@]}"
    else
      echo "  check-format.sh failed (clang-format missing/too old?) -- committing unformatted." >&2
    fi
  fi
fi

echo ""
echo "Running the test gate..."
GATE_OK=1
if ! ./launcher/test/run_tests.sh; then
  GATE_OK=0
fi
if [ "$GATE_OK" = "1" ] && ! ./launcher/test/check_app_sources.sh; then
  GATE_OK=0
fi
# run_tests.sh/check_app_sources.sh alone missed a real regression once
# already: a resolved dump_reactions.c conflict can compile clean and pass
# every host test while still leaving docs/Sand/Reaction-Table.md stale
# relative to what the generator it's paired with would now produce -- the
# exact bug this script's own design doc references. Only one such
# generator/doc-consistency check exists in the repo today; if another app
# grows one (`grep -rl -- --check launcher/main/apps/*/tools/*.sh` finds
# them), add it here the same way.
if [ "$GATE_OK" = "1" ] && [ -x launcher/main/apps/sand/tools/report_reactions.sh ] \
   && ! launcher/main/apps/sand/tools/report_reactions.sh --check; then
  GATE_OK=0
fi

if [ "$GATE_OK" != "1" ]; then
  echo "============================================================"
  echo " Test gate FAILED. The resolution is staged but NOT committed."
  echo " Inspect it with: git diff --cached"
  echo " Then either fix it and commit yourself, or \`git merge --abort\`"
  echo " to bail out of the merge entirely."
  if [ -n "$WORKTREE_DIR" ]; then
    echo " Worktree left at: $WORKTREE_DIR"
  fi
  echo "============================================================"
  HAS_CHANGES=1
  exit 1
fi

echo "Test gate passed. Committing..."
git commit --no-edit

if [ "$NO_PUSH" = "1" ]; then
  echo "--no-push set -- commit made, not pushed."
  if [ -n "$WORKTREE_DIR" ]; then
    echo "Push it yourself when ready:"
    echo "  cd \"$WORKTREE_DIR\" && git push origin HEAD:$ORIG_BRANCH"
  else
    echo "Push it yourself when ready: git push"
  fi
  exit 0
fi

if [ -n "$WORKTREE_DIR" ]; then
  # See docs/Model-Delegation-Workflow.md's "Git mechanics specific to
  # worktrees": this worktree's branch is a throwaway temp name, but
  # pushing straight to the remote ref of the branch actually checked out
  # elsewhere is unaffected by the "can't update a ref checked out in
  # another worktree" restriction -- only a LOCAL ref update would hit that.
  git push origin "HEAD:$ORIG_BRANCH"
  echo ""
  echo "Pushed to origin/$ORIG_BRANCH. Your other checkout of $ORIG_BRANCH is"
  echo "now behind -- fast-forward it yourself:"
  echo "  git pull --ff-only"
else
  git push
fi

echo "Done."
