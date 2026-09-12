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

A second, slower problem in the same file - FIXED. `.clang-format` was an
unedited copy of an upstream C++ config: 196 lines, 136 keys, of which
fifteen were legacy aliases clang-format 19 no longer echoes in
`--dump-config` (`AlwaysBreakAfterReturnType`, `SpacesInParentheses`,
`DeriveLineEnding`, `UseCRLF`, `IndentRequires`,
`KeepEmptyLinesAtTheStartOfBlocks` and the rest), and dozens more described
C++, Objective-C, Java and JavaScript constructs this repository does not
contain. Nothing was broken by it today, since 21.1 still accepts every one
of those spellings - but an unknown key in `.clang-format` is a hard error
rather than a warning, so the day one is dropped upstream the checker stops
working entirely instead of degrading.

Comparing the effective config against a plain LLVM one showed what was
actually being decided: **15 keys**. The file now states those and nothing
else, as deviations from `BasedOnStyle: LLVM`, in 67 lines with a sentence
of rationale per group. Equivalence was checked three ways rather than
assumed: `--dump-config` before and after differs only in
`BreakTemplateDeclarations` (C++ templates, of which this tree has none) and
the order of a macro set; all 170 files remain byte-identical under
clang-format 19; and both 19.1.2 (esp-clang) and 19.1.7 (what CI installs)
resolve the new file to the same effective config.

A file of deviations does lean harder on one version's defaults - which is
an argument for the pin above, not against the cleanup, since the pin is
what a version bump has to confront anyway.

## 3. Rules referred to but not written down anywhere - FIXED

Keeping `AGENTS.md`, `CLAUDE.md`, `.claude/` and `.codex/` out of the
repository is deliberate: they are personal, and `adb2028` plus `39f7f1f`
remove and ignore them on purpose. Restoring them is not the fix, and this
finding is not an argument to.

What is left behind is two dangling pointers. `docs/C-Style-Guide.md` defers
the comment policy (line 10) and the include/comment layering rule (line 125)
to `AGENTS.md`, and `comment-rules.yml`'s own header attributes its rules to
`CLAUDE.md`. Both rules are still enforced - `check_comment_length.py`,
`check_comment_symbols.py` and `check_comment_layers.py` run on every push -
so a contributor can fail CI on a rule that the repository describes only by
pointing at a file that is not in it.

Two pointers was the count in the style guide. Grepping the tracked tree
found **22 citations across 14 files** - C sources, shell scripts, Python
tools and four docs - all naming `CLAUDE.md` as the authority for a rule:
the generated-sources convention, the `app_*.c` naming split, "watch it fail
before it passes", "pass time in", the DEVICE_BUILD guard, the formatting
section, the first comment rule.

Every one of those rules already had a home in `docs/`, so the work was
repointing rather than writing: generated sources and the `app_*.c` split to
`docs/Launcher-Architecture.md`, the testing rules to
`docs/Testing-Guide.md`, formatting and comments to this guide. The comment
policy itself and the comment half of the layering rule were the two with no
home at all, and are now a **Comments** section in
`docs/C-Style-Guide.md`, written from what the three checkers actually
enforce rather than from the deleted file. `comment-rules.yml`'s header
cites the guide instead of `CLAUDE.md`. Nothing personal moved into the
tree: a policy three CI jobs enforce on every push is not personal, it was
just unwritten.

One citation was not merely dangling but false. `scripts/write-test-local.sh`
justified formatting only its own inserted line ranges by quoting "do not
reformat pre-existing files, .clang-format disagrees with the current style
in places" - true when it was written, untrue since #193 made the tree
match. The mechanism is still right (a tool should touch only what it wrote)
so it keeps it, with the real reason and the old one marked as history.

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

FIXED, and measured rather than argued. Scanning this one translation unit
three ways, with cppcheck 2.21 and the MISRA addon:

| | wall time | result |
| --- | --- | --- |
| stubs off entirely | >300 s | killed by the deadline, empty report |
| stubs as committed | 11 s | 283 findings, runtime path analysed as stubs |
| stub bounded to the tables | 67 s | 289 findings, runtime path analysed as written |

So the stubs are genuinely load-bearing - removing them is not an option -
but they only need to cover the tables. `GFX_RGB` is no longer touched at
all: the expansion that exhausts the addon is `LERP`'s channel arithmetic,
and with that stubbed `GFX_RGB(cheap)` costs nothing. `LERP` now goes back to
the real `LERP_RGB` after the last table, so every function is analysed as
written.

Comparing the two finding sets by source text rather than line number (the
edit shifts every line), what comes back is exactly the runtime colour path
the stub was hiding: 10.1 and 12.2 on both
`out[0] = GFX_RGB(LERP8(...))` blends, 10.1 on the leaf `LERP` at the top of
that branch, and 10.1/10.7/12.2 on glass's
`base = edge ? GLASS_EDGE_RGB(v) : GLASS_RGB(v)`. The cost is honest and
small: one `#undef` traded for another, so rule 20.5 is a wash.

`misra_check.sh` now also names every file it analysed with stubs, so a
finding count cannot be mistaken for coverage.

Two smaller notes on the same commit. `__CPPCHECK__` is in the
implementation's reserved identifier space - a name like `MISRA_SCAN` avoids
that, in a script whose whole purpose is conformance. And passing `-D` to
cppcheck normally restricts it to that single configuration, which would
narrow `#ifdef` coverage across every file in the scan; worth confirming,
with `--max-configs` as the lever if it holds.

While fixing #4, a second silent gap in the same script - now also fixed.
`material_palette.c` was split out of `material.c` on 2026-09-07, after every
build directory in this checkout was generated, so it appears in none of
their `compile_commands.json` files. cppcheck only errors when a filter
matches *nothing*, so the sand scan happily analysed the other ten
translation units and reported a number, with the file the MISRA commit was
patching silently absent. The script now prints how many translation units
the filter actually matched and, for a small set, names them - the sand scan
lists ten files and `material_palette.c` is visibly not among them.

## 5. `misra_check.sh` changed its contract without saying so

The header comment has been corrected as part of #4; the finding is kept for
the record. It read "This is report-only: it always exits 0."  It now
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
