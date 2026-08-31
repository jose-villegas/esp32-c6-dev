#!/usr/bin/env bash
# Runs launcher/tools/misra_check.sh (or reads an existing report with
# --report), groups the findings by file (main/ only -- managed_components/
# is vendor code and never touched), then fixes them the same way
# fix-audited-docs.sh fixes doc findings: each finding becomes a small, exact
# find/replace patch, verified against the real file content before being
# applied -- never a full-file rewrite, never forced.
#
# Two differences from the docs script, both requested for code:
#   1. Bulk parallel: one fixer call per FILE (not per finding, not
#      sequential) via --pool / --combo, up to --parallel at once.
#   2. Mandatory review loop, not an optional filter: --review's model looks
#      at a unified diff of each file's proposed patches and verdicts
#      VALID/INVALID per patch. Any INVALID goes back to the fixer with the
#      reviewer's reason attached and gets a chance to be revised, up to
#      --rounds times, before being dropped.
#
# Once patches are approved and applied, scripts/check-format.sh runs
# in-place on just the changed .c/.h files (not a pre-scan over everything --
# see the comment at that call site) before the commit, so the fixer's
# injected text matches .clang-format rather than whatever the model felt
# like indenting.
#
# --pool picks which combo of models does the bulk fixing (--combo <name>
# overrides it with any combo by name, for one-off experiments):
#   local        (default) local-coding -- this workspace's Ollama models,
#                free and unlimited. MISRA/cppcheck backlogs run into the
#                hundreds of findings (see misra_check.sh's own comment), and
#                firing that many fixer calls at a paid cloud model in
#                parallel is exactly the kind of bulk load local models exist
#                to absorb -- the --review model only ever sees a handful of
#                proposed diffs, not the whole backlog.
#   free         code-fix-free -- zero-marginal-cost API-key providers
#                (ollama-cloud, deepseek, gemini, nvidia free tiers), local
#                as final fallback. Higher quality than local alone, but
#                subject to those providers' free-tier rate limits -- lower
#                --parallel may be needed to avoid 429s, not higher.
#   subscription code-fix-subscription -- Claude/Codex/Copilot/Kimi-coding,
#                covered by a subscription you already pay flat-rate for. No
#                marginal $ per call, but bulk-parallel use does eat into
#                your personal daily usage quota on those services.
#   all          code-fix-all -- union of free + subscription plus a couple
#                more paid API-key providers. Best quality/throughput
#                ceiling, real per-call cost on the non-subscription entries.
# --parallel still defaults conservatively (3): tuned for local's single-GPU
# contention. Reconsider it for free/subscription/all -- see above.
#
# --worktree runs the whole thing in a fresh `git worktree` (sibling
# directory) instead of checking out the new branch in place, so your
# current checkout's HEAD and any uncommitted work never move, and the
# "working tree isn't clean" gate is skipped entirely (nothing here touches
# your real checkout). misra_check.sh needs a compile_commands.json from a
# prior `idf.py build`, which lives in a gitignored build directory a fresh
# worktree won't have of its own -- rather than requiring you to run
# misra_check.sh in your real checkout first (which would mean "isolated"
# still touched it), everything in the main checkout's launcher/<build_dir>
# gets symlinked into the worktree EXCEPT compile_commands.json, which gets
# rewritten instead: its absolute paths point at the main checkout's source
# no matter where cppcheck runs from, so a plain symlink would have cppcheck
# silently analyze (and write its *.dump crash files into) the main
# checkout regardless of the worktree -- the rewritten copy points those
# paths at the worktree's own source instead, so misra_check.sh genuinely
# runs entirely inside the worktree, same as everything else. (--report
# bypasses this: if you already have a report, its path is just read
# directly, worktree or not.) If nothing ends up changing, the worktree and
# its branch are removed automatically; if something does, both are left
# behind for you to inspect before opening
# the PR.
#
# --exclude <prefix> drops any finding whose main/-relative path starts with
# it (repeatable) -- e.g. --exclude main/apps/ to scan everything under
# main/ except the apps. Matched before misra_check.sh's own --file-filter
# ever runs, so it composes with a file_filter of "*/main/*" to express
# exclusions cppcheck's single-glob --file-filter can't (main minus apps).
#
# Use "*/main/*", never a bare "*", for a whole-project file_filter:
# cppcheck's --file-filter controls what gets ANALYZED, not just what gets
# reported -- "*" makes literally everything in compile_commands.json match,
# which on this repo means cppcheck (and its MISRA addon) fully parses all
# ~1800 translation units, ~1785 of them vendored (788 alone are LVGL) and
# none of them ever produced a usable finding, since main/-only filtering
# happens downstream anyway. That cost one stuck run over 12GB of RAM before
# being killed by hand. "*/main/*" scopes cppcheck's own analysis to the ~28
# real translation units instead, which is what was actually wanted.
#
# --no-push commits on the branch (worktree or not) but stops short of
# `git push` -- the branch/worktree is left for you to inspect and push
# yourself when ready.
#
# --local bypasses --pool/--combo/OmniRoute entirely (for both the fixer and
# the reviewer) and calls Ollama directly (`ollama run`) instead, so the
# whole run makes zero network calls to any cloud provider. This is
# deliberately a different path from --pool local's "local-coding" combo:
# that one still goes through OmniRoute's own ollama-local provider, which
# has no working connection pool (every model in it timed out identically at
# 30s on a trivial prompt -- see Model-Delegation-Workflow.md's "Route local
# through the Ollama CLI directly, not OmniRoute"). Fixer defaults to
# qwen2.5:14b, reviewer to gemma4:26b (LOCAL_FIXER_MODEL / LOCAL_REVIEW_MODEL
# env vars to override with anything else pulled -- see `ollama list`); two
# different model families so the review step is a real second opinion, not
# the same model checking its own work. --review is not required when
# --local is set (defaults to LOCAL_REVIEW_MODEL); passing --review together
# with --local overrides just the reviewer's model tag.
#
# Usage:
#   scripts/fix-audited-code.sh --review <model-id>
#       [--pool local|free|subscription|all | --combo <name>]
#       [--report <path>] [--exclude <prefix>]... [--worktree]
#       [--local] [--no-push] [--parallel N] [--rounds N]
#       [build_dir] [file_filter]
#
# Examples:
#   scripts/fix-audited-code.sh --review claude/claude-sonnet-5
#   scripts/fix-audited-code.sh --review claude/claude-sonnet-5 --pool free
#   scripts/fix-audited-code.sh --review claude/claude-sonnet-5 --pool free \
#       --exclude main/apps/ --worktree build.dev "*/main/*"
#   scripts/fix-audited-code.sh --review claude/claude-sonnet-5 --worktree \
#       --report launcher/tools/results/misra___apps_sand___.txt
#   scripts/fix-audited-code.sh --local --worktree --no-push
#
# --review is mandatory here unless --local is set (unlike fix-audited-
# docs.sh's optional --review): the reviewer is not a bonus quality gate on
# this script, it's the thing that makes trusting a bulk-parallel free-model
# fixer sane.
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

