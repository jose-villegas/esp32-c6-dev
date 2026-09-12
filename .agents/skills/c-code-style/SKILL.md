---
name: c-code-style
description: Format C/C++ (.c/.h) files with this repo's clang-format config via scripts/check-format.sh. Trigger on "format C code", "run clang-format", "check clang-format formatting".
---

# C code formatting

This repo has no house C style guide — the rule is the existing one: match
the surrounding code in whatever file you're editing rather than applying an
external convention. This skill only wires up the formatter, on request.

## Formatting

`scripts/check-format.sh` runs `clang-format` against the repo's
`.clang-format` (auto-discovered by walking up from the target file, no path
needed):

```sh
scripts/check-format.sh <file.c> [<file.h> ...]        # format in place
scripts/check-format.sh --check <file.c> [<file.h> ...] # verify only, exits 1 if not compliant
```

It looks for `clang-format` on `PATH` first, then falls back to the one
bundled with the ESP-IDF toolchain (`esp-clang`, version 19) if the IDF
environment hasn't been sourced in this shell. If neither is found it prints
an install command and exits.

## When to run it

Only on files you just wrote or edited, and only on request or for genuinely
new files — do not run this over pre-existing files. `.clang-format` here
does not match this codebase's current formatting in several places (e.g.
hand-aligned struct-literal tables), so running it over existing files
produces large, unrelated reformatting diffs.
