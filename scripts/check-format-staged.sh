#!/bin/sh
#
# Verifies that everything STAGED for the next commit is formatted, and says
# exactly how to fix it if not. This is the body of the pre-commit hook
# (scripts/git-hooks/pre-commit), kept as its own script so it can be run by
# hand and so CI's shell linting covers it - a file named `pre-commit` has no
# .sh extension and would be linted by nothing.
#
#   scripts/check-format-staged.sh
#
# It checks the staged CONTENT of each file, not the working copy. A file
# staged in part - `git add -p`, or a later edit after `git add` - is judged
# by what is actually about to be committed, which is the only reading that
# matches what CI will see on the pushed commit.
#
# It is feedback, not the gate. `git commit --no-verify` skips it, .git/hooks
# is not cloned so it only exists where somebody ran
# scripts/install-git-hooks.sh, and it binds neither a merge nor a commit made
# by CI. .github/workflows/format.yml is the gate; this just moves the news
# earlier.

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_ROOT"

# ACMR: added, copied, modified, renamed. A deleted file has nothing to
# format, and format-file-list.sh would drop it anyway.
staged=$(git diff --cached --name-only --diff-filter=ACMR -- '*.c' '*.h')
[ -n "$staged" ] || exit 0

files=$(printf '%s\n' "$staged" | scripts/format-file-list.sh --stdin)
[ -n "$files" ] || exit 0

# Resolved once, so a version problem is reported once rather than per file.
# check-format.sh owns finding it and refusing a wrong major.
if ! CLANG_FORMAT=$(scripts/check-format.sh --which); then
    echo "" >&2
    echo "Cannot check formatting, so nothing was verified. Fix the above, or" >&2
    echo "commit with --no-verify if you have a reason to skip the check." >&2
    exit 1
fi

offenders=""
IFS='
'
for file in $files; do
    # --assume-filename is what makes this a check of the staged blob rather
    # than the file on disk: clang-format reads the content from stdin but
    # resolves .clang-format (and the language) from the name.
    if ! git show ":$file" | "$CLANG_FORMAT" --assume-filename="$file" --dry-run --Werror >/dev/null 2>&1; then
        offenders="$offenders$file
"
    fi
done
unset IFS

[ -n "$offenders" ] || exit 0

echo "Staged changes are not formatted:" >&2
printf '%s' "$offenders" | while IFS= read -r file; do
    [ -n "$file" ] || continue
    echo "  $file" >&2
done
echo "" >&2
echo "Format them and stage the result:" >&2
printf '  scripts/check-format.sh' >&2
printf '%s' "$offenders" | while IFS= read -r file; do
    [ -n "$file" ] || continue
    printf ' %s' "$file" >&2
done
echo "" >&2
echo "  git add <those files>" >&2
echo "" >&2
echo "See docs/C-Style-Guide.md. To commit without this check: git commit --no-verify" >&2
exit 1
