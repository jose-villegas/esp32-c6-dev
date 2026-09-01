# Plan: split Diagnostics into a Settings app, then unify SELFTEST/diagnostics naming

**Status**: planned, not built. Written 2026-08-30, out of the conversation
that added `tools/build_flash_dev.sh` (see [Testing-Guide.md](Testing-Guide.md)
and `main/Kconfig.projbuild`) and noticed the seam this plan closes.

---

## The naming mismatch, and why it is not just a typo

Two vocabularies name the same flag today:

- `CONFIG_LAUNCHER_SELFTEST` (Kconfig), `boot/selftest.c`, `selftest_run()`
  — the code that actually runs the suites calls this "selftest."
- `main/apps/diagnostics/`, `app_diagnostics`, `build.diag/`,
  `build_flash.sh --diag` / `build_flash_diag.sh`,
  `sdkconfig.defaults.diag`, `.github/workflows/build-diagnostics.yml` —
  the app, the tooling, the CI workflow, and the prose docs all call this
  "diagnostics."

A straight rename (pick one word, sed it everywhere) would paper over a
real conceptual seam rather than close it. `main/apps/diagnostics/app_diagnostics.c`
today is two different screens' worth of concerns wearing one page:

- **Page 0** (the POST report) and the "run self test suite" button +
  result line on page 1 are genuinely `CONFIG_LAUNCHER_SELFTEST`-shaped -
  they exist only because the suites are compiled in.
- The rest of page 1 - the gfx dirty-region overlay checkbox, the gfx
  leaf-grid overlay checkbox, the interlace-mode checkbox, and the show
  orientation toggle with its accel/gravity/quarter readout - is
  `CONFIG_LAUNCHER_DEVELOPMENT`-shaped by the project's own stated rule
  (Testing-Guide.md, "Development-only instrumentation is its own flag,
  not SELFTEST"): none of it needs the test suites, all of it is exactly
  "meant for someone AT the device or watching its serial console while
  working on it."

The app is entirely gated behind `CONFIG_LAUNCHER_SELFTEST` today only
because that was the only flag available when it was written, not because
"diagnostics" and "selftest" are actually the same concept. `--dev`
(this session's own addition) proved the two flags are independently
useful; the app that predates it still assumes they travel together.

## The plan

**1. Extract the toggle page's `CONFIG_LAUNCHER_DEVELOPMENT`-only rows into
a new Settings app**, gated on `CONFIG_LAUNCHER_DEVELOPMENT` instead of
`CONFIG_LAUNCHER_SELFTEST` - so it ships in a `--dev` build with no test
suites at all, matching where this content already conceptually belongs.
The "run self test suite" button and its result line stay behind in
Diagnostics; they have no meaning without the suites.

**2. Once that split lands, what remains under `CONFIG_LAUNCHER_SELFTEST`
is purely selftest-shaped** - the POST report and the suite runner, nothing
development-only left riding along. At that point the SELFTEST/diagnostics
naming mismatch is a clean, low-risk mechanical rename (Kconfig symbol,
app folder, build directory, CLI flags, CI workflow file, docs) rather than
a rename that would still be glossing over a hybrid app underneath it.

Deliberately in that order: renaming first would rename a moving target,
since the deeper fix would still need to happen afterward and might not
land on the same word either way.

## Open questions to settle when this is actually picked up

- **What does Settings contain besides the migrated toggles?** Just the
  four rows verbatim at first, presumably - but "Settings" as a name
  invites more than debug overlays eventually (this project has no
  persisted user preferences of any kind yet - display orientation
  defaults, sound, etc. - worth deciding whether this app is scoped to
  developer toggles only or genuinely user-facing settings from the start).
- **Which word wins the rename**: "selftest" (already the Kconfig symbol
  and the suite-runner file name) or "diagnostics" (already the app name,
  every script, the CI workflow, and most of the prose docs)? Whichever
  loses has more surface area to touch.
- **Does `CONFIG_LAUNCHER_SELFTEST_AUTORUN` rename too**, for the same
  consistency reason.
- **CI workflow file rename** (`build-diagnostics.yml`) changes the
  workflow's badge URL - the README badges section needs updating in the
  same change, not as an afterthought.
- Should Settings be reachable from the launcher unconditionally (like any
  other app) or only exist in `--dev`/`--diag` builds the way Diagnostics
  does today? Development-only content argues for the latter, matching
  how `main/CMakeLists.txt` already excludes `apps/diagnostics/` by folder
  under `CONFIG_LAUNCHER_SELFTEST` - a `CONFIG_LAUNCHER_DEVELOPMENT` build
  guarding `apps/settings/` the same way is the obvious mirror.

## Non-goals

Nothing here is being built now. This is a placeholder for a future
session - the two-part shape and the open questions above are the
handoff, not a commitment to any answer among them yet.
