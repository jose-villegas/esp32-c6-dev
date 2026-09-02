#!/bin/sh
#
# One command from a git ref to a device performance verdict.
#
#   scripts/capture_ref.sh <git-ref> [--baseline REPORT.md] [--no-restore] \
#       [--build-only] [COM_PORT]
#
#   <git-ref>       any ref git can resolve: branch, tag, or SHA.
#   --baseline      forwarded to report_performance.sh's own --baseline -
#                   prints its verdict line against an earlier report.
#   --no-restore    forwarded to report_performance.sh's own --no-restore -
#                   leaves the device on build.diag instead of paying a
#                   second build+flash to restore build.release.
#   --build-only    build build.diag in the capture worktree and stop -
#                   never flashes, never touches a serial port. Useful on
#                   its own to pre-check that a candidate even LINKS
#                   (this week's aligned(64) candidate failed only at link
#                   time) before spending a device round on it.
#   COM_PORT        serial port the device is on. Default: COM3. Ignored
#                   entirely under --build-only - no port is ever opened.
#
# This is glue, not a reimplementation: the whole validate+summarize+
# verdict pipeline (free-heap check, the two control rows, compare_reports
# --verdict) already lives in report_performance.sh - see bd esp32c6-s3z,
# which added --baseline and --no-restore for exactly this caller. This
# script's only job is turning a git ref into a warm, ready checkout that
# script can build+flash+capture from, and it calls that script rather
# than duplicating any part of what it already does.
#
# --- the persistent capture worktree ---------------------------------------
#
# Candidate evaluation this week was: create/enter a worktree, detach it
# at a ref, sit through a ~12 minute COLD build, capture, hand-run four
# read-a-capture commands. A worktree reused across candidates instead
# measured ~2-3 minutes per incremental build - idf.py's own object cache
# survives a detach to a nearby commit even though the checked-out files
# change under it. That gap is the entire point of this script, so the
# worktree is created ONCE, at a fixed path, and reused by every later
# invocation rather than made fresh per ref.
#
# WHERE: "$REPO_ROOT/.claude/capture-worktree", where REPO_ROOT is the
# checkout that owns THIS script (two levels up: scripts/ -> repo root) -
# deliberately NOT centralised at the primary checkout the way
# launcher/tools/build_flash_select.sh places branch worktrees it creates.
# That script centralises on purpose: it is routing to a checkout that
# corresponds to one particular branch, and wants every session to find
# the same one. This worktree corresponds to no branch at all - it is a
# scratch build cache that gets re-detached to a different ref on every
# call - and this project already runs most work inside per-session agent
# worktrees that are sandboxed from touching files outside themselves (a
# real constraint, hit while writing this script: a worktree add outside
# the invoking checkout was refused outright). A path relative to
# REPO_ROOT keeps the cache inside whatever checkout is doing the work, so
# it stays warm across every candidate evaluated in one sitting - which is
# the case this script exists for - without one session's in-progress
# capture fighting another session's over a single shared path. Override
# with CAPTURE_WORKTREE_DIR if you deliberately want it somewhere else.
#
# REFUSING A DIRTY WORKTREE: a worktree left mid-build or holding someone's
# unpushed edit must never be silently reset - that discards work with no
# way back. If it is dirty, this script prints `git status --porcelain`
# and stops; clean it up yourself and run again.

set -eu

usage() {
    sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

REF=""
COM_PORT=""
BASELINE=""
NO_RESTORE=0
BUILD_ONLY=0

while [ $# -gt 0 ]; do
    case "$1" in
        --baseline)
            [ $# -ge 2 ] || { echo "ERROR: --baseline requires a path" >&2; exit 2; }
            BASELINE="$2"
            shift 2
            ;;
        --baseline=*)
            BASELINE="${1#--baseline=}"
            shift
            ;;
        --no-restore)
            NO_RESTORE=1
            shift
            ;;
        --build-only)
            BUILD_ONLY=1
            shift
            ;;
        -h|--help)
            usage 0
            ;;
        -*)
            echo "ERROR: unknown flag: $1" >&2
            usage 2
            ;;
        *)
            if [ -z "$REF" ]; then
                REF="$1"
            elif [ -z "$COM_PORT" ]; then
                COM_PORT="$1"
            else
                echo "ERROR: unexpected argument: $1" >&2
                usage 2
            fi
            shift
            ;;
    esac
done

if [ -z "$REF" ]; then
    echo "ERROR: a git ref is required." >&2
    usage 2
