# Perf Round Guide

The entry point for a fresh session told to run a sand performance round.
Read this start to finish before doing anything. For the *record* of past
rounds see [`Performance-Tuning-Attempts.md`](Performance-Tuning-Attempts.md)
(never-retry list, recurring failure modes) and
[`Tuning-At-a-Glance.md`](Tuning-At-a-Glance.md) (scoreboard, pass-ownership
map). This page is instructions, not narrative.

## The loop

1. **Attribute before optimising.** Host counters first: which pass, which
   function, does the suspect code even run in the failing benchmark's
   window (`git log --all --grep attempt` for prior instances of this
   mistake — three device rounds have been burned by skipping this step).
   Measure-by-deleting for a first number; `objdump -t` if inlining is a
   candidate.
2. Design against the sharpest existing test for the mechanism you're
   touching, not the average case.
3. Implement.
4. Host suite green (`bash launcher/test/run_tests.sh`).
5. Device capture.
6. Peg or re-peg budgets from what the capture actually measured — never
   from a host number, never from a guess.

Never skip step 1. Every round in this campaign that designed a fix before
counting spent at least one device cycle on code the failing test never
called or work that was already cheap to fall through.

## Exact commands

Host suite (portable suites, <1s):

```sh
bash launcher/test/run_tests.sh
```

Device capture (build + flash `build.diag`, capture, validate, generate the
report):

```sh
bash launcher/main/apps/sand/tools/report_performance.sh [COM_PORT] [OUT.md]
```

This script now removes a stale `build.diag/sdkconfig` before building —
`idf.py` only applies `SDKCONFIG_DEFAULTS` when it *creates* the sdkconfig,
so a leftover one from a previous build silently wins and the fragments
below never take effect — and asserts `CONFIG_LAUNCHER_SELFTEST` /
`CONFIG_LAUNCHER_SELFTEST_AUTORUN` actually landed in the generated config
before it spends five minutes capturing an image with no self-test in it.

A device-only build in your own build directory (so you don't fight another
session for `build.diag`), same three sdkconfig fragments the script uses:

```sh
idf.py -B build.diag.<yours> \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.diag;sdkconfig.defaults.diag_autorun" \
  -D SDKCONFIG=build.diag.<yours>/sdkconfig build
```

Reports land in `launcher/main/apps/sand/tools/results/` — both the
generated `.md` table and the raw serial capture (`*_raw.txt`) beside it.
Read the raw capture, not just the table, when a row looks wrong; the table
generator can only report what it was pointed at.

### The capture must be validated before it is read

`report_performance.sh` runs `launcher/tools/sweeps/validate_capture.py`
between the capture and the report, and refuses to generate a table from a
bad capture. Run it by hand on any raw file you did not capture yourself:

```sh
python launcher/tools/sweeps/validate_capture.py <capture>_raw.txt
```

It catches the three failures that have each cost a full cycle here: a run
that never finished, a crash loop (more than one boot banner), and - the
expensive one - a capture that completes cleanly while measuring nothing.
`python launcher/tools/sweeps/validate_capture.py --selftest` checks the
validator itself against known-good and known-bad captures in `results/`.

**Free heap is a precondition, not a detail.** Every frame-budget scene
mallocs its grid, so when the heap is short the suite still runs, still
prints `SELFTEST_COMPLETE`, and still produces a report-shaped capture -
with no timings in it at all. Grep the capture for
`free heap after framebuffer` before reading anything else:

| Free heap | What you get |
|---|---|
| ~66,600 B | every scene allocates; measurements are real |
| ~42,900 B | all 34 fixture-based tests fail to allocate; zero timings |

One grid is ~41 KB on its own. If a round suddenly reports nothing, suspect
a static test fixture added since the last good capture before suspecting
the device - that has now been the cause twice.

### Comparing two rounds

```sh
python launcher/main/apps/sand/tools/compare_reports.py <before>.md <after>.md
```

It derives the noise floor from the two control rows in the reports being
compared rather than hardcoding one, so a run that was noisier than usual
does not get read as a win.

## Reading a capture

- **Check the controls first.** The two liquid-free control benchmarks
  (full-size step, settled-pile flip) land on one of exactly two quantised
  value-pairs across builds, never in between (six observations, six
  landings so far). Reading which pair they landed in separates a real
  regression from the ordinary flash-layout lottery before you look at
  anything else.
- **Confirm `SELFTEST_COMPLETE` is present.** No headline line means the
  run is worthless regardless of what the table shows — check for a
  timeout, a crash, or an image that compiled the suites in but never ran
  them (`CONFIG_LAUNCHER_SELFTEST_AUTORUN` unset).
