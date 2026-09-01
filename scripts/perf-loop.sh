#!/bin/sh
#
# Evaluate optimisation candidates unattended, and refuse to let one be
# accepted for the wrong reason.
#
# Each candidate is a shell command that edits the implementation. This
# script applies it, runs every gate below in order, classifies the result,
# reverts the tree, and moves on. Nothing it accepts ever touches main -
# accepted candidates are committed to a branch for review.
#
#   scripts/perf-loop.sh --baseline REPORT.md --candidates FILE
#   scripts/perf-loop.sh --baseline REPORT.md --candidate "sed -i ... file.c"
#   scripts/perf-loop.sh --host-only --candidate "..."     # no device needed
#
# THE GATES, cheapest first, so a bad candidate dies in seconds rather than
# after a seven-minute device round-trip:
#
#   A  allowlist   the diff may only touch implementation files
#   B  host        run_tests.sh green, and check_app_sources.sh clean
#   C  fingerprint the simulation still produces the same grid
#   D  device      build, flash, capture, and validate that capture
#   E  verdict     a measured win beyond the noise floor, no regression
#
# WHY GATE A EXISTS, and why it is first. Every sand budget in this project
# is a deliberately-failing reduction target. An agent told "make the tests
# pass" therefore has two routes: do the optimisation, or raise the budget.
# The second is trivially cheaper, and weakening a scene is cheaper still -
# this repo has already seen a test lose its ability to detect a broken cap
# while staying green. A rule in a document does not stop that; an
# allowlist does. Budgets and scenes live in files this script will not let
# a candidate open, so both cheats are unreachable rather than discouraged.
#
# WHY GATE C EXISTS. Gates B and D only prove the code is fast and the
# assertions still hold at the points they happen to look. Neither proves
# the simulation is unchanged. See grid_fingerprint.c's own top comment.
#
# THE THREE OUTCOMES. A candidate is not simply accepted or rejected:
#
#   ACCEPT      won, and behaviour is byte-identical -> committed to branch
#   QUARANTINE  won, but behaviour changed -> patch kept for a human
#   REJECT      failed a gate, or did not win -> logged and discarded
#
# Quarantine exists because "byte-identical" would throw away legitimate
# wins. Reordering the burning cell's three neighbour walks was priced at
# ~5% and deferred precisely because it changes observable ordering while
# staying physically valid. That class of change must not be silently
# binned, but it must not be auto-accepted either. It is the pile to read
# in the morning - and the fingerprint's material histogram tells you which
# ones are reorderings and which are bugs.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
LAUNCHER_DIR="$REPO_ROOT/launcher"
SAND_TOOLS="$LAUNCHER_DIR/main/apps/sand/tools"

BASELINE_REPORT=""
CANDIDATE=""
CANDIDATES_FILE=""
HOST_ONLY=0
BRANCH="perf-loop-accepted"
OUT_DIR="$REPO_ROOT/.perf-loop"
COM_PORT="COM3"

usage() {
    sed -n '2,60p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
    --baseline)   BASELINE_REPORT="$2"; shift 2 ;;
    --candidate)  CANDIDATE="$2";       shift 2 ;;
    --candidates) CANDIDATES_FILE="$2"; shift 2 ;;
    --branch)     BRANCH="$2";          shift 2 ;;
    --out)        OUT_DIR="$2";         shift 2 ;;
    --port)       COM_PORT="$2";        shift 2 ;;
    --host-only)  HOST_ONLY=1;          shift ;;
    -h|--help)    usage 0 ;;
    *) echo "Unknown argument: $1" >&2; usage 2 ;;
    esac
done

if [ -z "$CANDIDATE" ] && [ -z "$CANDIDATES_FILE" ]; then
    echo "Nothing to evaluate: pass --candidate or --candidates." >&2
    exit 2
fi
if [ "$HOST_ONLY" -eq 0 ] && [ -z "$BASELINE_REPORT" ]; then
    echo "--baseline REPORT.md is required unless --host-only is given:" >&2
    echo "without it there is nothing to measure a win against." >&2
    exit 2
fi