POOL="local"
COMBO=""
REVIEW_MODEL=""
REPORT=""
USE_WORKTREE=0
LOCAL_MODE=0
LOCAL_FIXER_MODEL="${LOCAL_FIXER_MODEL:-qwen2.5:14b}"
LOCAL_REVIEW_MODEL="${LOCAL_REVIEW_MODEL:-gemma4:26b}"
NO_PUSH=0
MAX_PARALLEL=3
MAX_ROUNDS=2
POSITIONAL=()
EXCLUDES=()

while [ "$#" -gt 0 ]; do
  case "$1" in
    --review)
      REVIEW_MODEL="$2"
      shift 2
      ;;
    --report)
      REPORT="$2"
      shift 2
      ;;
    --exclude)
      EXCLUDES+=("$2")
      shift 2
      ;;
    --pool)
      POOL="$2"
      shift 2
      ;;
    --combo)
      COMBO="$2"
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
    --parallel)
      MAX_PARALLEL="$2"
      shift 2
      ;;
    --rounds)
      MAX_ROUNDS="$2"
      shift 2
      ;;
    *)
      POSITIONAL+=("$1")
      shift
      ;;
  esac
done

if [ -z "$REVIEW_MODEL" ]; then
  if [ "$LOCAL_MODE" = "1" ]; then
    REVIEW_MODEL="$LOCAL_REVIEW_MODEL"
  else
    echo "Usage: $0 --review <model-id> [--pool local|free|subscription|all | --combo <name>] [--report <path>] [--exclude <prefix>]... [--worktree] [--local] [--no-push] [--parallel N] [--rounds N] [build_dir] [file_filter]" >&2
    echo "--review is required unless --local is set (defaults fixer/reviewer to $LOCAL_FIXER_MODEL / $LOCAL_REVIEW_MODEL via Ollama)." >&2
    exit 1
  fi