- **Test output that stops mid-suite means a crash, not a timeout.** A
  `Stack protection fault` or an allocation failure aborts the run where it
  stands; the capture script's own 300s timeout is generous enough that
  reaching it cleanly is itself informative (something hung, not something
  slow).
- **A capture is a measurement of a tree, not a project.** If you didn't
  just build the image yourself, check the self-test names in the raw log
  against `RUN_TEST()` in the current `suite_sand.c` before trusting any
  number from it — a stale capture has cost this campaign real time more
  than once.

## Budget rules

- Every sand frame-budget test is a **deliberately-failing reduction
  target**: `measured × 0.9`, rounded, from the first clean capture after
  the row was last touched. Failing means the work isn't done yet, not
  that something broke.
- **One exception:** the wet-earth scene's budget is specified as
  `measured × 0.8` (a 20% target), by explicit instruction from the person
  who pegged it — not this file's usual convention. Do not "correct" it to
  ×0.9 for consistency.
- A row that is genuinely bus-bound (dominated by hardware, not logic) gets
  a tight regression guard instead of a reduction target — say which row
  and why in the comment above it, the way the existing budgets do.
- **Never raise a budget.** If a feature genuinely earns a higher cost,
  that is a re-peg from a fresh measurement, decided deliberately and
  recorded as such — not a quiet loosening to make a red row green.
- A newly-passing budget gets **re-pegged**, not left at its old number
  banked as slack for later.

## House rules

- A new test must be seen to **fail first** on the pre-fix code. A test
  that has never been red might be asserting nothing.
- Free every allocation before asserting in a fixture — an
  assert-before-free skips earlier frees on failure and leaks for the rest
  of that boot, taking every later test down with it.
- No Co-Authored-By trailer on commits in this repo.
- Plain, unadorned commit messages, in the repo's own voice — read
  `git log --oneline` for the tone before writing one.
- Other sessions work in this tree concurrently. `git add` only the files
  you actually changed — never `-A`, never a blanket `.`.
- The main/interactive session owns the physical hardware. Do not flash
  unless you are the session explicitly asked to; building and running the
  host suite is always fine.

## Where the leads are

Pass ownership (who to blame first for a given scene) is mapped in
[`Tuning-At-a-Glance.md`](Tuning-At-a-Glance.md)'s "Which pass owns which
scene" table: water and the mixed flip are the cross-flow pass; thermal
shock and the boiler are the reactions pass; the three gas-heavy scenes
are the gas pass; the every-material flip is genuinely diffuse (no single
pass owns more than 44% of it). Build a four-way stub-each-pass map like
that one before designing anything, on any new failing scene — it is four
builds and it has caught a wrong-pass experiment every time it's been
skipped.

Open items, as of this file's writing:

- **The wet-earth scene's budget is unpegged.** It ships with a
  deliberately loose 300,000 µs provisional ceiling
  (`test_the_wet_earth_scene_fits_in_the_frame_budget`, `suite_sand.c`) and
  awaits its first device capture. Re-peg at `measured × 0.8` per the rule
  above, not ×0.9.
- **Five blast-scene tests currently fail on device with allocation
  errors** — the device has ~66–68 KB free after the framebuffer and one
  grid alone is ~41 KB; several blast/dune/vessel fixtures allocate a grid
  plus an impulse buffer plus (for some scenes) a one-bit-per-cell mask on
  top of that. Tracked as `bd esp32c6-e9t`, which also proposes making the
  *host* enforce the device's stack and heap limits so this class of bug
  is a one-second host failure instead of a wasted capture cycle. Recent
  captures in `launcher/main/apps/sand/tools/results/` (2026-08-31 onward)
  show this has widened past the original five — `heap free` has been
  measured as low as ~28 KB on device, below what
  `test_framebuffer_fits_with_headroom_to_spare` itself requires. Check the
  current free-heap number before adding any new device fixture; this is
  likely the most valuable single fix available right now, not a new
  optimisation.
- **Attempt 15's "finding A" residual (~267–269 µs on the two liquid-free
  controls) is unbisected** between two commits that both touch
  `sand_step`'s compiled object — `30bfba7` ("Grow the plant, and fix the
  two things that stopped it") and `0dac86a` ("Let trees lean with the
  tilt, and fix the fan that made them jump"). Neither commit's source
  touches anything either control benchmark executes; the residual is
  believed to be the same "same instructions, different schedule" effect
  attempt 15 found for `e03aabd`, just not run to ground. A host bisect
  the way attempt 16 did it (compile `suite_sand.c` itself against shims,
  not a hand-copied scene) is the cheap way to close it.
