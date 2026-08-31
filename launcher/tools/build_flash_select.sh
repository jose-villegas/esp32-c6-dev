#!/usr/bin/env bash
#
# Interactive front end for build_flash.sh: pick the build VARIANT
# (release/dev/diag) and the SOURCE (this checkout, an existing worktree,
# or any branch by name - auto-creating a worktree for it if none exists
# yet) from two menus, then run that source's own build_flash.sh.
#
# Usage:
#   tools/build_flash_select.sh
#
# No flags - this exists specifically to replace typing them out, and to
# replace the manual "which worktree was that branch in again, or do I need
# to add one" step that came before typing them out. Everything it asks is
# answered interactively; there is nothing to script around it with.
#
# WHY THIS EXISTS ON TOP OF build_flash.sh RATHER THAN INSIDE IT
#
# build_flash.sh already takes --dev/--diag and a COM port, and deliberately
# has no notion of "which checkout" - it always builds and flashes whatever
# is sitting in the directory it lives in, which is exactly right for the
# common case (you are already where you want to be) and says nothing about
# the other one. The friction this script exists to remove is specifically
# *getting to* the right checkout: several branches worth of work live in
# sibling worktrees under .claude/worktrees/, gained and abandoned across
# sessions, and finding "is claude/foo-bar already checked out somewhere,
# and if not, where do I put it" by hand before every flash is the part
# that doesn't belong inside a build script that has no reason to know
# about git worktrees at all. This one does exactly that, then hands off to
# the target's own build_flash.sh unchanged - it is a router, not a
# reimplementation.
#
# NEW WORKTREES GO UNDER THE PRIMARY CHECKOUT'S .claude/worktrees/, NOT
# WHEREVER THIS SCRIPT HAPPENS TO BE RUNNING FROM
#
# `git rev-parse --path-format=absolute --git-common-dir` resolves to the
# same shared .git directory no matter which worktree asks - that is what
# makes git worktrees one repository rather than several - so its parent is
# always the ONE primary checkout, and that is where a freshly created
# worktree is placed, matching every existing entry in `git worktree list`
# today. A worktree created next to whichever copy of this script happened
# to be invoked would scatter new ones across sibling checkouts depending on
# where you started, which defeats the point of having one place to look.
#
# NAMING A NEW WORKTREE
#
# `git worktree list` today shows plenty of existing worktrees whose
# directory name has nothing to do with the branch inside them - each one
# was named after whatever session created it, not after the branch it
# ended up holding, and those names cannot be reconstructed from the branch
# alone. This script does not try to imitate that; a new worktree it
# creates is named after the branch itself (its `claude/` prefix dropped as
# noise, remaining slashes flattened to dashes), which is predictable
# rather than decorative - see sanitize_branch_for_dirname() below.

set -euo pipefail

THIS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Printing the menu to stderr, not stdout, is what lets a caller capture
# the chosen VALUE with plain command substitution - `x=$(choose_from_list
# ...)` - without stdout also carrying the numbered list and the prompt
# text. Loops on invalid input rather than failing outright: a mistyped
# number here should cost one re-prompt, not the whole script under
# `set -euo pipefail` above.
choose_from_list() {
    local prompt=$1
    shift
    local choices=("$@")
    local input choice_index

    while true; do
        printf '%s\n' "$prompt" >&2
        for i in "${!choices[@]}"; do
            printf '  %2d) %s\n' "$((i + 1))" "${choices[$i]}" >&2
        done
        printf '> ' >&2
        read -r input

        if [[ "$input" =~ ^[0-9]+$ ]]; then
            choice_index=$((input - 1))
            if [ "$choice_index" -ge 0 ] && [ "$choice_index" -lt "${#choices[@]}" ]; then
                printf '%s\n' "${choices[$choice_index]}"
                return 0
            fi
        fi
        printf 'not a valid choice: %s\n' "$input" >&2
    done
}

# claude/foo-bar -> foo-bar; a/b/c -> a-b-c. Only used for a worktree this
# script itself creates - an existing worktree's own directory name (found
# via `git worktree list`, below) is never touched or reinterpreted.
sanitize_branch_for_dirname() {
    local name="${1#claude/}"
    printf '%s' "${name//\//-}"
}

VARIANT_LABEL=$(choose_from_list "Which build?" "release" "dev" "diag")
case "$VARIANT_LABEL" in
    release) VARIANT_FLAG="" ;;
    dev)     VARIANT_FLAG="--dev" ;;
    diag)    VARIANT_FLAG="--diag" ;;
esac

