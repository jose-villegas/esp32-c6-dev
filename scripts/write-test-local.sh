#!/usr/bin/env bash
# Delegate writing ONE Unity test function body to a local Ollama model,
# from an exact spec YOU write, then verify it for real before leaving it
# in the tree. Zero cloud calls -- same conventions as scripts/resolve-
# conflicts-local.sh (--think=false, stderr to a log not merged into
# stdout, a reviewer from a different model family as a second opinion).
#
# DIVISION OF LABOR (see docs/Model-Delegation-Workflow.md)
#
# This script does NOT decide what to test. You write the spec: the exact
# scene-setup C statements (which fixture/helper calls, in what order) and
# the exact assertions (full TEST_ASSERT_*_MESSAGE calls, not a vague
# description) -- that is the judgment part, and it stays yours. The
# model's only job is turning that into a correctly-formatted
# `static void test_<name>(void) { ... }` matching the target suite's own
# house style, plus a WHY-comment above it built from the CONTEXT you give
# it. It is told, explicitly, not to invent, drop, or reorder a single
# assertion or scene statement -- and the reviewer checks that it didn't.
#
# WHY THIS SPLIT AND NOT "WRITE ME A TEST FOR X"
#
# A model deciding what to assert is deciding what correctness means here,
# and that is exactly the kind of judgment call resolve-conflicts-local.sh
# found local models unreliable at (see its own header comment on the
# prose-splicing failure it caught and fixed). Handing over only the
# typing -- given an exact scene and exact assertions -- keeps this in the
# bucket local models actually are good at.
#
# THE VERIFICATION GATE
#
# 1. The target suite must not already have a test of this name.
# 2. ./launcher/test/run_tests.sh must still pass, in full, afterward.
# 3. The new test must show up in that output as PASS EXACTLY ONCE -- zero
#    times means the RUN_TEST wiring didn't take; more than once means
#    something duplicated. Either way this is CLAUDE.md's own "watch it
#    fail before it passes" turned into an automatic check on the wiring,
#    not the logic.
# 4. --regression-commit <SHA>, if given, goes one step further and
#    actually proves the test can fail: it inserts the SAME generated test
#    into the tree as it stood at <SHA>^ (the commit BEFORE whatever fixed
#    the bug this test is meant to guard), in an isolated worktree, and
#    confirms it does NOT pass there. A test that already passes on the
#    pre-fix code cannot be guarding anything -- see CLAUDE.md's Testing
#    section on exactly this failure mode. This step is a strong warning,
#    not a hard abort: it tells you the test may not test what you think,
#    but the decision to keep or rewrite it is still yours.
#
# Any gate failure rolls the suite file back to what it was before this
# script touched it -- nothing broken is ever left in the tree.
#
# SPEC FILE FORMAT
#
#   SUITE: launcher/main/apps/sand/suite_sand.c
#   NAME: water_evaporates_when_forced
#
#   === CONTEXT ===
#   Free text: what this test guards against, why it matters. Becomes the
#   substance of the leading comment, rewritten into the target file's own
#   comment style -- not copied verbatim if that style is denser or
#   terser than what you wrote here.
#   === END CONTEXT ===
#
#   === SCENE ===
#   sand_t s;
#   uint8_t cells[W * H];
#   sand_init(&s, cells, W, H, 777u);
#   sand_set(&s, 2, 2, CELL_MAKE(MAT_WATER, MASS_MAX));
#   sand_set_evaporates(&s, 255);
#   sand_step(&s, 16, ...);
#   === END SCENE ===
#
#   === ASSERTIONS ===
#   TEST_ASSERT_EQUAL_UINT8_MESSAGE(MAT_GAS, CELL_MATERIAL(sand_at(&s, 2, 2)),
#       "acid forced to evaporate must turn into gas within one step");
#   === END ASSERTIONS ===
#
# SCENE and ASSERTIONS are copied into the generated function VERBATIM --
# they are not prose for the model to interpret, they are the actual C
# statements, written by you (or delegated as their own atomic edits the
# usual way, per Model-Delegation-Workflow.md step 3 -- this script is the
# last, mechanical step, not a replacement for writing the spec).
#
# Usage:
#   scripts/write-test-local.sh --suite <file> --spec <file>
#       [--regression-commit <SHA>] [--rounds N] [--worktree]
#
# --worktree runs the whole thing in a fresh sibling worktree instead of
# your current checkout (same convention as resolve-conflicts-local.sh),
# left behind afterward either way for you to inspect and commit yourself
# -- this script never commits or pushes anything on its own; adding one
# test is a small enough step to stay a normal part of whatever you were
# already doing, not a branch of its own.
#
# Env overrides (distinct names from the other two local scripts on
# purpose, so setting one never silently changes another):
#   TESTWRITE_FIXER_MODEL   default qwen2.5:14b
#   TESTWRITE_REVIEW_MODEL  default mistral-nemo:latest
# Both chosen so their weights alone (~9GB / ~7GB) fit a 16GB card; bigger
# options like qwen3-coder:30b or gemma4:26b (~18GB each) remain available
# via the env vars above for anyone with real VRAM headroom. If local
# Ollama runs are freezing the machine regardless of model choice, see
# docs/Model-Delegation-Workflow.md's "A global Ollama setting can make
# picking a 'small enough' model pointless" -- a stuck 262144 Context
# Length setting in the Ollama app itself overrides every model's context
# and is the far more likely culprit; one test spec plus a suite file never
# needs anywhere near that once the app setting is sane.
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