fi
COM_PORT="${COM_PORT:-COM3}"

if [ -n "$BASELINE" ] && [ "$BUILD_ONLY" -eq 1 ]; then
    echo "NOTE: --baseline has no effect with --build-only (nothing is" >&2
    echo "captured to compare) - ignoring it." >&2
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
IDF_EXPORT_PS1="${IDF_EXPORT_PS1:-C:\\Espressif\\esp-idf-v5.5\\export.ps1}"

# shellcheck source=lib/capture_worktree.sh
. "$SCRIPT_DIR/lib/capture_worktree.sh"

WORKTREE="$(capture_worktree_path "$REPO_ROOT")"
capture_worktree_checkout "$REPO_ROOT" "$WORKTREE" "$REF"
CAPTURE_SHA="$CAPTURE_WORKTREE_SHA"

LAUNCHER_DIR="$WORKTREE/launcher"
if [ ! -d "$LAUNCHER_DIR" ]; then
    echo "ERROR: $LAUNCHER_DIR does not exist - $REF predates launcher/," >&2
    echo "or is missing it entirely." >&2
    exit 1
fi
LAUNCHER_DIR_WIN="$(cd "$LAUNCHER_DIR" && pwd -W 2>/dev/null || echo "$LAUNCHER_DIR")"

if [ "$BUILD_ONLY" -eq 1 ]; then
    # Same three sdkconfig fragments and SDKCONFIG override
    # report_performance.sh uses (see docs/Sand/Perf-Round-Guide.md,
    # "Exact commands") - just the build half, with no flash and no
    # capture, so a link failure surfaces in minutes instead of after a
    # flash+18-minute-capture cycle. The stale-sdkconfig removal is the
    # same idf.py gotcha report_performance.sh and report_test_results.sh
    # already both work around: idf.py only applies SDKCONFIG_DEFAULTS
    # when it CREATES the sdkconfig, so a leftover one from a previous
    # ref's build would otherwise silently keep winning.
    rm -f "$LAUNCHER_DIR/build.diag/sdkconfig"

    echo "=== Building build.diag for $CAPTURE_SHA (--build-only, no flash) ==="
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
        Remove-Item Env:\MSYSTEM -ErrorAction SilentlyContinue
        & '$IDF_EXPORT_PS1' | Out-Null
        Set-Location '$LAUNCHER_DIR_WIN'
        idf.py -B build.diag -D SDKCONFIG_DEFAULTS=\"sdkconfig.defaults;sdkconfig.defaults.diag;sdkconfig.defaults.diag_autorun\" -D SDKCONFIG=build.diag/sdkconfig build
        exit \$LASTEXITCODE
    "

    for flag in CONFIG_LAUNCHER_SELFTEST CONFIG_LAUNCHER_SELFTEST_AUTORUN; do
        if ! grep -q "^${flag}=y" "$LAUNCHER_DIR/build.diag/sdkconfig"; then
            echo "ERROR: ${flag} is not set in the generated build.diag/sdkconfig -" >&2
            echo "a flashed image built from this would boot without running" >&2
            echo "the suites. The build itself succeeded; only the config is wrong." >&2
            exit 1
        fi
    done

    echo "=== Build only: build.diag built for $REF ($CAPTURE_SHA) ==="
    echo "Build dir: $LAUNCHER_DIR/build.diag"
    exit 0
fi

# --- full round: report_performance.sh does build+flash+capture+validate+
# summarize+verdict. Nothing below duplicates any of that - it only builds
# the argument list report_performance.sh already understands and picks a
# report path this script can name back to the caller afterward.
REF_SLUG=$(printf '%s' "$REF" | tr -c 'A-Za-z0-9._-' '-')
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULTS_DIR="$LAUNCHER_DIR/main/apps/sand/tools/results"
OUT_MD="$RESULTS_DIR/capture_ref_${REF_SLUG}_${TIMESTAMP}.md"

set --
[ "$NO_RESTORE" -eq 1 ] && set -- "$@" --no-restore
[ -n "$BASELINE" ] && set -- "$@" --baseline "$BASELINE"
set -- "$@" "$COM_PORT" "$OUT_MD"

echo "=== Handing off to report_performance.sh for $CAPTURE_SHA ==="
STATUS=0
sh "$LAUNCHER_DIR/main/apps/sand/tools/report_performance.sh" "$@" || STATUS=$?

echo
echo "Report: $OUT_MD"
exit "$STATUS"
