# Perf Round Guide

The entry point for a fresh session told to run a sand performance round.
Read this start to finish before doing anything. For the *record* of past
rounds see [`Performance-Tuning-Attempts.md`](Performance-Tuning-Attempts.md)
(never-retry list, recurring failure modes) and
[`Tuning-At-a-Glance.md`](Tuning-At-a-Glance.md) (scoreboard, pass-ownership
map). This page is instructions, not narrative.

## The loop

0. **Start every round from a capture you took yourself.** Do not open the
   newest `.md` in `results/` and read its numbers as current, and do not
   carry a previous round's table forward as the baseline. Those files are
   an archive, not a state - they are timestamped, never overwritten, and
   several of them were generated from captures that turned out to have
   measured nothing. This has already gone wrong twice: once a stale report
   was quoted as a fresh result because it was simply the most recent file
   on disk, and once a whole round's budgets were read from a capture whose
   fixtures had all failed to allocate. A capture is cheap next to a wrong
   conclusion drawn from an old one - and if a fresh capture and an archived
   table disagree, the fresh capture wins every time.

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

### One command from a ref to a device verdict

Evaluating a candidate branch used to mean: create or enter a worktree,
detach it at the ref, sit through a cold build, run `report_performance.sh`
by hand, and read four commands' worth of capture output yourself.
`scripts/capture_ref.sh` does all of that as one call, against one
persistent worktree reused across every ref you evaluate in a sitting so
the build stays warm (`.claude/capture-worktree/` by default; see the
script's own top comment for exactly where and why):

```sh
sh scripts/capture_ref.sh some-branch --baseline <report>.md
sh scripts/capture_ref.sh some-branch --build-only        # build only, never flashes
```

`--build-only` builds `build.diag` in the capture worktree and stops before
any flash or capture — useful on its own to check a candidate even LINKS
(an `aligned(64)` candidate once failed only at link time) before spending
a device round on it. `--baseline` and `--no-restore` are passed straight
through to `report_performance.sh`'s own flags, described above — this
script does not re-validate, re-summarize, or re-derive a verdict; it only
turns a ref into a warm checkout that script can run against.

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

| Free heap | What you get | |
|---|---|---|
| 66,632 B | every scene allocates | measured 2026-08-28 |
| 63,952 B | every scene allocates | measured 2026-09-01 |
| 42,880 B | all 34 fixture tests fail to allocate; zero timings | measured 2026-09-01 |
| 28,480 B | same, worse | measured 2026-08-31 |

One grid is ~41 KB on its own. If a round suddenly reports nothing, suspect
a static test fixture added since the last good capture before suspecting
the device - that has now been the cause twice.

The host suite now allocates from an arena this size (`test/heap_arena.c`,
fed from the device profile) and gates test-code stack frames against the
3,584-byte stack, so both failures are supposed to be caught on a laptop
before a capture is ever spent on them - see the Testing Guide's "The host
runner enforces two of the device's limits". Supposed to: the arena starts
from a clean process, so it does not model fragmentation inherited from the
rest of a real boot, and `bd esp32c6-e82`'s device allocation failures do
not reproduce on it. Still check the free-heap line in the capture.

**A healthy suite is SLOW, and that is the correct sign.** While the
fixtures were failing to allocate, the whole device run finished in ~168
seconds, because failing a malloc is instant. With the heap freed and every
scene actually simulating, a full pass measured 1,125,726 ms - 18.8
minutes. Both report scripts capture with a 1500-second window for this
reason. If a run finishes in a couple of minutes, that is not a fast
device: it is a suite that measured nothing, and the validator will say so.

### Attributing on the host, before touching the device

Step 1's "host counters first" has a harness already:
`launcher/main/apps/sand/tools/perf_probe/` compiles suite_sand.c itself
with `-DDEVICE_BUILD` on a laptop, against a link-only gfx stub and a real
`esp_timer_get_time()`, and calls the actual frame-budget test bodies
through the `SAND_HOST_PROBE` wrapper functions beside them - not a
hand-copied scene, so it can't drift from what the device build measures.
It also compiles with the device's own codegen-shaping flags, taken from
the device profile, so a switch cannot become a jump table here that the
board could never have had. On this tree that changes nothing - no
portable sand source emits an indirect jump either way - but it removes
the failure mode rather than leaving it to be rediscovered.

Read the magnitudes it gives you with the record in mind: host numbers
call *direction* reliably for changes of code shape, and land at 0.7x-2x
for changes of work quantity. The factor neither this harness nor any x86
host can see is flash placement against the 32 KB instruction cache. The
route to that number, once a QEMU with TCG plugins exists, is sketched and
half-built in `launcher/tools/oracle/`.

```sh
bash launcher/main/apps/sand/tools/perf_probe/build_probe.sh out/probe
out/probe --list                      # every scene this build knows
python launcher/main/apps/sand/tools/perf_probe/run_probe.py out/probe \
    --n 10 water mixed_flip lava_stress   # interleaved best-of-N, min/median
```

This is the one host harness - do not build another one. Two per-round
copies of this already accumulated in this tree days apart (bd
esp32c6-o2s) before being merged back into this single directory; if a
scene you need isn't in `--list`, add a `SAND_HOST_PROBE` wrapper next to
its test body in suite_sand.c and a row in `perf_probe/probe_main.c`'s own
scene table, rather than standing up a new probe next to this one.

### Comparing two rounds

```sh
python launcher/main/apps/sand/tools/compare_reports.py <before>.md <after>.md
```

It derives the noise floor from the two control rows in the reports being
compared rather than hardcoding one, so a run that was noisier than usual
does not get read as a win.

### Unattended candidate evaluation

`scripts/perf-loop.sh` evaluates optimisation candidates without a human,
and is built so a candidate cannot be accepted for the wrong reason:

```sh
sh scripts/perf-loop.sh --baseline <report>.md --candidates <file>
sh scripts/perf-loop.sh --host-only --candidate "sed -i ... sand.c"
sh scripts/perf-loop.sh --baseline <report>.md --candidate-ref some-branch
```

A candidate is normally a shell command applied to the working tree in
place. `--candidate-ref` instead names a git ref, checked out into
`capture_ref.sh`'s own persistent worktree and run through the same five
gates below - gate A adapted to diff the ref against its merge-base rather
than read `git status --porcelain` (which reads clean for a committed ref
regardless of what it touches), so a ref cannot buy a pass by rewriting a
budget or a scene any more than a sed candidate can. See `scripts/perf-
loop.sh`'s own "REF CANDIDATES" comment for the full reasoning.

Five gates, cheapest first - allowlist, host suite, fingerprint, device
capture, measured verdict - then one of three outcomes: ACCEPT (won, and
behaviour byte-identical, committed to a branch), QUARANTINE (won, but
behaviour changed - patch kept for review), REJECT.

The allowlist runs FIRST and matters most. The cheapest way to make a
deliberately-failing budget pass is to raise the budget, and the next
cheapest is to weaken the scene; both live in files a candidate may not
open, so neither is discouraged - both are unreachable.

`main/apps/sand/tools/report_fingerprint.sh --check` is the behavioural
gate and is worth running by hand during any perf round. It hashes the
grid after a fixed number of steps across five scenes and prints the
per-material histogram beside each hash. Proven necessary: setting
`SAND_VENT_LAYER` from 3 to 5 passes all 680 tests and changes the
simulation - the suite cannot see it, this does. Read a failure by the
histogram, not the hash: identical counts with a different hash is a
reordering, changed counts mean material was created or destroyed.

`--update` re-records the baseline and is deliberately a human act. A loop
that can re-record its own baseline has no baseline.

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

- **The dispatcher rung of the pair-matrix is unshipped** — the loop+switch
  shape is in the never-retry list; a different shape, plus the ordering
  sweep that waits on it, lives on the `sand-pair-matrix` branch
  (`bd esp32c6-iu5`).
- **Water's remaining gap (−44%) is call volume, not code shape** —
  attempt 19's counters and null closed the layout line; the next water
  idea has to reduce the double touch (~11k grains × sweep + equalise per
  step), a mechanism-class change.

Resolved items are deleted from this list rather than struck through —
their record lives in the attempt table and `git log`.
