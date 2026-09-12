# Review: the C style baseline

An adversarial review of the four commits that established the firmware C
style and applied it to the tree:

| Commit | Title |
| --- | --- |
| `4b502e4` | Apply the C formatting baseline (#193) |
| `48e151b` | Document the firmware C style (#191) |
| `fae4b92` | Fix runaway MISRA scan (#192) |
| `adb2028`, `39f7f1f` | Remove local agent instructions from repository |

The reformat itself is sound - the findings below are all about the
scaffolding around it. Findings are ranked; each says what is wrong, why it
matters, and what fixes it. A finding marked FIXED was addressed on this
branch.

## What was verified about the reformat

`4b502e4` touches 159 files, +15432/-17128 lines. A diff that size cannot be
read line by line, so it was checked mechanically instead.

- **Token equivalence.** Every touched file was tokenized before and after
  (comments stripped, adjacent string literals concatenated as the
  preprocessor would) and the two token streams compared. Across all 159
  files the only changes are **440 inserted braces** and **47 include
  reorderings**. No expression, literal, identifier or string content
  changed anywhere in the commit.
- **`InsertBraces` is the one option in `.clang-format` that can change
  meaning.** clang-format inserts braces without full semantic information,
  and the way it goes wrong is wrapping a preprocessor conditional so a
  different set of statements ends up inside the body. Every hunk was
  scanned for an added brace within four lines of a `#if`/`#else`/`#endif`:
  zero hits.
- **Comments are intact.** Eleven files differ in whitespace-stripped
  content but not in code tokens; the difference is macro line-continuation
  (`\`) placement only. Comment text is identical modulo internal spacing,
  which matters in a tree where the comments carry the design rationale.
- **Include reordering is safe.** The reorderings are almost all
  `"unity.h"`/`"suites.h"` swaps in test files. `launcher/test/suites.h`
  references no Unity symbol, so nothing depended on the old order.
- **Tests and builds.** The host suite passes (999 tests, 0 failures) and all
  five workflows are green on `main`, including both ESP-IDF device builds -
  which is the only evidence that covers the device-only files the host
  suite never compiles.

Five `.c` files do not include their own header first, against the rule in
the style guide: `palette.c`, `sand.c`, `sand_impulse.c`,
`suite_sand_common.c` and `suite_sand_scenes.c`. All five predate `4b502e4`;
the reformat did not cause them.

## 1. The baseline had no enforcement - FIXED

`docs/C-Style-Guide.md` states that `scripts/check-format.sh` is the
enforcement of the mechanical rules. Nothing ran it. There were five
workflows - host tests, two builds, shell scripts, comment rules - and no
format workflow, and the only hook in `scripts/hooks/` measures comment
length. A 159-file baseline whose drift nothing detects is a baseline with a
short life, and the softer rule family next door (comment length) was
already gated.

Fixed on this branch by a pre-commit hook for local feedback and a CI
workflow as the actual gate, both drawing their file set from
`scripts/format-file-list.sh` so the two cannot disagree. See "How the gate
is put together" below.

## 2. The formatter version is the real specification, and it was unpinned

`.clang-format` does not define one formatting; a *version* of clang-format
reading `.clang-format` does. Measured against the committed tree:

| clang-format | first-party files it would reformat |
| --- | --- |
| 18.1.3 | 7 |
| **19.1.7** | **0** |
| 20.1.8 | 1 |
| 21.1.2 | 3 |

So the baseline was applied with clang-format 19, and 19 is the only version
that agrees with the tree. `check-format.sh` accepted anything `>= 19` with
no upper bound, which means a contributor on 20 or 21 reformats files nobody
asked them to touch - `suite_sand_scenes.c` at 20, plus `material.c` and
`gfx.c` at 21. The divergence is not exotic: braced initializer lists inside
macros, `(uint8_t)~x` cast spacing, and a function returning a pointer to an
array.

Now pinned: CI installs exactly 19.1.7, and `check-format.sh` warns when the
local major is not 19 rather than silently accepting it.

A second, slower problem in the same file. Eleven keys in `.clang-format`
are legacy aliases that clang-format no longer echoes in `--dump-config`:
`SpacesInParentheses`, `SpacesInConditionalStatement`,
`SpacesInCStyleCastParentheses`, `SpaceInEmptyParentheses`,
`DeriveLineEnding`, `UseCRLF`, `IndentRequires`,
`ConstructorInitializerAllOnOneLineOrOnePerLine`,
`AllowAllConstructorInitializersOnNextLine`, `BreakBeforeInheritanceComma`
and `BreakConstructorInitializersBeforeComma`. They are still accepted as of
21.1, so nothing is broken today - but an unknown key in `.clang-format` is a
hard error, not a warning, so the day one of them is dropped upstream the
checker stops working entirely rather than degrading. The file is an
unedited copy of an upstream C++ config; roughly sixty of its keys describe
C++, Objective-C and JavaScript constructs this repository does not contain.
Worth a pass that deletes what does not apply and rewrites the legacy keys
to their current names.

## 3. The style guide's normative dependency was deleted

`docs/C-Style-Guide.md` defers two rules to `AGENTS.md`: the comment policy
(line 10) and the include/comment layering rule (line 125). `adb2028`
deleted `AGENTS.md` and `CLAUDE.md` - 716 lines - and `39f7f1f` gitignored
them. Both rules are now documented nowhere in the repository, while
`comment-rules.yml` still enforces them in CI and its own header still
attributes them to `CLAUDE.md`.

A fresh clone therefore has a style guide with a dangling pointer, and a CI
job whose rationale left with the file. Either fold the shared parts into
`docs/`, or restore a tracked `AGENTS.md` holding only what the guide and
the workflow refer to. The genuinely local material (session state, personal
agent definitions) belongs behind the new `.gitignore` rules either way.

## 4. The MISRA stubs silence findings in live code, not just in tables

`fae4b92` stubs two macros under `__CPPCHECK__` in
`launcher/main/apps/sand/material_palette.c` so the MISRA addon stops
exhausting itself on the palette tables. The comments justify this as being
about "these large constant tables" and "the compile-time tables", but
`#undef GFX_RGB` applies to the whole translation unit, and both stubbed
macros are also used at runtime:

- `LERP(...)` at line 889, inside the leaf/wood blend.
- `GFX_RGB(...)` at lines 890, 920 and 942 - the first function in the file
  starts at line 508, so all three are live code.

Under the scan, `GFX_RGB` becomes a bare cast. The real macro narrows a
byte-swapped RGB565 `uint32_t` to a `gfx_color_t`, which is precisely the
shape MISRA's 10.x essential-type rules exist to flag, and `LERP` becomes
`lo + hi + sh` in place of its shift-and-divide arithmetic. The blind spot
covers the hottest colour path in the app, and nothing in the report says
the scan was narrowed.

Fix: bound the stub to the table region and restore the real macros before
the first function, or move the tables into their own translation unit that
the `--file-filter` excludes. Either way, have `misra_check.sh` print a line
when the stubs are active, so a low finding count cannot be read as
coverage.

Two smaller notes on the same commit. `__CPPCHECK__` is in the
implementation's reserved identifier space - a name like `MISRA_SCAN` avoids
that, in a script whose whole purpose is conformance. And passing `-D` to
cppcheck normally restricts it to that single configuration, which would
narrow `#ifdef` coverage across every file in the scan; worth confirming,
with `--max-configs` as the lever if it holds.

## 5. `misra_check.sh` changed its contract without saying so

The header still reads "This is report-only: it always exits 0."  It now
exits 2 on a rejected file filter, a bad `MISRA_JOBS`, or a timeout, and
otherwise propagates cppcheck's status. The caller matters:
`scripts/fix-audited-code.sh` invokes it unguarded under `set -euo
pipefail`, so a timed-out scan aborts the fixer. That is probably the right
behaviour - auto-patching from a truncated report is worse - but it is
undocumented, and in `--worktree` mode the `EXIT` trap then removes the
worktree that holds the "partial report: ..." path the script just told the
user to look at.

Also worth tightening: the `*/main/*` guard is a substring match, not a
root-anchored one. A checkout whose path happens to contain `/main/` matches
every translation unit in `compile_commands.json` and reopens the 12 GB
runaway the commit exists to fix.

## 6. Smaller findings

- **The guide omits the most visible rule of the new style.**
  `AlwaysBreakAfterReturnType: AllDefinitions` puts the return type on its
  own line for every definition (`static int` then `fx_round_div(...)`). The
  mechanical rules list covers indentation, braces, pointer binding and
  column limit but never this, so nobody reading the guide would predict
  what the tree looks like.
- **The guide contradicts itself on include order.** The mechanical rules
  say system headers sort before project headers, matching
  `IncludeCategories`; the headers section says a `.c` file includes its own
  header first and standard-library headers after it. Both hold only because
  `IncludeBlocks: Preserve` keeps them in separate blocks - delete a blank
  line and the rule that applies changes silently. Worth stating outright.
- **One first-party file missed the baseline** - FIXED.
  `launcher/tools/boot_anim_render_host.c` was still in the old style (brace
  on its own line after `main`, `char **argv`, `const gfx_color_t *fb`)
  while its siblings under `launcher/tools/oracle/` conform. It was the only
  file standing between the tree and a whole-tree gate. Everything else the
  baseline skipped is correctly skipped: six generated headers and the
  vendored microui, small3dlib and Unity sources.
- **Scope creep in `48e151b`.** "Document the firmware C style" also deleted
  `.claude/agents/naive-player.md` and `.codex/agents/naive-player.toml`
  (380 lines) and reversed a deliberately argued `.gitignore` decision to
  track agent definitions. A fine decision to make; it is just invisible
  under that title.

## How the gate is put together

Four pieces, so that the same file set and the same formatter version answer
to both a commit and a push:

- `scripts/format-file-list.sh` - the single definition of which files the
  rules apply to. Excludes the vendored trees by path and generated files by
  the `GENERATED FILE` marker they already carry, so a newly generated
  header is excluded the day it appears rather than the day someone
  remembers to add it to a list.
- `scripts/check-format-staged.sh` - the pre-commit logic. Checks the
  **staged content** of each file rather than the working copy, so a
  partially staged file is judged by what is actually being committed.
- `scripts/git-hooks/pre-commit` plus `scripts/install-git-hooks.sh` - opt-in
  local feedback. `git commit --no-verify` skips it, `.git/hooks` is not
  cloned, and nothing it does binds a merge or a CI commit, which is exactly
  why it is not the gate.
- `.github/workflows/format.yml` - the gate. Installs clang-format 19.1.7
  exactly and checks every file the list names, so drift fails a push
  whether or not the author installed anything.