SUITE=""
SPEC=""
REGRESSION_COMMIT=""
MAX_ROUNDS=2
USE_WORKTREE=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --suite) SUITE="$2"; shift 2 ;;
    --spec) SPEC="$2"; shift 2 ;;
    --regression-commit) REGRESSION_COMMIT="$2"; shift 2 ;;
    --rounds) MAX_ROUNDS="$2"; shift 2 ;;
    --worktree) USE_WORKTREE=1; shift ;;
    -h|--help)
      sed -n '2,88p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

if [ -z "$SUITE" ] || [ -z "$SPEC" ]; then
  echo "Usage: $0 --suite <file> --spec <file> [--regression-commit <SHA>] [--rounds N] [--worktree]" >&2
  exit 1
fi
[ -f "$SPEC" ] || { echo "Spec file not found: $SPEC" >&2; exit 1; }

FIXER_MODEL="${TESTWRITE_FIXER_MODEL:-qwen2.5:14b}"
REVIEW_MODEL="${TESTWRITE_REVIEW_MODEL:-mistral-nemo:latest}"

for m in "$FIXER_MODEL" "$REVIEW_MODEL"; do
  if ! ollama list | awk '{print $1}' | grep -qxF "$m"; then
    echo "Model '$m' is not in \`ollama list\`. Pull it first, or override" >&2
    echo "via TESTWRITE_FIXER_MODEL / TESTWRITE_REVIEW_MODEL env vars." >&2
    echo "Currently pulled:" >&2
    ollama list >&2
    exit 1
  fi
done

ORIG_BRANCH="$(git symbolic-ref --short -q HEAD || true)"

# --- parse the spec file --------------------------------------------------
extract_marked() {
  awk -v s="$2" -v e="$3" '
    $0 == s { grabbing = 1; next }
    $0 == e { grabbing = 0 }
    grabbing { print }
  ' "$1"
}

SPEC_SUITE="$(awk -F': ' '/^SUITE:/{print $2; exit}' "$SPEC")"
NAME="$(awk -F': ' '/^NAME:/{print $2; exit}' "$SPEC")"
if [ -z "$NAME" ]; then
  echo "Spec is missing a 'NAME: <test_name>' line." >&2
  exit 1
fi
if [ -n "$SPEC_SUITE" ] && [ "$SPEC_SUITE" != "$SUITE" ]; then
  echo "Spec says SUITE: $SPEC_SUITE but --suite was $SUITE -- pick one." >&2
  exit 1
fi
[ -f "$SUITE" ] || { echo "Suite file not found: $SUITE" >&2; exit 1; }

