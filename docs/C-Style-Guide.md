# C style guide

This guide records the style already used by the first-party firmware under
`launcher/main/`. It also applies to its host and device tests. Vendored
components and generated files keep their upstream or generator-owned form.
Self-contained apps follow the guide, but do not define its conventions or
provide its examples because they can be added or removed independently.

The comment policy and the include/comment layering rule remain in
`AGENTS.md`; this guide uses them without restating them. Cppcheck and the
MISRA addon check semantic rules through `launcher/tools/misra_check.sh`.
Their findings are not formatting or naming rules and are not part of this
guide.

The guide deliberately takes structural ideas from
[OpenBSD style(9)](https://man.openbsd.org/style.9), interface discipline from
[POSIX](https://pubs.opengroup.org/onlinepubs/9799919799/functions/V2_chap02.html),
and invariant checking from
[SQLite's use of assert](https://sqlite.org/assert.html). Their surface syntax
does not override this repository's established C: four spaces, attached
braces, pointer-left declarations, plain names, and declarations near first
use.

## Mechanical rules

These rules are decidable. `.clang-format` is their definition and
`.github/workflows/format.yml` is their enforcement. If this prose and the
formatter ever disagree, fix the prose or make a separately reviewed formatter
change; do not hand-format around the tool.

`.clang-format` lists only where this repository deviates from clang-format's
LLVM style - fifteen keys - so everything in it is a decision someone made,
and anything absent is that style's default.

- Indent with four spaces and never tabs.
- Attach opening braces and require braces around control-statement bodies.
- Bind `*` to the type in pointer declarations.
- Put a definition's return type on its own line, above the function name.
  This is the most visible thing about the style and the least guessable, so
  it is named here rather than left to the config.
- Keep code within 120 columns where clang-format can do so without changing a
  token, string, or unflowed comment.
- Use the formatter's spacing, continuation indentation, blank-line, and
  `case` indentation decisions.
- Let clang-format sort each existing include block, with system headers before
  project headers. Preserve intentional blank lines between blocks. That
  ordering is *within* a block: the own-header-first rule under "Headers and
  module boundaries" survives only because a blank line separates it from the
  standard-library block, so removing one of those blank lines changes which
  rule applies.

Run the checker on every first-party C or header file a change touches:

```sh
scripts/check-format.sh --check path/to/file.c path/to/file.h
```

Format named files only. Do not aim the formatter at the whole repository or
at vendored/generated sources:

```sh
scripts/check-format.sh path/to/file.c path/to/file.h
```

### One formatter version

clang-format 19, exactly - not "19 or newer". `.clang-format` does not define
a formatting on its own; a version of clang-format reading it does, and they
disagree about this config on real files here. Against the reformatted tree,
19 changes nothing, 20 reformats `suite_sand_scenes.c`, 21 also reformats
`material.c` and `gfx.c`. So `check-format.sh` refuses any other major
version, CI installs `clang-format==19.1.7`, and 19 is also what ESP-IDF's
esp-clang bundles - sourcing the IDF export script is usually all it takes.
`CLANG_FORMAT_ANY_VERSION=1` forces a one-off run on another version, at the
cost of reformatting files you did not touch.

### Where the rules are checked

Three places, one file list. `scripts/format-file-list.sh` defines which files
the rules apply to - vendored trees by path, generated files by the
`GENERATED FILE` marker they carry - so the hook and CI cannot disagree about
what is in scope.

```sh
scripts/install-git-hooks.sh          # opt in to the pre-commit hook
scripts/install-git-hooks.sh --status # is it active in this clone?
scripts/format-file-list.sh | xargs scripts/check-format.sh --check  # what CI runs
```

The pre-commit hook checks the *staged content* of the C and header files in a
commit, so a partially staged file is judged by what is actually being
committed. It is feedback and not a gate: `--no-verify` skips it, `.git/hooks`
is not cloned, and it never sees a merge or a commit made by CI. The workflow
is the gate, and it checks every file in the list on every push - so a drift
that reaches `main` is a failed build, not a surprise six months later.

## Judgment rules

These rules need a reader who understands the code. Do not add scripts that
guess at them.

### Names

Use lowercase `snake_case` for functions, variables, and fields. Public
functions start with their module name and then say what they do:
`display_init()`, `gfx_fill_rect()`, `imu_read()`, `touch_read()`. Keep the
prefix stable because it is the C namespace for that module.

Static functions do not need a synthetic prefix. Give them the shortest clear
verb or predicate in their file's context, such as `panel_bring_up()`. Do not
add `prv_`, type-encoded Hungarian prefixes, or another naming layer whose only
purpose is to restate linkage or type information.

Name predicates so true has an obvious meaning. `is_`, `has_`, `can_`,
`may_have_`, and `*_ready()` are established forms. Use `bool`, `true`, and
`false` for boolean state rather than integer substitutes.

Use a lowercase `_t` name for project types and uppercase names for enum
constants and macros. A private struct may have a matching tag when a forward
declaration helps; do not introduce tags or typedefs that no interface needs.
Use `TAG` for an ESP logging tag local to one translation unit.

Names describe the domain quantity, not its storage width. Add a unit suffix
when the unit is otherwise ambiguous (`dt_ms`, `now_us`, `radius_px`). Use an
`out_` prefix for output parameters when it distinguishes them from inputs.

### Integer and type discipline

Use fixed-width integer types when width or signedness is part of the value's
contract: pixels, device registers, serialized bytes, protocol fields,
persistent counters with deliberate wrap, and arithmetic that depends on a
known width. Use `size_t` for object sizes, `sizeof` results, allocation sizes,
and indexes whose contract is the size of an object.

Plain `int` is the normal type for screen coordinates, small counts, loop
indexes, enum-adjacent values, and return sentinels such as `-1`, provided the
whole documented range fits. It keeps coordinate subtraction and boundary
checks signed and avoids casts that hide underflow. Plain `unsigned` is
appropriate for bit masks, hashes, and intentional modulo arithmetic when no
specific width is required.

Convert at a boundary, then calculate in one deliberate type. Do not mix
signed and unsigned operands merely to silence a warning. Before narrowing,
make the range evident from a check, a compile-time assertion, or the source
type's contract. Prefer `sizeof object` to `sizeof(type)` when allocating or
copying that object.

Cppcheck's MISRA essential-type rules (especially the 10.x family) can demand
more explicit conversions than this house style. Treat those as semantic
findings on the affected expression; do not replace every natural `int` with a
fixed-width type as a formatting rule.

### Headers and module boundaries

Give each module one public header that owns its public types, constants, and
function declarations. A module may have a private header when several of its
`.c` files share implementation details. Do not create forwarding headers or
one-declaration headers.

Headers use `#pragma once`, are safe to include more than once, and include the
headers required to understand their own public declarations. They must not
depend on an includer's order or unrelated transitive includes. Keep
implementation-only dependencies in the `.c` file.

A `.c` file includes its own public header first when it has one. Follow it
with standard-library headers, then ESP-IDF or other external headers, then
project headers. Keep project includes layer-qualified as required by
`AGENTS.md`. Do not expose a lower layer to a higher-layer type just to avoid
passing a small value across the boundary.

Keep a module's public contract small and stable. Add an exported function
when another translation unit needs the operation, not merely to shorten the
original file. A function should perform one coherent job; split it when the
new function has a name and contract clearer than the block it replaces.

### Assertions

Use `_Static_assert` for facts the compiler can prove: encoded values, array
capacity, table stride, object size, and relationships between constants.

Use `assert()` at internal boundaries for invariants that normal callers
cannot violate: a private helper's precondition already established by its
caller, an index derived from validated state, or a state-machine relationship
whose failure means the program is internally inconsistent. Assertions must
have no side effects.

Do not use `assert()` for allocation failure, device absence, I/O failure,
untrusted input, or any condition the running product must handle. Assertions
may be compiled out, so code that needs a release behavior still needs a
simple branch and a return, fallback, or terminal action. Do not add an
assertion merely because a condition seems unlikely.

The current tree relies heavily on `_Static_assert` and has little runtime
`assert()` use. Add runtime assertions where a real internal contract benefits
from executable documentation; do not seed files with speculative assertions
to meet a count.

### Errors and failure

Return errors to the boundary that can decide what they mean. Driver-facing
helpers normally preserve `esp_err_t`. A module whose callers only need
available/unavailable may translate that to `bool` at its public boundary.
Pure lookup and geometry functions use a documented sentinel such as `false`,
`NULL`, or `-1` when absence is an ordinary result.

Log where the failure acquires operational meaning, usually once. A low-level
helper should return the error without logging if its caller can add the useful
device or operation context. Do not emit the same failure at every level of a
call chain.

For an optional peripheral or feature, log the loss and continue in the
documented degraded mode. For a required startup resource, log and stop before
entering the frame loop; on the device that can mean parking the task so the
board remains flashable. For invalid internal state, assert as described
above. Do not turn recoverable hardware conditions into assertions.

Prefer early returns for failed preconditions and completed edge cases. They
keep the main path shallow. This intentionally conflicts with MISRA C:2012 Rule
15.5's single-exit preference; record and triage that rule in the MISRA
pipeline rather than contorting house style or hiding the finding.

Keep return-code contracts small. Avoid a project-wide error enum: use
`esp_err_t` where its detail is useful, `bool` for a two-state module boundary,
and a module-specific enum only when callers genuinely make different choices
for three or more outcomes.