mkdir -p "$OUT_DIR/quarantine"
LOG="$OUT_DIR/loop.log"
SUMMARY="$OUT_DIR/summary.txt"

# A dirty tree makes every verdict meaningless: the gates below would be
# judging the candidate plus whatever was already sitting there, and the
# revert between candidates would throw the user's work away. Refuse
# outright rather than try to be clever about stashing.
if [ -n "$(git -C "$REPO_ROOT" status --porcelain)" ]; then
    echo "Working tree is not clean. Commit or stash first - this script" >&2
    echo "reverts the tree between candidates and would discard your work." >&2
    exit 2
fi

START_COMMIT=$(git -C "$REPO_ROOT" rev-parse HEAD)

log() {
    printf '%s\n' "$*" | tee -a "$LOG"
}

# --- gate A ----------------------------------------------------------------
# Deliberately an allowlist, not a denylist: a denylist has to anticipate
# every file worth protecting, and the one it forgets is the one that gets
# edited. Untracked files are checked too (status --porcelain, not diff),
# or a candidate could simply add a new file nobody vetted.
allowlist_ok() {
    changed=$(git -C "$REPO_ROOT" status --porcelain | awk '{print $NF}')
    [ -z "$changed" ] && { echo "candidate changed nothing"; return 1; }
    for f in $changed; do
        case "$f" in
        # Implementation the loop may optimise.
        launcher/main/apps/sand/sand.c \
        |launcher/main/apps/sand/sand_reactions.c \
        |launcher/main/apps/sand/sand_gas.c \
        |launcher/main/apps/sand/sand_liquid.c \
        |launcher/main/apps/sand/material.c \
        |launcher/main/apps/sand/row_runs.c \
        |launcher/main/apps/sand/palette.c \
        |launcher/main/apps/sand/tilt.c \
        |launcher/main/apps/sand/sand.h \
        |launcher/main/apps/sand/sand_priv.h \
        |launcher/main/apps/sand/material.h \
        |launcher/main/gfx/*.c \
        |launcher/main/gfx/*.h)
            ;;
        *)
            echo "$f"
            return 1
            ;;
        esac
    done
    return 0
}

revert_tree() {
    git -C "$REPO_ROOT" reset --hard "$START_COMMIT" >/dev/null 2>&1
    # Scoped to the allowlisted directories, and deliberately WITHOUT -x:
    # captures and build dirs are gitignored, and -x would delete every one
    # of them - including the baseline report this run is measuring against.
    # Untracked-but-not-ignored files a candidate created are removed, which
    # is the intent; ignored files are left alone, which is essential.
    git -C "$REPO_ROOT" clean -fdq -- launcher/main/apps/sand launcher/main/gfx \
        >/dev/null 2>&1 || true
}

record() {
    verdict="$1"; label="$2"; detail="$3"
    printf '%-11s %-28s %s\n' "$verdict" "$label" "$detail" >> "$SUMMARY"
    log "=== $verdict: $label - $detail"
}

evaluate() {
    label="$1"; cmd="$2"
    log ""
    log "########## candidate: $label"
    log "########## command:   $cmd"

    if ! sh -c "$cmd" >>"$LOG" 2>&1; then
        record REJECT "$label" "candidate command itself failed"
        revert_tree; return
    fi

    if ! offending=$(allowlist_ok); then
        record REJECT "$label" "touched a file outside the allowlist: $offending"
        revert_tree; return
    fi

    if ! TEST_BUILD_DIR="$OUT_DIR/hostbuild" \
         sh "$LAUNCHER_DIR/test/run_tests.sh" >>"$LOG" 2>&1; then
        record REJECT "$label" "host suite failed (compile or test)"
        revert_tree; return
    fi

    if [ -x "$LAUNCHER_DIR/test/check_app_sources.sh" ]; then
        if ! sh "$LAUNCHER_DIR/test/check_app_sources.sh" >>"$LOG" 2>&1; then
            record REJECT "$label" "check_app_sources.sh failed"
            revert_tree; return
        fi
    fi

    # Gate C is classified, not fatal: a behaviour change is only
    # interesting once we know whether the candidate was also a win.
    BEHAVIOUR=identical
    FP_DIFF="$OUT_DIR/quarantine/$label.fingerprint.diff"
    if ! sh "$SAND_TOOLS/report_fingerprint.sh" --check >"$FP_DIFF" 2>&1; then
        BEHAVIOUR=changed
    else
        rm -f "$FP_DIFF"
    fi

    if [ "$HOST_ONLY" -eq 1 ]; then
        record HOST-OK "$label" "gates A-C passed, behaviour=$BEHAVIOUR (no device evidence)"
        revert_tree; return
    fi

    # Gate D. report_performance.sh does the whole device round-trip and
    # refuses to emit a report from a capture that measured nothing, so a
    # failure here is either a broken build or a broken capture - never a
    # silently empty result.
    NEW_REPORT="$OUT_DIR/$label.report.md"
    if ! sh "$SAND_TOOLS/report_performance.sh" "$COM_PORT" "$NEW_REPORT" \
         >>"$LOG" 2>&1; then
        record REJECT "$label" "device build/flash/capture failed or was invalid"
        revert_tree; return
    fi

    VERDICT_OUT="$OUT_DIR/$label.verdict.txt"
    if python "$SAND_TOOLS/compare_reports.py" "$BASELINE_REPORT" \
              "$NEW_REPORT" --verdict >"$VERDICT_OUT" 2>&1; then
        WIN=1
    else
        WIN=0
    fi
    headline=$(head -1 "$VERDICT_OUT")

    if [ "$WIN" -eq 0 ]; then
        record REJECT "$label" "no measured win: $headline"
        revert_tree; return
    fi

    if [ "$BEHAVIOUR" = changed ]; then
        git -C "$REPO_ROOT" diff > "$OUT_DIR/quarantine/$label.patch"
        record QUARANTINE "$label" "WON but behaviour changed: $headline"
        log "    patch:       $OUT_DIR/quarantine/$label.patch"
        log "    fingerprint: $FP_DIFF"
        log "    Read the histogram columns: identical counts with a changed"
        log "    hash is a reordering; changed counts mean material was"
        log "    created or destroyed, which is a bug until shown otherwise."
        revert_tree; return
    fi

    # ACCEPT. Committed onto a branch, never onto whatever is checked out:
    # an unattended run must not be able to move main.
    git -C "$REPO_ROOT" add -A
    git -C "$REPO_ROOT" stash push -q -m "perf-loop-$label"
    git -C "$REPO_ROOT" checkout -q -B "$BRANCH" "$START_COMMIT"
    git -C "$REPO_ROOT" stash pop -q
    git -C "$REPO_ROOT" add -A
    git -C "$REPO_ROOT" commit -q -m "perf-loop: $label

$headline

Accepted by scripts/perf-loop.sh: host suite green, grid fingerprint
byte-identical to baseline, and a device-measured win beyond the noise
floor with no row regressing past it. Not reviewed by a human."
    git -C "$REPO_ROOT" checkout -q -
    record ACCEPT "$label" "$headline"
}

: > "$SUMMARY"
log "perf-loop starting at $(git -C "$REPO_ROOT" rev-parse --short HEAD)"
log "host-only=$HOST_ONLY branch=$BRANCH out=$OUT_DIR"

if [ -n "$CANDIDATE" ]; then
    evaluate "manual" "$CANDIDATE"
fi

if [ -n "$CANDIDATES_FILE" ]; then
    n=0
    # Format: one candidate per line, "<label><TAB><shell command>".
    # Blank lines and # comments ignored, so a candidate list can be
    # annotated with what each one is trying and why.
    while IFS="$(printf '\t')" read -r label cmd; do
        case "$label" in ""|\#*) continue ;; esac
        [ -z "${cmd:-}" ] && continue
        n=$((n + 1))
        evaluate "$label" "$cmd"
    done < "$CANDIDATES_FILE"
    log "evaluated $n candidates from $CANDIDATES_FILE"
fi

log ""
log "########## summary"
cat "$SUMMARY" | tee -a "$LOG"
log ""
log "Accepted candidates are on branch '$BRANCH' and are NOT reviewed."
log "Quarantined patches are in $OUT_DIR/quarantine - read those first."
