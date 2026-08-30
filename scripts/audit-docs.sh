#!/usr/bin/env bash
# On-demand doc audit: for each doc file, pull out the source paths it
# actually names -- both prose references (`main/post.c`) and #include
# lines inside its own fenced code examples -- and challenge them in two
# tiers:
#   1. Deterministic: does that path still exist? If not, is a same-named
#      file findable elsewhere (i.e. it moved)? No model call needed here.
#      Relative #include paths (`../gfx.h`) cannot be checked this way --
#      there is no fixed root to resolve them against -- so instead their
#      real-file match is surfaced to tier 2 as something to reason about.
#   2. OmniRoute's free "docs-update-free" combo cross-checks the doc's own
#      text against the current content of whatever paths DID resolve --
#      function/macro names, register addresses, magic numbers, described
#      behavior, and whether a code example's relative #include depth is
#      consistent with the file location the doc says to create -- and
#      flags concrete contradictions.
# This is a report, not an editor: unlike update-docs.sh it makes no commits
# and picks no diff window -- it catches doc rot that accumulated over time,
# not just drift from the latest change.
#
# Usage: scripts/audit-docs.sh [doc-file ...]   (default: all tracked docs)
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

COMBO="docs-update-free"
SRC_ROOT="launcher"                 # docs refer to main/... relative to here
MAX_DOC_CHARS=80000                 # cap so even the smallest fallback model fits
MAX_SRC_CHARS=40000
MAX_REFS=8

if [ "$#" -gt 0 ]; then
  DOC_FILES="$*"
else
  DOC_FILES=$(git ls-files README.md docs)
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
REPORT="$TMPDIR/report.md"
: > "$REPORT"