TMPDIR=$(mktemp -d)
WORKTREE_DIR=""
trap 'rm -rf "$TMPDIR"; [ -n "$WORKTREE_DIR" ] && git -C "$REPO_ROOT" worktree remove "$WORKTREE_DIR" --force 2>/dev/null || true' EXIT

extract_marked "$SPEC" "=== CONTEXT ===" "=== END CONTEXT ===" > "$TMPDIR/context"
extract_marked "$SPEC" "=== SCENE ===" "=== END SCENE ===" > "$TMPDIR/scene"
extract_marked "$SPEC" "=== ASSERTIONS ===" "=== END ASSERTIONS ===" > "$TMPDIR/assertions"
if [ ! -s "$TMPDIR/scene" ] || [ ! -s "$TMPDIR/assertions" ]; then
  echo "Spec must have non-empty === SCENE === and === ASSERTIONS === blocks." >&2
  exit 1
fi

if [ "$USE_WORKTREE" = "1" ]; then
  TMP_BRANCH="write-test-$(date +%Y%m%d-%H%M%S)"
  WORKTREE_DIR="$(dirname "$REPO_ROOT")/$(basename "$REPO_ROOT")-$TMP_BRANCH"
  echo "Creating worktree at $WORKTREE_DIR..."
  git worktree add -b "$TMP_BRANCH" "$WORKTREE_DIR" "${ORIG_BRANCH:-HEAD}" >&2
  cd "$WORKTREE_DIR"
fi

if grep -q "static void test_$NAME" "$SUITE"; then
  echo "$SUITE already has a test named test_$NAME -- pick another name." >&2
  exit 1
fi

# --- test-writer.js: find style examples, insert the generated function --
# and its RUN_TEST call. Brace matching runs over a "masked" copy of the
# file (comments/strings/char-literals blanked out, length preserved) so a
# stray '{' inside one of this codebase's own dense WHY-comments (common
# here) can never be mistaken for real code structure.
cat > "$TMPDIR/test-writer.js" << "JSEOF"
const fs = require("fs");

function mask(text) {
  let out = [];
  let i = 0;
  const n = text.length;
  let state = "CODE";
  while (i < n) {
    const c = text[i], c2 = text[i + 1];
    if (state === "CODE") {
      if (c === "/" && c2 === "/") { state = "LCOM"; out.push(" ", " "); i += 2; continue; }
      if (c === "/" && c2 === "*") { state = "BCOM"; out.push(" ", " "); i += 2; continue; }
      if (c === "\"") { state = "STR"; out.push(" "); i += 1; continue; }
      if (c === "'") { state = "CHR"; out.push(" "); i += 1; continue; }
      out.push(c); i += 1; continue;
    }
    if (state === "LCOM") {
      if (c === "\n") { state = "CODE"; out.push("\n"); i += 1; continue; }
      out.push(" "); i += 1; continue;
    }
    if (state === "BCOM") {
      if (c === "*" && c2 === "/") { state = "CODE"; out.push(" ", " "); i += 2; continue; }
      out.push(c === "\n" ? "\n" : " "); i += 1; continue;
    }
    if (state === "STR") {
      if (c === "\\") { out.push(" ", " "); i += 2; continue; }
      if (c === "\"") { state = "CODE"; out.push(" "); i += 1; continue; }
      out.push(" "); i += 1; continue;
    }
    if (state === "CHR") {
      if (c === "\\") { out.push(" ", " "); i += 2; continue; }
      if (c === "'") { state = "CODE"; out.push(" "); i += 1; continue; }
      out.push(" "); i += 1; continue;
    }
  }
  return out.join("");
}

function netBraces(maskedLine) {
  let n = 0;
  for (const c of maskedLine) { if (c === "{") n++; else if (c === "}") n--; }
  return n;
}