# The repo root this copy of the script lives in - two levels up from
# launcher/tools - not necessarily the PRIMARY checkout (see the top
# comment on where new worktrees are placed either way).
HERE_ROOT="$(cd "$THIS_DIR/../.." && pwd)"
HERE_BRANCH="$(git -C "$HERE_ROOT" branch --show-current 2>/dev/null || true)"
HERE_LABEL="Here - $HERE_ROOT${HERE_BRANCH:+ [$HERE_BRANCH]}"

# One porcelain pass over every worktree the shared repo knows about,
# turned into two parallel arrays (path/branch) rather than an associative
# array - bash 3.2 (macOS's shipped /bin/bash) has no associative arrays,
# and this project's other tools/*.sh files stay compatible with it.
WT_PATHS=()
WT_BRANCHES=()
current_path=""
while IFS= read -r line; do
    case "$line" in
        "worktree "*) current_path="${line#worktree }" ;;
        "branch "*)
            WT_PATHS+=("$current_path")
            WT_BRANCHES+=("${line#branch refs/heads/}")
            ;;
        "detached")
            WT_PATHS+=("$current_path")
            WT_BRANCHES+=("(detached HEAD)")
            ;;
    esac
done < <(git -C "$HERE_ROOT" worktree list --porcelain)

MENU_LABELS=("$HERE_LABEL")
MENU_PATHS=("$HERE_ROOT")
for i in "${!WT_PATHS[@]}"; do
    # HERE_ROOT already has its own entry above - skip it here rather than
    # listing the current checkout twice under two different labels.
    [ "${WT_PATHS[$i]}" = "$HERE_ROOT" ] && continue
    MENU_LABELS+=("${WT_BRANCHES[$i]} - ${WT_PATHS[$i]}")
    MENU_PATHS+=("${WT_PATHS[$i]}")
done
MENU_LABELS+=("Other branch (type a name; a worktree is created if none exists)")

SOURCE_LABEL=$(choose_from_list "Which checkout?" "${MENU_LABELS[@]}")

if [ "$SOURCE_LABEL" = "Other branch (type a name; a worktree is created if none exists)" ]; then
    printf 'Branch name: ' >&2
    read -r BRANCH
    [ -n "$BRANCH" ] || { echo "no branch name given" >&2; exit 1; }

    TARGET_ROOT=""
    for i in "${!WT_BRANCHES[@]}"; do
        [ "${WT_BRANCHES[$i]}" = "$BRANCH" ] && TARGET_ROOT="${WT_PATHS[$i]}"
    done

    if [ -z "$TARGET_ROOT" ]; then
        MAIN_ROOT="$(dirname "$(git -C "$HERE_ROOT" rev-parse --path-format=absolute --git-common-dir)")"
        TARGET_ROOT="$MAIN_ROOT/.claude/worktrees/$(sanitize_branch_for_dirname "$BRANCH")"

        if [ -e "$TARGET_ROOT" ]; then
            echo "refusing to reuse $TARGET_ROOT - it exists but git worktree" >&2
            echo "list does not know about it; clean it up by hand first" >&2
            exit 1
        fi

        if git -C "$HERE_ROOT" show-ref --verify --quiet "refs/heads/$BRANCH"; then
            git -C "$HERE_ROOT" worktree add "$TARGET_ROOT" "$BRANCH"
        elif git -C "$HERE_ROOT" show-ref --verify --quiet "refs/remotes/origin/$BRANCH"; then
            git -C "$HERE_ROOT" worktree add -b "$BRANCH" "$TARGET_ROOT" "origin/$BRANCH"
        else
            echo "no such branch '$BRANCH' locally or on origin" >&2
            exit 1
        fi
    fi
else
    for i in "${!MENU_LABELS[@]}"; do
        [ "${MENU_LABELS[$i]}" = "$SOURCE_LABEL" ] && TARGET_ROOT="${MENU_PATHS[$i]}"
    done
fi

TARGET_BUILD_FLASH="$TARGET_ROOT/launcher/tools/build_flash.sh"
if [ ! -f "$TARGET_BUILD_FLASH" ]; then
    echo "$TARGET_BUILD_FLASH does not exist - this branch predates" >&2
    echo "build_flash.sh, or its worktree is missing launcher/ entirely" >&2
    exit 1
fi

read -r -p "Serial port [COM3]: " PORT
PORT="${PORT:-COM3}"

echo "=== $VARIANT_LABEL from $TARGET_ROOT, on $PORT ==="
# shellcheck disable=SC2086
"$TARGET_BUILD_FLASH" $VARIANT_FLAG "$PORT"