for doc in $DOC_FILES; do
  [ -f "$doc" ] || continue
  echo "=== $doc ==="

  mapfile -t RAW_REFS < <(
    grep -oE '`[A-Za-z0-9_./-]+\.(c|h|cpp|hpp|py|sh|cmake)`' "$doc" \
      | tr -d '`' | sort -u
  )

  # #include "..." inside the doc's own fenced code examples. These are
  # relative to wherever the tutorial says to create the file, which this
  # script has no reliable way to know -- so they are never treated as a
  # deterministic path claim. Instead, resolve the real file by basename
  # and hand the model both the as-written include and the real location,
  # so it can judge whether the relative depth is actually consistent with
  # where the doc says the new file goes.
  mapfile -t CODE_INCLUDES < <(
    grep -ohE '#include[[:space:]]+"[^"]+"' "$doc" \
      | sed -E 's/#include[[:space:]]+"([^"]+)"/\1/' | sort -u
  )

  FOUND=()
  PATH_ISSUES=()
  CODE_INCLUDE_NOTES=()
  for inc in ${CODE_INCLUDES[@]+"${CODE_INCLUDES[@]}"}; do
    base=$(basename "$inc")
    hit=$(find "$SRC_ROOT" -name "$base" 2>/dev/null | head -1)
    if [ -n "$hit" ]; then
      FOUND+=("$hit")
      CODE_INCLUDE_NOTES+=("- Code example has \`#include \"$inc\"\`; the real file named \`$base\` is at \`$hit\`. Check whether the \`../\` depth in the include is consistent with where the doc says to create the new file.")
    fi
  done

  for ref in ${RAW_REFS[@]+"${RAW_REFS[@]}"}; do
    if [ -f "$SRC_ROOT/$ref" ]; then
      FOUND+=("$SRC_ROOT/$ref")
      continue
    elif [ -f "$ref" ]; then
      FOUND+=("$ref")
      continue
    fi

    case "$ref" in
      */*)
        # Has a directory component -- this is an actual path claim, so a
        # miss is worth flagging (possibly the file moved).
        base=$(basename "$ref")
        hit=$(find "$SRC_ROOT" -name "$base" 2>/dev/null | head -1)
        if [ -n "$hit" ]; then
          PATH_ISSUES+=("- CLAIM: doc references \`$ref\`")
          PATH_ISSUES+=("  SOURCE: no such path -- a file named \`$base\` exists instead at \`$hit\`")
          FOUND+=("$hit")
        else
          PATH_ISSUES+=("- CLAIM: doc references \`$ref\`")
          PATH_ISSUES+=("  SOURCE: no file by that name exists anywhere under $SRC_ROOT/")
        fi
        ;;
      *)
        # Bare filename -- informal shorthand, not a path claim. Resolve it
        # quietly for content-checking; don't flag it either way if it's
        # not found (e.g. idf.py is an external tool, not part of this repo).
        hit=$(find "$SRC_ROOT" -name "$ref" 2>/dev/null | head -1)
        [ -n "$hit" ] && FOUND+=("$hit")
        ;;
    esac
  done

  if [ "${#PATH_ISSUES[@]}" -gt 0 ]; then
    echo "  path issues: $((${#PATH_ISSUES[@]} / 2))"
    {
      echo "## $doc"
      echo ""
      printf '%s\n' "${PATH_ISSUES[@]}"
      echo ""
    } >> "$REPORT"
  fi

  if [ "${#FOUND[@]}" -eq 0 ]; then
    echo "  no resolvable source references to cross-check further"
    continue
  fi
  mapfile -t FOUND < <(printf '%s\n' "${FOUND[@]}" | sort -u | head -"$MAX_REFS")

  PROMPT="$TMPDIR/prompt.txt"
  RESPONSE="$TMPDIR/response.txt"

  {
    echo "You are auditing one documentation file from a firmware repo (ESP32-C6 app"
    echo "shell) against the actual source it names. Below is the doc's full text,"
    echo "then the current content of every source file it references that still"
    echo "exists at the path it claims."
    echo ""
    echo "Cross-check every concrete, checkable claim in the doc against that source:"
    echo "function/macro/variable names, register addresses, pin/GPIO numbers, magic"
    echo "numbers, and described behavior. Only report things the provided source"
    echo "actually contradicts -- do not flag missing coverage, style, or claims you"
    echo "can't verify from what's given."
    echo ""
    if [ "${#CODE_INCLUDE_NOTES[@]}" -gt 0 ]; then
      echo "The doc's own code examples contain #include lines naming files by a"
      echo "relative path. For each one below, work out how many directory levels"
      echo "separate it from the file location the doc instructs the reader to"
      echo "create (often stated just before the code block), and check whether"
      echo "that matches the number of \`../\` segments actually written -- flag it"
      echo "as an inaccuracy if the include would not actually resolve to the real"
      echo "file from that location."
      printf '%s\n' "${CODE_INCLUDE_NOTES[@]}"
      echo ""
    fi
    echo "Output one bullet per inaccuracy, each exactly:"
    echo "- CLAIM: <what the doc says, quoted or paraphrased briefly>"
    echo "  SOURCE: <what the source actually shows, with file:line if you can tell>"
    echo "If nothing checkable is wrong, output exactly: NONE"
    echo ""
    echo "=== DOC: $doc ==="
    head -c "$MAX_DOC_CHARS" "$doc"
    echo ""
    echo "=== REFERENCED SOURCE ==="
    total=0
    for f in "${FOUND[@]}"; do
      remaining=$((MAX_SRC_CHARS - total))
      [ "$remaining" -le 0 ] && break
      echo "--- $f ---"
      chunk=$(head -c "$remaining" "$f")
      echo "$chunk"
      total=$((total + ${#chunk}))
    done
  } > "$PROMPT"

  echo "  cross-checking against: ${FOUND[*]}"
  if ! omniroute chat -m "$COMBO" --reasoning-effort low --max-tokens 3000 \
        --file "$PROMPT" --no-history > "$RESPONSE" 2>&1; then
    echo "  omniroute call failed:" >&2
    cat "$RESPONSE" >&2
    continue
  fi

  if grep -qx "NONE" "$RESPONSE"; then
    echo "  clean"
  else
    echo "  found content issues"
    {
      echo "## $doc (content)"
      echo ""
      cat "$RESPONSE"
      echo ""
    } >> "$REPORT"
  fi
done

echo ""
if [ -s "$REPORT" ]; then
  echo "===================== AUDIT REPORT ====================="
  cat "$REPORT"
else
  echo "No inaccuracies found in any checked doc."
fi