// findFunctionSpan: given lines[]/maskedLines[] and the index of a line
// that DECLARES a function (name + args, brace on a following line, this
// codebase's own style), returns [declLineIdx, closeBraceLineIdx].
function findFunctionSpan(lines, maskedLines, declLineIdx) {
  let openLineIdx = -1;
  for (let i = declLineIdx; i < lines.length; i++) {
    if (maskedLines[i].includes("{")) { openLineIdx = i; break; }
  }
  if (openLineIdx === -1) throw new Error(`no opening brace found after line ${declLineIdx}`);
  let depth = 0;
  for (let i = openLineIdx; i < lines.length; i++) {
    depth += netBraces(maskedLines[i]);
    if (depth === 0) return [declLineIdx, i];
  }
  throw new Error(`no matching closing brace for function at line ${declLineIdx}`);
}

function mode() { return process.argv[2]; }

if (mode() === "examples") {
  const [, , , file, maxCount] = process.argv;
  const text = fs.readFileSync(file, "utf8");
  const lines = text.split("\n");
  const maskedLines = mask(text).split("\n");
  const declRe = /^static void (test_\w+)\(void\)$/;
  const found = [];
  for (let i = 0; i < lines.length; i++) {
    const m = lines[i].match(declRe);
    if (!m) continue;
    try {
      const [start, end] = findFunctionSpan(lines, maskedLines, i);
      found.push({ name: m[1], lineCount: end - start + 1, text: lines.slice(start, end + 1).join("\n") });
    } catch (e) { /* skip anything this simple scanner can't span */ }
  }
  found.sort((a, b) => a.lineCount - b.lineCount);
  const usable = found.filter(f => f.lineCount >= 3);
  const pick = (usable.length ? usable : found).slice(0, parseInt(maxCount, 10));
  for (const f of pick) {
    console.log(`--- example: ${f.name} ---`);
    console.log(f.text);
    console.log("");
  }
  process.exit(0);
}

