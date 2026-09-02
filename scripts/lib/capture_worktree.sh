# Sourced, not copied - see tools/find_cc.sh's own top comment for the
# same pattern used elsewhere in this repo. Shared logic for "the one
# persistent capture worktree": where it lives, and the create-once /
# refuse-if-dirty / detach-at-ref rules around reusing it. Used by both
# scripts/capture_ref.sh and scripts/perf-loop.sh's --candidate-ref path,
# so there is exactly one place that decides where the worktree lives and
# exactly one place that decides whether it is safe to touch it - a
# second, slightly different copy of "is this dirty" is exactly the kind
# of drift that would let one caller's bug quietly discard the other
# caller's work.
#
# Requires CDPATH unset and `set -eu` in the sourcing script, same as
# every other POSIX-sh script in this repo.

# capture_worktree_path <repo_root>
# Prints the path the persistent worktree lives (or would be created) at.
# CAPTURE_WORKTREE_DIR overrides it - see capture_ref.sh's own top comment
# for why the default is relative to the calling checkout rather than
# centralised at the primary one.
capture_worktree_path() {
    printf '%s\n' "${CAPTURE_WORKTREE_DIR:-$1/.claude/capture-worktree}"
}

# capture_worktree_checkout <repo_root> <worktree_path> <ref>
#
# Creates the worktree on first use, or reuses it - refusing outright,
# with the dirty files printed and NOTHING discarded, if it is not clean.
# Detaches it at <ref> either way. On success prints progress to stdout
# and leaves CAPTURE_WORKTREE_SHA set to the short SHA now checked out.
#
# Refusals `return 1` rather than `exit` - this runs sourced into the
# CALLER's shell, not a subshell, so `exit` here would kill the caller's
# whole process. capture_ref.sh calls this as a bare statement, where
# `set -e` turns that return into exactly the "stop now" it wants;
# perf-loop.sh's evaluate_ref() wraps it in `if ! ...; then`, where the
# same return lets it record one REJECT and move on to the next candidate
# instead of aborting the whole run.
capture_worktree_checkout() {
    _cwt_repo_root="$1"
    _cwt_worktree="$2"
    _cwt_ref="$3"

    if [ -d "$_cwt_worktree" ]; then
        # It exists on disk - but only trust it if git itself knows about
        # it too. A directory left behind by hand (or by a different
        # worktree's administration) would otherwise be silently treated
        # as a real worktree and get `checkout --detach` run inside a
        # plain, unmanaged directory.
        #
        # `git worktree list` prints Windows-style paths ("C:/Users/...")
        # even under Git Bash, where plain `pwd` gives POSIX-style
        # ("/c/Users/...") - the two never compare equal as strings, so
        # this goes through `pwd -W` (same trick LAUNCHER_DIR_WIN uses
        # elsewhere in this repo) rather than the POSIX-style path.
        _cwt_worktree_win="$(cd "$_cwt_worktree" && pwd -W 2>/dev/null || pwd)"
        _cwt_registered=$(git -C "$_cwt_repo_root" worktree list --porcelain \
            | awk -v p="$_cwt_worktree_win" '$1=="worktree" && $2==p {found=1} END{print found+0}')
        if [ "$_cwt_registered" != "1" ]; then
            echo "ERROR: $_cwt_worktree exists but git worktree list does not" >&2
            echo "know about it. Clean it up by hand (or remove it and let" >&2
            echo "this script recreate it) before running again." >&2
            return 1
        fi

        _cwt_dirty=$(git -C "$_cwt_worktree" status --porcelain)
        if [ -n "$_cwt_dirty" ]; then
            echo "ERROR: the capture worktree has uncommitted changes -" >&2
            echo "refusing to touch it. Nothing was discarded." >&2
            echo "Path: $_cwt_worktree" >&2
            echo "$_cwt_dirty" >&2
            return 1
        fi

        echo "=== Reusing capture worktree: $_cwt_worktree ==="
        git -C "$_cwt_worktree" checkout --detach "$_cwt_ref"
    else
        echo "=== Creating capture worktree: $_cwt_worktree ==="
        mkdir -p "$(dirname "$_cwt_worktree")"
        git -C "$_cwt_repo_root" worktree add --detach "$_cwt_worktree" "$_cwt_ref"
    fi

    CAPTURE_WORKTREE_SHA=$(git -C "$_cwt_worktree" rev-parse --short HEAD)
    echo "=== Capture worktree is at $CAPTURE_WORKTREE_SHA ($(git -C "$_cwt_worktree" log -1 --format=%s)) ==="
}