fi

if [ -z "$COMBO" ]; then
  if [ "$LOCAL_MODE" = "1" ]; then
    COMBO="$LOCAL_FIXER_MODEL"
  else
    case "$POOL" in
      local) COMBO="local-coding" ;;
      free) COMBO="code-fix-free" ;;
      subscription) COMBO="code-fix-subscription" ;;
      all) COMBO="code-fix-all" ;;
      *)
        echo "Unknown --pool '$POOL' (expected local|free|subscription|all), or pass --combo <name> directly." >&2
        exit 1
        ;;
    esac
  fi
fi

BUILD_DIR="${POSITIONAL[0]:-build.dev}"
FILE_FILTER="${POSITIONAL[1]:-*/apps/sand/*}"

# Resolve --report to an absolute path before any cd, so it's still readable
# from inside an isolated worktree -- its own launcher/tools/results/ is
# gitignored and won't contain a report generated in the main checkout.
if [ -n "$REPORT" ]; then
  REPORT="$(cd "$(dirname "$REPORT")" && pwd)/$(basename "$REPORT")"
fi

BRANCH="code-audit-fix-$(date +%Y%m%d-%H%M%S)"
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

  # compile_commands.json lives in a gitignored build dir, so a fresh
  # worktree never has one of its own even though the main checkout does.
  # A plain symlink of the whole build dir is NOT enough here (an earlier
  # version of this did exactly that, and it silently analyzed the wrong
  # tree): compile_commands.json bakes in ABSOLUTE paths to the main
  # checkout's source -- "file", "directory", and every -I/-o argument in
  # "command" -- which don't change no matter which directory cppcheck is
  # actually invoked from. Symlinking only makes the file reachable; cppcheck
  # still reads and reports on the main checkout's real files regardless
  # (and writes its *.dump crash artifacts there too, which is what actually
  # surfaced this: they kept landing outside the worktree).
  #
  # Fix: symlink everything in the build dir EXCEPT compile_commands.json
  # (so the config headers and toolchain response file .cflags reference
  # via absolute path still resolve, unchanged, to the one real shared
  # build), then write a REWRITTEN compile_commands.json as a real file
  # (never through the symlink, which would edit the main checkout's copy)
  # with every occurrence of the main checkout's absolute launcher path
  # replaced by the worktree's own -- so cppcheck genuinely analyzes this
  # worktree's source. Only when --report wasn't already given (that path
  # is read directly, see above) and only if the worktree doesn't already
  # have something at that name.
  if [ -z "$REPORT" ] && [ ! -e "launcher/$BUILD_DIR" ] && [ -d "$REPO_ROOT/launcher/$BUILD_DIR" ]; then
    echo "Rewriting launcher/$BUILD_DIR/compile_commands.json for this worktree's own source..."
    REPO_ROOT_WIN="$(cd "$REPO_ROOT" && pwd -W)"
    WORKTREE_DIR_WIN="$(pwd -W)"
    mkdir -p "launcher/$BUILD_DIR"
    for entry in "$REPO_ROOT/launcher/$BUILD_DIR"/*; do
      name="$(basename "$entry")"
      [ "$name" = "compile_commands.json" ] && continue
      ln -s "$entry" "launcher/$BUILD_DIR/$name"
    done
    node -e '
      const fs = require("fs");
      const [, , srcPath, dstPath, mainRoot, worktreeRoot] = process.argv;
      const replaceAllLiteral = (str, find, repl) => str.split(find).join(repl);
      let content = fs.readFileSync(srcPath, "utf8");
      // Forward-slash form: "directory", and every -I/-D in "command".
      content = replaceAllLiteral(content, mainRoot, worktreeRoot);
      // Backslash form: "file" and -o path in "command", JSON-escaped as
      // double-backslash in the raw text (one real \ is written as \\).
      const bs = (s) => s.split("/").join("\\\\");
      content = replaceAllLiteral(content, bs(mainRoot), bs(worktreeRoot));
      fs.writeFileSync(dstPath, content);
    ' "$REPO_ROOT/launcher/$BUILD_DIR/compile_commands.json" \
      "launcher/$BUILD_DIR/compile_commands.json" \
      "$REPO_ROOT_WIN" "$WORKTREE_DIR_WIN"
  fi
else
  if [ -n "$(git status --porcelain)" ]; then
    echo "Working tree isn't clean. Commit or stash first." >&2
    exit 1
  fi
fi

if [ -z "$REPORT" ]; then
  echo "Running misra_check.sh $BUILD_DIR '$FILE_FILTER'..."
  launcher/tools/misra_check.sh "$BUILD_DIR" "$FILE_FILTER"
  safe_name="$(echo "$FILE_FILTER" | tr -c 'A-Za-z0-9_' '_')"
  REPORT="launcher/tools/results/misra_${safe_name}.txt"
fi

if [ ! -f "$REPORT" ]; then
  echo "No report at $REPORT" >&2
  exit 1
fi

MAX_FINDINGS_PER_FILE=20
MAX_SRC_CHARS=40000

mkdir -p "$TMPDIR/findings" "$TMPDIR/work"

# Parses the cppcheck/MISRA text report into per-file finding lists. Only
# "<path>:<line>:<col>: <severity>: <message> [<id>]" lines match -- the
# report's source-context and "^" caret lines, and unbracketed "note:"
# continuation lines, fall through silently, which is exactly what we want.
# Paths come out backslash-separated (cppcheck ran from launcher/ on
# Windows); normalized to forward slashes and re-rooted at launcher/ here so
# the rest of the script can treat them as ordinary repo-relative paths.
# managed_components/ (vendored, not ours to fix) and anything outside
# main/ is dropped before it ever reaches a model, and so is anything under
# an --exclude prefix (matched against the main/-relative path, e.g.
# "main/apps/" -- repeatable). Writes one findings file per qualifying
# source file, and prints "key<TAB>file<TAB>count" per file for the bash
# loop below to mapfile.
#
# Piped through a temp file rather than `mapfile < <(node -e ...)`: bash's
# set -e does not reliably propagate a failure from inside a process
# substitution, so a node crash here (e.g. EISDIR if $REPORT is somehow a
# directory) would otherwise be silently swallowed as "zero entries found"
# instead of aborting the script -- exactly what happened once already.
if ! node -e '
    const fs = require("fs");
    const [, , reportPath, tmpDir, maxPerFile, ...excludes] = process.argv;
    const lineRe = /^(.+?):(\d+):(\d+):\s+(error|warning|style|performance|portability):\s+(.+?)\s+\[([\w.-]+)\]$/;
    const byFile = new Map();
    const lines = fs.readFileSync(reportPath, "utf8").split(/\r?\n/);
    for (const line of lines) {
      const m = line.match(lineRe);
      if (!m) continue;
      const rawPath = m[1].replace(/\\/g, "/");
      if (!rawPath.startsWith("main/")) continue;
      if (excludes.some(ex => rawPath.startsWith(ex))) continue;
      const resolved = "launcher/" + rawPath;
      if (!fs.existsSync(resolved)) continue;
      const bullet = `- ${m[2]}:${m[3]}: ${m[4]}: ${m[5]} [${m[6]}]`;
      if (!byFile.has(resolved)) byFile.set(resolved, new Set());
      byFile.get(resolved).add(bullet);
    }
    const cap = parseInt(maxPerFile, 10);
    for (const [file, bullets] of byFile) {
      const key = file.replace(/[^A-Za-z0-9_]+/g, "_");
      const arr = [...bullets].slice(0, cap);
      fs.writeFileSync(`${tmpDir}/findings/${key}.txt`, arr.join("\n") + "\n");
      console.log(`${key}\t${file}\t${bullets.size}`);
    }
  ' "$REPORT" "$TMPDIR" "$MAX_FINDINGS_PER_FILE" ${EXCLUDES[@]+"${EXCLUDES[@]}"} > "$TMPDIR/report_entries.txt"; then
  echo "Report parsing failed (see node error above) -- aborting." >&2
  exit 1
fi
mapfile -t FILE_ENTRIES < "$TMPDIR/report_entries.txt"

if [ "${#FILE_ENTRIES[@]}" -eq 0 ]; then
  echo "No fixable findings under main/ in $REPORT. Nothing to do."
  exit 0
fi

echo "${#FILE_ENTRIES[@]} file(s) with findings to fix (up to $MAX_PARALLEL in parallel, $COMBO fixer, $REVIEW_MODEL review, $MAX_ROUNDS round(s)):"
printf '  %s\n' "${FILE_ENTRIES[@]}"
echo ""

# Pulls choices[0].message.content out of an `omniroute --output json chat`
# response, same as fix-audited-docs.sh.
cat > "$TMPDIR/extract-content.js" << "JSEOF"
const fs = require("fs");
const text = fs.readFileSync(process.argv[2], "utf8");
const start = text.indexOf("{");
const end = text.lastIndexOf("}");
if (start === -1 || end === -1) { process.exit(1); }
const envelope = JSON.parse(text.slice(start, end + 1));
process.stdout.write(envelope.choices[0].message.content);
JSEOF

# Bracket-depth-aware JSON array extraction (a patch's replace/reason text
# can itself contain "[" or "]"), same as fix-audited-docs.sh.
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

# chat_call PROMPT_FILE OUT_FILE MODEL_ID MAX_TOKENS [REASONING_EFFORT] LOG_FILE
# Writes the model's raw text response (unwrapped from any provider
# envelope) to OUT_FILE, appending any failure detail to LOG_FILE.
# LOCAL_MODE=1 calls Ollama directly instead of OmniRoute -- see the --local
# comment near the top of this file. Returns nonzero on failure. Runs inside
# fix_one_file's subshells same as everything else there, so it's defined
# once here rather than per call site.
#
# --think=false is not optional: confirmed live on this machine that
# reasoning-capable models (gemma4:26b, qwen3.8:27b -- not just the
# obviously-named deepseek-r1 ones) print a full "Thinking... ...done
# thinking." preamble to stdout by default, plain text, no <think> tags to
# regex out. Left in, that preamble becomes part of what extract-array.js
# scans for a JSON array, and since it scans from the FIRST "[" in the
# whole text, any "[" the reasoning text happens to contain (a MISRA rule
# ID, a markdown link, a quoted example) before the real answer would win
# out over the real array. --think=false suppresses this cleanly for every
# locally-pulled model tested (reasoning and non-reasoning alike).
chat_call() {
  local prompt_file="$1" out_file="$2" model_id="$3" max_tokens="$4" effort="$5" log_file="$6"
  if [ "$LOCAL_MODE" = "1" ]; then
    if ! ollama run "$model_id" --think=false < "$prompt_file" > "$out_file" 2>>"$log_file"; then
      echo "  ollama run $model_id failed" >> "$log_file"
      return 1
    fi
    return 0
  fi
  local envelope="$out_file.envelope"
  local effort_args=()
  [ -n "$effort" ] && effort_args=(--reasoning-effort "$effort")
  if ! omniroute --output json chat -m "$model_id" "${effort_args[@]}" --max-tokens "$max_tokens" \
        --file "$prompt_file" --no-history > "$envelope" 2>>"$log_file"; then
    echo "  omniroute call failed" >> "$log_file"
    return 1
  fi
  node "$TMPDIR/extract-content.js" "$envelope" > "$out_file" 2>>"$log_file"
}

fix_one_file() {
  local key="$1" file="$2" workdir="$3"
  mkdir -p "$workdir"
  local log="$workdir/log.txt"
  : > "$log"
  local findings
  findings="$(cat "$TMPDIR/findings/$key.txt")"

  {
    echo "You are fixing static-analysis findings (cppcheck / MISRA C:2012) in one"
    echo "C/C++ source file from a firmware repo (ESP32-C6 app shell). Below are the"
    echo "findings for this file, then the file's current content."
    echo ""
    echo "For each finding you can resolve as a single, minimal, exact text"
    echo "substitution that keeps the code correct and behavior-preserving, output a"
    echo "JSON object: {\"find\": \"<exact substring, verbatim, from the file text"
    echo "below>\", \"replace\": \"<corrected substring>\"}. The \"find\" string MUST"
    echo "appear character-for-character in the file text below -- do not paraphrase"
    echo "it, and keep it as short as possible while still being unambiguous (the"
    echo "specific offending expression or line, not the whole function). Preserve"
    echo "existing style and indentation. Rule text for MISRA IDs isn't available"
    echo "here -- reason from the rule number and the message. Skip any finding that"
    echo "needs more than a simple substitution (control-flow restructuring,"
    echo "anything you're not fully confident preserves behavior, or anything that"
    echo "looks like a false positive)."
    echo ""
    echo "Output ONLY a JSON array of such objects, nothing else -- no markdown"
    echo "fences, no commentary. If none apply, output exactly: []"
    echo ""
    echo "=== FINDINGS ==="
    echo "$findings"
    echo ""
    echo "=== FILE: $file ==="
    head -c "$MAX_SRC_CHARS" "$file"
  } > "$workdir/fix_prompt.txt"

  echo "fixing $file" >> "$log"
  if ! chat_call "$workdir/fix_prompt.txt" "$workdir/fix_content.txt" "$COMBO" 3000 low "$log"; then
    echo "  fixer call failed" >> "$log"
    echo "[]" > "$workdir/patches.json"
    return 0
  fi

  node -e '
    const fs = require("fs");
    const [, extractorPath, contentFile, filePath, outPath] = process.argv;
    const { extractJsonArray } = require(extractorPath);
    const text = fs.readFileSync(contentFile, "utf8");
    const slice = extractJsonArray(text);
    if (!slice) { console.error("  no JSON array found in fixer response"); fs.writeFileSync(outPath, "[]"); process.exit(0); }
    let arr;
    try { arr = JSON.parse(slice); }
    catch (e) { console.error("  could not parse fixer JSON: " + e.message); fs.writeFileSync(outPath, "[]"); process.exit(0); }
    const fileContent = fs.readFileSync(filePath, "utf8");
    const kept = [];
    for (const p of arr) {
      if (!p || typeof p.find !== "string" || typeof p.replace !== "string") continue;
      if (!fileContent.includes(p.find)) {
        console.error(`  SKIP (no exact match in file): "${p.find.slice(0, 60)}"`);
        continue;
      }
      console.error(`  patch: "${p.find.slice(0, 60)}" -> "${p.replace.slice(0, 60)}"`);
      kept.push(p);
    }
    fs.writeFileSync(outPath, JSON.stringify(kept));
  ' "$TMPDIR/extract-array.js" "$workdir/fix_content.txt" "$file" "$workdir/patches.json" >> "$log" 2>&1

  local round=1
  while true; do
    local patch_count
    patch_count=$(node -e 'console.log(JSON.parse(require("fs").readFileSync(process.argv[1],"utf8")).length)' "$workdir/patches.json")
    if [ "$patch_count" -eq 0 ]; then
      echo "  no patches survived verification" >> "$log"
      return 0
    fi

    # Apply the current patch set to a scratch copy and diff it, for review.
    node -e '
      const fs = require("fs");
      const [, filePath, patchesFile, outFile] = process.argv;
      let content = fs.readFileSync(filePath, "utf8");
      const patches = JSON.parse(fs.readFileSync(patchesFile, "utf8"));
      for (const p of patches) {
        if (content.includes(p.find)) content = content.split(p.find).join(p.replace);
      }
      fs.writeFileSync(outFile, content);
    ' "$file" "$workdir/patches.json" "$workdir/proposed.txt"
    diff -u "$file" "$workdir/proposed.txt" > "$workdir/diff.txt" || true

    {
      echo "Here is a JSON array of proposed find/replace patches responding to"
      echo "static-analysis findings in one C/C++ file, followed by a unified diff of"
      echo "what applying them produces. For each item (by its 0-based index in the"
      echo "array), decide VALID (correctly and minimally fixes what it responds to,"
      echo "without changing behavior or introducing a new bug or MISRA violation) or"
      echo "INVALID."
      echo ""
      echo "Output ONLY a JSON array of {\"index\": N, \"verdict\": \"VALID\"|\"INVALID\","
      echo "\"reason\": \"...\"}, one entry per patch, nothing else."
      echo ""
      echo "=== ORIGINAL FINDINGS ==="
      echo "$findings"
      echo ""
      echo "=== PROPOSED PATCHES ==="
      cat "$workdir/patches.json"
      echo ""
      echo "=== DIFF: $file ==="
      cat "$workdir/diff.txt"
    } > "$workdir/review_prompt_r$round.txt"

    echo "  round $round: sending to $REVIEW_MODEL for review" >> "$log"
    if ! chat_call "$workdir/review_prompt_r$round.txt" "$workdir/review_content_r$round.txt" "$REVIEW_MODEL" 4000 "" "$log"; then
      echo "  review call failed -- keeping patches unreviewed" >> "$log"
      return 0
    fi

    node -e '
      const fs = require("fs");
      const [, extractorPath, contentFile, patchesFile, rejectedFile] = process.argv;
      const { extractJsonArray } = require(extractorPath);
      const text = fs.readFileSync(contentFile, "utf8");
      const slice = extractJsonArray(text);
      const all = JSON.parse(fs.readFileSync(patchesFile, "utf8"));
      if (!slice) { console.error("  no verdicts found -- keeping patches unreviewed"); fs.writeFileSync(rejectedFile, "[]"); process.exit(0); }
      let verdicts;
      try { verdicts = JSON.parse(slice); }
      catch (e) { console.error("  could not parse verdicts -- keeping patches unreviewed"); fs.writeFileSync(rejectedFile, "[]"); process.exit(0); }
      const byIndex = new Map(verdicts.map(v => [v.index, v]));
      const kept = [], rejected = [];
      all.forEach((p, i) => {
        const v = byIndex.get(i);
        if (v && v.verdict === "INVALID") {
          console.error(`  REJECTED "${p.find.slice(0,50)}" -> "${p.replace.slice(0,50)}": ${v.reason || "no reason given"}`);
          rejected.push({ find: p.find, replace: p.replace, reason: v.reason || "" });
        } else {
          kept.push(p);
        }
      });
      fs.writeFileSync(patchesFile, JSON.stringify(kept));
      fs.writeFileSync(rejectedFile, JSON.stringify(rejected));
      console.error(`  ${kept.length}/${all.length} patch(es) approved this round.`);
    ' "$TMPDIR/extract-array.js" "$workdir/review_content_r$round.txt" "$workdir/patches.json" "$workdir/rejected.json" >> "$log" 2>&1

    local rejected_count
    rejected_count=$(node -e 'console.log(JSON.parse(require("fs").readFileSync(process.argv[1],"utf8")).length)' "$workdir/rejected.json")
    if [ "$rejected_count" -eq 0 ]; then
      echo "  all patches approved after round $round" >> "$log"
      return 0
    fi
    if [ "$round" -ge "$MAX_ROUNDS" ]; then
      echo "  $rejected_count patch(es) still rejected after $MAX_ROUNDS round(s) -- dropping them" >> "$log"
      return 0
    fi

    {
      echo "A reviewer rejected some of your proposed patches to $file. Revise ONLY"
      echo "the rejected ones below, given the reviewer's reason. Keep the same JSON"
      echo "patch schema: {\"find\": \"<exact substring, verbatim, from the file text"
      echo "below>\", \"replace\": \"<corrected substring>\"}. If you can't produce a"
      echo "confident fix for one, omit it entirely rather than guessing."
      echo ""
      echo "Output ONLY a JSON array of patch objects for the rejected ones you can"
      echo "now confidently fix, nothing else. If none, output exactly: []"
      echo ""
      echo "=== REJECTED PATCHES AND REVIEWER REASONS ==="
      cat "$workdir/rejected.json"
      echo ""
      echo "=== FILE: $file ==="
      head -c "$MAX_SRC_CHARS" "$file"
    } > "$workdir/revise_prompt.txt"

    echo "  round $round: sending rejected patch(es) back to $COMBO for revision" >> "$log"
    if ! chat_call "$workdir/revise_prompt.txt" "$workdir/revise_content.txt" "$COMBO" 3000 low "$log"; then
      echo "  revise call failed -- keeping already-approved patches only" >> "$log"
      return 0
    fi

    node -e '
      const fs = require("fs");
      const [, extractorPath, contentFile, filePath, patchesFile] = process.argv;
      const { extractJsonArray } = require(extractorPath);
      const text = fs.readFileSync(contentFile, "utf8");
      const slice = extractJsonArray(text);
      if (!slice) { console.error("  no revised patches found"); process.exit(0); }
      let arr;
      try { arr = JSON.parse(slice); }
      catch (e) { console.error("  could not parse revised JSON: " + e.message); process.exit(0); }
      const fileContent = fs.readFileSync(filePath, "utf8");
      const all = JSON.parse(fs.readFileSync(patchesFile, "utf8"));
      for (const p of arr) {
        if (!p || typeof p.find !== "string" || typeof p.replace !== "string") continue;
        if (!fileContent.includes(p.find)) {
          console.error(`  SKIP revised patch (no exact match in file): "${p.find.slice(0, 60)}"`);
          continue;
        }
        console.error(`  revised patch: "${p.find.slice(0, 60)}" -> "${p.replace.slice(0, 60)}"`);
        all.push(p);
      }
      fs.writeFileSync(patchesFile, JSON.stringify(all));
    ' "$TMPDIR/extract-array.js" "$workdir/revise_content.txt" "$file" "$workdir/patches.json" >> "$log" 2>&1

    round=$((round + 1))
  done
}

running=0
for entry in "${FILE_ENTRIES[@]}"; do
  IFS=$'\t' read -r key file count <<< "$entry"
  ( fix_one_file "$key" "$file" "$TMPDIR/work/$key" ) &
  running=$((running + 1))
  if [ "$running" -ge "$MAX_PARALLEL" ]; then
    wait -n
    running=$((running - 1))
  fi
done
wait

PATCHES="$TMPDIR/patches.json"
echo "[]" > "$PATCHES"
for entry in "${FILE_ENTRIES[@]}"; do
  IFS=$'\t' read -r key file count <<< "$entry"
  workdir="$TMPDIR/work/$key"
  echo ""
  echo "=== $file ==="
  cat "$workdir/log.txt" 2>/dev/null || true
  node -e '
    const fs = require("fs");
    const [, filePath, moreFile, allPatchesFile] = process.argv;
    const more = JSON.parse(fs.readFileSync(moreFile, "utf8"));
    const all = JSON.parse(fs.readFileSync(allPatchesFile, "utf8"));
    for (const p of more) all.push({ file: filePath, find: p.find, replace: p.replace });
    fs.writeFileSync(allPatchesFile, JSON.stringify(all));
  ' "$file" "$workdir/patches.json" "$PATCHES" 2>/dev/null || true
done

PATCH_COUNT=$(node -e 'console.log(JSON.parse(require("fs").readFileSync(process.argv[1], "utf8")).length)' "$PATCHES")
echo ""
echo "$PATCH_COUNT reviewer-approved patch(es) across ${#FILE_ENTRIES[@]} file(s)."
if [ "$PATCH_COUNT" -eq 0 ]; then
  echo "Nothing to apply."
  exit 0
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

# Normalize the fixer's injected replacement text to house style before
# committing. Deliberately a post-step on just the files this run already
# touched, not a pre-scan pass over everything: reformatting whole files up
# front would land style-only diffs in the same commit as the audit fixes,
# which is exactly the full-file-rewrite blast radius the find/replace-patch
# design above exists to avoid. check-format.sh needs clang-format
# installed; if that's missing or too old, warn and commit unformatted
# rather than losing an otherwise-good, reviewer-approved fix over it.
FORMAT_FILES=""
for f in $CHANGED_FILES; do
  case "$f" in
    *.c | *.h) FORMAT_FILES="$FORMAT_FILES $f" ;;
  esac
done
if [ -n "$FORMAT_FILES" ]; then
  echo "Formatting changed files with check-format.sh..."
  # shellcheck disable=SC2086 # FORMAT_FILES is a deliberate word-split file list
  if ! scripts/check-format.sh $FORMAT_FILES; then
    echo "  check-format.sh failed (clang-format missing/too old?) -- committing unformatted." >&2
  fi
fi

if [ "$USE_WORKTREE" != "1" ]; then
  git checkout -b "$BRANCH"
fi
git add $CHANGED_FILES
git commit -m "fix: resolve static-analysis findings (via $COMBO, reviewed by $REVIEW_MODEL)"

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