if (mode() === "insert") {
  const [, , , file, name, bodyFile] = process.argv;
  const newFn = fs.readFileSync(bodyFile, "utf8").replace(/\s+$/, "");
  const text = fs.readFileSync(file, "utf8");
  const lines = text.split("\n");
  const maskedLines = mask(text).split("\n");

  const regMatch = text.match(/SUITE_REGISTER\(\s*(\w+)\s*\)/);
  if (!regMatch) {
    console.error(`No SUITE_REGISTER(...) found in ${file} -- this script only knows how to`);
    console.error("insert into an app-owned suite with its own run_<name>_suite function, not");
    console.error("a shell suite whose RUN_TEST calls live directly in host_main.c.");
    process.exit(1);
  }
  const runFn = regMatch[1];
  const declRe = new RegExp(`^(?:static )?void ${runFn}\\(void\\)$`);
  let declLineIdx = -1;
  for (let i = 0; i < lines.length; i++) {
    if (declRe.test(lines[i])) { declLineIdx = i; break; }
  }
  if (declLineIdx === -1) {
    console.error(`Could not find the declaration of ${runFn}(void) in ${file}.`);
    process.exit(1);
  }
  const [start, end] = findFunctionSpan(lines, maskedLines, declLineIdx);

  // Where to insert the new RUN_TEST(...) line: right after the LAST
  // UNCONDITIONAL existing one inside this function (ppDepth === 0),
  // copying its indentation. This codebase guards hardware-only tests with
  // #ifdef DEVICE_BUILD around their own RUN_TEST line (see CLAUDE.md's
  // Testing section) - the naive "last RUN_TEST anywhere in the function"
  // found live landed a portable test's wiring INSIDE that guard, where a
  // host build silently compiles it back out ("defined but not used"
  // becomes the only symptom, not a test failure). #else/#elif don't
  // change nesting depth, only #if/#ifdef/#ifndef and #endif do - good
  // enough for this file's actual usage, which never nests them. Falls
  // back to 4-space indent right after the opening brace if the suite has
  // no unconditional RUN_TEST calls at all (an edge case, not the normal
  // shape).
  let lastRunTestLine = -1, indent = "    ", ppDepth = 0;
  for (let i = start; i <= end; i++) {
    const trimmed = lines[i].trim();
    if (/^#\s*(if|ifdef|ifndef)\b/.test(trimmed)) { ppDepth++; continue; }
    if (/^#\s*endif\b/.test(trimmed)) { ppDepth--; continue; }
    if (ppDepth > 0) continue;
    const m = lines[i].match(/^(\s*)RUN_TEST\(/);
    if (m) { lastRunTestLine = i; indent = m[1]; }
  }
  const insertRunTestAfter = lastRunTestLine !== -1 ? lastRunTestLine : start + 1;
  const runTestLine = `${indent}RUN_TEST(${name});`;

  // Built as flat line arrays (not one multi-line string per chunk) so the
  // exact 1-based line RANGES of what got inserted can be reported back --
  // needed so the caller can format ONLY those lines (clang-format
  // --lines=N:M) rather than the whole file. This codebase deliberately
  // does not match .clang-format throughout (see CLAUDE.md's formatting
  // section: "do not reformat pre-existing files, .clang-format disagrees
  // with the current style in places") - confirmed live the naive
  // whole-file `check-format.sh` rewrote all ~18000 lines of a real suite
  // file's brace/pointer/alignment style for the sake of formatting one
  // new function, which is exactly the mistake that rule exists to
  // prevent.
  const newFnLines = newFn.split("\n");
  const prefix = lines.slice(0, declLineIdx);
  const middle = lines.slice(declLineIdx, insertRunTestAfter + 1);
  const suffix = lines.slice(insertRunTestAfter + 1);

  const funcStartLine = prefix.length + 1;             // 1-based
  const funcEndLine = funcStartLine + newFnLines.length - 1;
  const runTestLineNo = funcEndLine + 1 /* blank line */ + middle.length + 1;

  const out = [...prefix, ...newFnLines, "", ...middle, runTestLine, ...suffix];
  fs.writeFileSync(file, out.join("\n"));
  console.log(`FUNC_RANGE:${funcStartLine}:${funcEndLine}`);
  console.log(`RUNTEST_RANGE:${runTestLineNo}:${runTestLineNo}`);
  process.exit(0);
}

console.error(`Unknown mode '${mode()}' (expected examples|insert)`);
process.exit(1);
JSEOF

# chat_call PROMPT_FILE OUT_FILE MODEL_ID LOG_FILE
# --think=false, stderr routed to a log (never merged into stdout), and
# --nowordwrap are all load-bearing -- see resolve-conflicts-local.sh's own
# chat_call comment for what each one prevents. --nowordwrap in particular
# was found live while building THIS script: qwen3-coder:30b (not one of
# the models tested when --think=false/stderr-routing were first
# documented) still wrote raw ANSI redraw bytes into a long assertion
# message on stdout despite both of those already being in place.
chat_call() {
  local prompt_file="$1" out_file="$2" model_id="$3" log_file="$4"
  ollama run "$model_id" --think=false --nowordwrap < "$prompt_file" > "$out_file" 2>>"$log_file"
}

node "$TMPDIR/test-writer.js" examples "$SUITE" 2 > "$TMPDIR/examples.txt" || true

build_fixer_prompt() {
  local out="$1" extra_reason="$2"
  {
    echo "You are writing ONE Unity test function for a C embedded firmware"
    echo "project's host test suite. Match the house style of the EXAMPLE"
    echo "test(s) below exactly -- brace placement, indentation, whether (and"
    echo "how densely) comments are used above a test, how TEST_ASSERT calls"
    echo "are wrapped across lines. Do not invent a different style."
    echo ""
    if [ -s "$TMPDIR/examples.txt" ]; then
      echo "$(cat "$TMPDIR/examples.txt")"
    fi
    echo "--- YOUR TEST'S NAME ---"
    echo "test_$NAME"
    echo ""
    echo "--- CONTEXT (what this test guards against -- turn this into a"
    echo "leading comment above the function, in the SAME style/density as"
    echo "the example(s) above, not copied verbatim if that reads differently"
    echo "from how this file actually writes comments) ---"
    cat "$TMPDIR/context"
    echo ""
    echo "--- SCENE (copy these statements into the function body EXACTLY,"
    echo "verbatim, in this order -- do not paraphrase, reorder, add to, or"
    echo "drop any of them) ---"
    cat "$TMPDIR/scene"
    echo ""
    echo "--- ASSERTIONS (copy these EXACTLY, verbatim -- same macro, same"
    echo "arguments, same message string. Do not invent additional"
    echo "assertions and do not drop any of these) ---"
    cat "$TMPDIR/assertions"
    echo ""
    if [ -n "$extra_reason" ]; then
      echo "A previous attempt was rejected on review for:"
      echo "\"$extra_reason\""
      echo "Fix that specific problem this time."
      echo ""
    fi
    echo "Output the COMPLETE function, from any leading comment through the"
    echo "closing brace, and nothing else -- no markdown fences, no"
    echo "explanation -- between these exact marker lines:"
    echo "=== TEST ==="
    echo "<function>"
    echo "=== END TEST ==="
  } > "$out"
}

build_review_prompt() {
  local candidate="$1" out="$2"
  {
    echo "You are reviewing a generated Unity test function against the"
    echo "exact spec it was supposed to render. REJECT it as INVALID if it:"
    echo "- dropped, reordered, paraphrased, or added to any SCENE statement"
    echo "- dropped, changed, or added any ASSERTIONS (macro, args, or"
    echo "  message string must match exactly)"
    echo "- invented an assertion that was not in the spec"
    echo "- doesn't compile as plausible C given the style shown -- including"
    echo "  any stray line before or after the function (e.g. the bare"
    echo "  function name repeated as its own line, a title, a markdown"
    echo "  artifact) that is not part of a real leading comment or the"
    echo "  function itself"
    echo "Otherwise reply VALID. Minor comment wording is fine as long as it"
    echo "accurately reflects the given CONTEXT -- do not reject over prose"
    echo "style alone."
    echo ""
    echo "--- SCENE (must appear verbatim) ---"
    cat "$TMPDIR/scene"
    echo ""
    echo "--- ASSERTIONS (must appear verbatim) ---"
    cat "$TMPDIR/assertions"
    echo ""
    echo "--- CONTEXT ---"
    cat "$TMPDIR/context"
    echo ""
    echo "--- GENERATED FUNCTION ---"
    cat "$candidate"
    echo ""
    echo "Reply with EXACTLY one line between these markers: either"
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
echo " Writing test_$NAME into $SUITE"
echo " Fixer:    $FIXER_MODEL"
echo " Reviewer: $REVIEW_MODEL"
echo " No cloud calls."
echo "============================================================"

REASON=""
APPROVED=0
for round in $(seq 1 "$MAX_ROUNDS"); do
  build_fixer_prompt "$TMPDIR/fixer_prompt" "$REASON"
  if ! chat_call "$TMPDIR/fixer_prompt" "$TMPDIR/fixer_raw" "$FIXER_MODEL" "$TMPDIR/log"; then
    echo "fixer call failed (round $round) -- see $TMPDIR/log" >&2
    continue
  fi
  extract_marked "$TMPDIR/fixer_raw" "=== TEST ===" "=== END TEST ===" > "$TMPDIR/candidate.raw"
  # qwen3-coder:30b was seen live writing the bare function name as its own
  # line before the real declaration ("test_foo\nstatic void test_foo(void)
  # {...") -- a title-style artifact, not a formatting choice worth asking
  # the model to fix itself, since a mechanical strip of an exact-match
  # leading line is a guaranteed fix where re-prompting would only reduce
  # frequency. Only strips a line that is EXACTLY "test_$NAME" with nothing
  # else on it, so a legitimate leading `/* comment */` block is untouched.
  awk -v n="test_$NAME" 'NR==1 && $0==n{next} {print}' "$TMPDIR/candidate.raw" > "$TMPDIR/candidate"
  if [ ! -s "$TMPDIR/candidate" ]; then
    echo "round $round: fixer produced no parseable function"
    REASON="Your previous reply did not contain a === TEST === / === END TEST === block with real content."
    continue
  fi

  build_review_prompt "$TMPDIR/candidate" "$TMPDIR/review_prompt"
  if ! chat_call "$TMPDIR/review_prompt" "$TMPDIR/review_raw" "$REVIEW_MODEL" "$TMPDIR/log"; then
    echo "reviewer call failed (round $round) -- see $TMPDIR/log" >&2
    continue
  fi
  VERDICT="$(extract_marked "$TMPDIR/review_raw" "=== VERDICT ===" "=== END VERDICT ===" \
             | tr -d '\r' | sed '/^$/d' | head -n1)"
  case "$VERDICT" in
    VALID)
      echo "round $round: approved"
      APPROVED=1
      break
      ;;
    INVALID:*)
      REASON="${VERDICT#INVALID: }"
      echo "round $round: rejected: $REASON"
      ;;
    *)
      REASON="Your reply must be exactly VALID or INVALID: <reason>, inside the === VERDICT === markers. Got: $VERDICT"
      echo "round $round: unparseable verdict: $VERDICT"
      ;;
  esac
done

if [ "$APPROVED" != "1" ]; then
  echo "============================================================"
  echo " Not resolved after $MAX_ROUNDS round(s). Nothing was changed."
  if [ -n "$WORKTREE_DIR" ]; then
    echo " Worktree left at: $WORKTREE_DIR"
  fi
  echo "============================================================"
  exit 1
fi

cp "$SUITE" "$TMPDIR/suite.backup"
INSERT_OUT="$(node "$TMPDIR/test-writer.js" insert "$SUITE" "test_$NAME" "$TMPDIR/candidate")"
FUNC_RANGE="$(echo "$INSERT_OUT" | awk -F: '/^FUNC_RANGE:/{print $2":"$3}')"
RUNTEST_RANGE="$(echo "$INSERT_OUT" | awk -F: '/^RUNTEST_RANGE:/{print $2":"$3}')"

# Format ONLY the lines this script just inserted (clang-format --lines=N:M,
# repeatable for the two separate, non-adjacent ranges: the new function
# and its RUN_TEST wiring). Never the whole file -- see the "Built as flat
# line arrays" comment in test-writer.js's insert mode for why a naive
# whole-file `scripts/check-format.sh` is the wrong tool here: this
# codebase deliberately does not match .clang-format throughout, and a
# full-file format was confirmed live to rewrite ~18000 unrelated lines of
# real house style for the sake of one new function.
CLANG_FORMAT="clang-format"
if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
  esp_clang="$(ls -d "$HOME"/.espressif/tools/esp-clang/*/esp-clang/bin/clang-format* 2>/dev/null | sort -V | tail -n1)"
  [ -n "$esp_clang" ] && CLANG_FORMAT="$esp_clang"
fi
if command -v "$CLANG_FORMAT" >/dev/null 2>&1 && [ -n "$FUNC_RANGE" ] && [ -n "$RUNTEST_RANGE" ]; then
  "$CLANG_FORMAT" -i "--lines=$FUNC_RANGE" "--lines=$RUNTEST_RANGE" "$SUITE" \
    && echo "Formatted just the new lines ($FUNC_RANGE, $RUNTEST_RANGE) with clang-format." \
    || echo "  clang-format --lines failed -- left unformatted." >&2
else
  echo "  clang-format not found or ranges unparsed -- left unformatted." >&2
fi

echo ""
echo "Running the test gate..."
RUN_OUT="$TMPDIR/run_tests.out"
GATE_OK=1
if ! ./launcher/test/run_tests.sh > "$RUN_OUT" 2>&1; then
  GATE_OK=0
fi
cat "$RUN_OUT"
PASS_COUNT="$(grep -c ":test_${NAME}:PASS$" "$RUN_OUT" || true)"
if [ "$PASS_COUNT" != "1" ]; then
  echo "Expected exactly 1 PASS line for test_$NAME, found $PASS_COUNT." >&2
  GATE_OK=0
fi
if [ "$GATE_OK" = "1" ] && [ -x launcher/main/apps/sand/tools/report_reactions.sh ] \
   && ! launcher/main/apps/sand/tools/report_reactions.sh --check > /dev/null 2>&1; then
  echo "report_reactions.sh --check went stale -- inserting this test somehow" >&2
  echo "touched generated-doc-relevant content. Rolling back." >&2
  GATE_OK=0
fi

if [ "$GATE_OK" != "1" ]; then
  cp "$TMPDIR/suite.backup" "$SUITE"
  echo "============================================================"
  echo " Test gate FAILED. $SUITE was rolled back -- nothing was left broken."
  if [ -n "$WORKTREE_DIR" ]; then
    echo " Worktree left at: $WORKTREE_DIR"
  fi
  echo "============================================================"
  exit 1
fi

if [ -n "$REGRESSION_COMMIT" ]; then
  echo ""
  echo "Checking this test actually fails on the pre-fix code ($REGRESSION_COMMIT^)..."
  PROOF_BRANCH="write-test-proof-$(date +%Y%m%d-%H%M%S)"
  PROOF_DIR="$(dirname "$REPO_ROOT")/$(basename "$REPO_ROOT")-$PROOF_BRANCH"
  if git worktree add -b "$PROOF_BRANCH" "$PROOF_DIR" "${REGRESSION_COMMIT}^" >&2 2>>"$TMPDIR/log"; then
    # $PROOF_DIR/$SUITE is ALREADY the real pre-fix file, exactly as
    # `git worktree add` checked it out at ${REGRESSION_COMMIT}^ -- it must
    # never be overwritten from the current branch's $SUITE, which by this
    # point in the script already HAS test_$NAME inserted. Doing that once
    # (a real bug, caught by re-running this exact proof twice and getting
    # a different answer each time) fed test-writer.js a file that already
    # contained the test, so "insert" added a SECOND copy -- a duplicate-
    # definition compile error, which the old exit-code-only check then
    # misreported as "confirmed fails as expected". It was neither: the
    # suite never even finished compiling.
    if [ -f "$PROOF_DIR/$SUITE" ]; then
      node "$TMPDIR/test-writer.js" insert "$PROOF_DIR/$SUITE" "test_$NAME" "$TMPDIR/candidate" \
        2>>"$TMPDIR/log" || echo "  could not insert into the pre-fix file (suite shape differs too much)." >&2
      (cd "$PROOF_DIR" && ./launcher/test/run_tests.sh > "$TMPDIR/proof.out" 2>&1) || true
      if grep -q ":test_${NAME}:PASS$" "$TMPDIR/proof.out"; then
        echo "  WARNING: test_$NAME also PASSES on the pre-fix code at" >&2
        echo "  ${REGRESSION_COMMIT}^ -- it may not actually be guarding the" >&2
        echo "  regression you think it is. Review the scene/assertions." >&2
      elif grep -q ":test_${NAME}:FAIL" "$TMPDIR/proof.out"; then
        echo "  Confirmed: test_$NAME fails on the pre-fix code (as expected)."
      else
        echo "  INCONCLUSIVE: test_$NAME's own PASS/FAIL line never appeared in" >&2
        echo "  the pre-fix build's output -- the suite likely failed to compile" >&2
        echo "  there for a reason unrelated to this test. Last 15 lines:" >&2
        tail -15 "$TMPDIR/proof.out" >&2
      fi
    else
      echo "  $SUITE doesn't exist at ${REGRESSION_COMMIT}^ -- skipping the proof." >&2
    fi
    git worktree remove "$PROOF_DIR" --force 2>/dev/null || true
    git branch -D "$PROOF_BRANCH" 2>/dev/null || true
  else
    echo "  Could not create a worktree at ${REGRESSION_COMMIT}^ -- skipping the proof." >&2
  fi
fi

echo ""
echo "============================================================"
echo " test_$NAME added to $SUITE and verified. Not committed --"
echo " review the diff and commit it as part of your normal workflow."
if [ -n "$WORKTREE_DIR" ]; then
  echo " Worktree: $WORKTREE_DIR"
fi
echo "============================================================"
git diff -- "$SUITE" | head -100
