# Plan: split Diagnostics into a Settings app, then unify SELFTEST/diagnostics naming

**Status**: planned, not built. Written 2026-08-30, out of the conversation
that added `tools/build_flash_dev.sh` (see [Testing-Guide.md](Testing-Guide.md)
and `main/Kconfig.projbuild`) and noticed the seam this plan closes.

**2026-09-02 update**: step 1 below did not land as written. The maintainer's
actual motivation surfaced first - the gfx debug-overlay checkboxes need to
be reachable from a `--dev` build so they can be used while working on
`sand`, and a `--diag` build cannot stand in for that because its linked-in
test suites eat enough static RAM that `sand`'s grid allocation fails. Given
that, moving the *whole* Diagnostics app to `CONFIG_LAUNCHER_DEVELOPMENT` and
guarding only the self-test-runner bits (the button, its result line,
`selftest_run()`) behind `CONFIG_LAUNCHER_SELFTEST` was simpler than first
extracting a Settings app, and unblocked the real goal immediately. See
`launcher/main/CMakeLists.txt` (the `apps/diagnostics/` exclusion, now
keyed on `CONFIG_LAUNCHER_DEVELOPMENT`) and `launcher/main/apps/diagnostics/
app_diagnostics.c` (the `#if CONFIG_LAUNCHER_SELFTEST` guards around the
runner). That also means the factual claim in "The naming mismatch" below -
that the POST report is `CONFIG_LAUNCHER_SELFTEST`-shaped - is no longer
true; it is `CONFIG_LAUNCHER_DEVELOPMENT`-shaped like the rest of the app
now, and ships in `--dev`. The "run self test suite" button and its result
line remain genuinely SELFTEST-shaped.

Step 1 (the Settings extraction) is still open, now as a pure UI/
organisation question rather than one gating memory or build correctness -
see "What survives" below. Step 2 (the SELFTEST/diagnostics rename) is, if
anything, more pressing than when this was written: "diagnostics" now names
an app that ships in a build that is not itself called diagnostics, which is
exactly the kind of naming friction step 2 exists to remove.

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
is (as of 2026-09-02) two different screens' worth of concerns wearing one
page, gated at two different granularities:

- **The "run self test suite" button and its result line** on page 1 are
  genuinely `CONFIG_LAUNCHER_SELFTEST`-shaped - they exist only because the
  suites are compiled in, and are guarded that way in the source
  (`#if CONFIG_LAUNCHER_SELFTEST` in `app_diagnostics.c`).
- **Everything else** - page 0 (the POST report) and the rest of page 1 (the
  gfx dirty-region overlay checkbox, the gfx leaf-grid overlay checkbox, the
  interlace-mode checkbox, and the show orientation toggle with its
  accel/gravity/quarter readout) - is `CONFIG_LAUNCHER_DEVELOPMENT`-shaped by
  the project's own stated rule (Testing-Guide.md, "Development-only
  instrumentation is its own flag, not SELFTEST"): none of it needs the test
  suites, all of it is exactly "meant for someone AT the device or watching
  its serial console while working on it" - and, as of 2026-09-02, that is
  exactly the flag the whole app (not just these rows) is gated on.

The app used to be gated entirely behind `CONFIG_LAUNCHER_SELFTEST` only
because that was the only flag available when it was written, not because
"diagnostics" and "selftest" are actually the same concept. `--dev` (added
in the conversation this doc opened with) proved the two flags are
independently useful; the app has since been moved onto
`CONFIG_LAUNCHER_DEVELOPMENT` wholesale, with only the button/result-line
pair still carrying the narrower `CONFIG_LAUNCHER_SELFTEST` gate they
actually need.

## The plan

**1. Extract the DEVELOPMENT-only rows into a new Settings app.** This step
no longer changes what a `--dev` build can reach - as of 2026-09-02 the
whole Diagnostics app, POST report included, already ships there - so it is
now purely an organisation/UI question: should the gfx overlay checkboxes,
interlace toggle, and orientation readout live on their own screen instead
of as Diagnostics' second page, and does a `--dev` build want a `Settings`
entry in its app list distinct from `Diagnostics`. The "run self test
suite" button and its result line would stay behind in Diagnostics either
way; they have no meaning without the suites.

**2. Once that split lands (or is deliberately skipped), the SELFTEST/
diagnostics naming mismatch is a clean, low-risk mechanical rename**
(Kconfig symbol, app folder, build directory, CLI flags, CI workflow file,
docs). It no longer strictly needs to wait on step 1 the way it once did -
`app_diagnostics.c` today has only one small SELFTEST-shaped island (the
button + result line) rather than a whole hybrid page - but doing the
extraction first still keeps the rename mechanical rather than another
occasion to relitigate what belongs where.

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
  does today? Development-only content argues for the latter, matching how
  `main/CMakeLists.txt` already excludes `apps/diagnostics/` by folder under
  `CONFIG_LAUNCHER_DEVELOPMENT` - guarding `apps/settings/` the same way,
  under the same flag, is the obvious mirror.

## Non-goals

Nothing here is being built now. This is a placeholder for a future
session - the two-part shape and the open questions above are the
handoff, not a commitment to any answer among them yet.
