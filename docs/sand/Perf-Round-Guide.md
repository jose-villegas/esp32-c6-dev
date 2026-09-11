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

But step 1 tells you where the cost *is*, not what a change will *save*,
and its host numbers have one measured blind spot: a candidate that stops
executing work can read as zero on a laptop and win several percent on the
board. Read "The one class where a host null means nothing" below before
you drop a candidate because the host shrugged at it.

## Exact commands

Host suite (portable suites, ~35s):

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

Add `--perf-scope` to either script (or a fourth fragment,
`sdkconfig.defaults.diag_perf`, to that command) for a build carrying only
the sand frame-budget suite and its scenes — shorter, and the only way a
round has static RAM left for its own gates. Read "Scope the build first"
below before using it: its numbers do not compare with an unscoped
capture's.

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
`launcher/main/apps/sand/tools/perf_probe/` compiles the sand test suite's
own sources (`suite_sand_*.c`) with `-DDEVICE_BUILD` on a laptop, against a
link-only gfx stub and a real
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

### The one class where a host null means nothing (bd esp32c6-vk4)

**A change that stops EXECUTING work can measure zero on the host and win
several percent on the device.** Not a magnitude error - a sign error. The
0.7x-2x range above does not hold here, and this has already cost this
campaign real decisions.

Measured 2026-09-07 with a probe built for the question: 32 dummy field
tests per non-empty cell, one build executing them and one skipping them,
gated on a `volatile` so both builds emit *identical* code - 4,829
instructions with **zero** differing instruction lines across the whole
object, `sand_step` 6,208 bytes and a 768-byte frame either way - and
differ only in one `.data` initialiser. Reproduce that check with
`launcher/tools/codegen_diff.py`. Each machine's delta against its own
baseline for the same scene:

| scene | host | device | device/host |
|---|---:|---:|---:|
| water | 33.7% | 67.3% | **2.0x** |
| every-material flip | 5.0% | 32.9% | **6.6x** |

So the host understates the cost of executed instructions by 2-6.6x
relative to the surrounding simulation work. It is not that the device is
uniformly slower - a uniform slowdown cancels in that ratio.

**The cause is not the i-cache, and that is now measured rather than
argued.** The EXTMEM hit/miss counters read 89 instruction misses and zero
data misses across a 2.3 million cycle water step, with the fetch unit
active 0.89 cycles in every one - see [`Perf-Instruments.md`](Perf-Instruments.md)'s
cache-counter section. The device is simply a single-issue in-order core
against a wide superscalar out-of-order host, so 32 extra ALU ops cost ~32
cycles here and far less there through instruction-level parallelism alone.
Two earlier rounds assumed "i-cache" and one wrote it down; the growing-
const-data test that measured *exactly zero* was right for the reason it
could not confirm at the time.

**What follows from that, and it is the useful half:** on this core a pass
costs what it EXECUTES, near enough linearly. That is why removing
instructions has paid every time the removal was real (PR #81, PR #88, this
round's probe rows) and why adding them has lost every time, including the
unroll of the cross-flow scan helpers (PR #167, water +2.14%) that was read
as a footprint effect. There is no footprint effect to read.

And the ratio is **scene-specific**, so it cannot be turned into one
correction factor and applied. An earlier estimate did exactly that -
fire measures ~318x host, the every-material flip ~63x, and extrapolating
one from the other predicted 226-244 ms against an actual 60 ms, four
times too pessimistic. That is why step 6 says a budget comes from a
capture and never from a host number.

Two consequences for how you run a round:

- **A host null is not a reason to abandon a candidate that removes
  executed work.** Round 6's own ceiling test measured +1% on host; the
  device paid -7.4% for the same idea, and the round was nearly closed on
  that null before the code was written.
- **Candidates already retired on host nulls of this class are suspect
  and may be worth re-testing on device.** Named: attempt 12's per-cell
  "can this material react at all" mask, retired at -0.1%
  (Performance-Tuning-Attempts.md, "Never retry"), and round 6's ceiling
  test above.

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
its test body in the relevant `suite_sand_*.c` file and a row in
`perf_probe/probe_main.c`'s own scene table, rather than standing up a new
probe next to this one.

### Comparing two rounds

```sh
python launcher/main/apps/sand/tools/compare_reports.py <before>.md <after>.md
```

It derives the noise floor from the two control rows in the reports being
compared rather than hardcoding one, so a run that was noisier than usual
does not get read as a win.

### Count, do not time, when the question is "did the work change"

Host wall-clock timing carries a 7-15% cross-binary noise floor - two
separately-linked host binaries of the SAME source disagree by that much
before either one has changed anything. bd esp32c6-8zx hit this chasing a
sub-20% water regression: the liquid-free control moved MORE between two
builds than the effect being chased, so no amount of extra host timing
rounds could ever resolve the question. Counting the actual work instead -
calls, rows walked, blocks/cells examined, transfers - sidesteps the noise
floor entirely: two builds of byte-identical source produce byte-identical
counts, so a real change shows up as a real difference and a no-op window
shows up as exact equality, not "probably nothing."

`sand_liquid.c` itself carries none of this instrumentation - counting is
injection, not an in-tree opt-in. `sand_work_counters.h`/`.c` live as tool
assets in `tools/perf_probe/`, and `tools/perf_probe/compare_counters.py` is
the driver: point it at two refs and it `git archive`s each into a scratch
tree (never checking out over a worktree), carries the current counters and
a small standalone scene driver into both, injects the `SAND_WORK_COUNT()`
call sites into each scratch tree's own `sand_liquid.c` at verified text
anchors, builds, runs the water scene, and prints a per-counter delta table
- no device, no capture, and it bisects for free the way a timed capture
never could (~8 minutes each on device vs. two host builds). A release or
ordinary development build never sees a single increment, because the
source they compile never carries one.

```sh
python launcher/main/apps/sand/tools/perf_probe/compare_counters.py <ref> [<ref>]
```

The second ref defaults to `<ref>^` - the ordinary case is "did this one
commit change the work."

### A counter delta is a prediction about time, never a result

The section above is about *which commit changed behaviour*, and counters
answer that exactly. They do not answer *what it costs*. They count work
items; the device charges cycles, and the exchange rate between the two is
not something a count can tell you.

bd esp32c6-u2g is the worked example. Restoring cross-flow's pre-attempt-14
sweep order restored its counters to the digit - `find_shallowest` iterations
89,734 -> 47,980, transfers 8,535 -> 4,190, exactly the old figures - against
a scene where the pass decomposition had put cross-flow at 48.4% of device
cost. That was written up as the water regression recovered. The device then
gave **-2.5%**: 20,777 -> 20,250 us, about a tenth of the ~5,000 us the
regression added, and inside that row's own ~5% build-to-build spread. Both
layout controls came back byte-identical across the pair, so there was no win
hiding under noise. Those iterations are simply cheap on this chip.

Two things follow. The write-up rule: say "work halved, time unknown until
measured", and do not name a cause in an issue until a capture has priced it -
that mis-attribution sat on bd esp32c6-8zx for a day. The instrument rule:
when the question is *where the time goes*, reach for
`CONFIG_LAUNCHER_SAND_PASS_GATES` (`main/Kconfig.projbuild`) instead - one
binary, five configurations, one capture, no layout difference between
configurations to confound the comparison. Counters find the commit; the gates
price it.

### Verify both endpoints of a bisect window are actually measured

Before spending anything INSIDE a window, confirm both ends of it were
freshly measured rather than assumed. bd esp32c6-8zx picked an old bisect
endpoint on the assumption a scene's cost was still what an earlier
capture said, never re-verified it, and spent a full day attributing a
window that turned out to contain no change at all - the real regression
was in a different six-day span nobody had looked at yet.

The counters caught this before anyone thought to question the window: a
window that truly contains a regression does not produce EXACT equality
across rows walked, blocks examined, cells examined and transfers all at
once. Twelve counters landing on the same digit - not approximately equal,
identical to the digit - is the signature of a window in which nothing
happened, not evidence that "the work did not grow." Read that result as a
prompt to move an endpoint and re-run, not as a finished answer.

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

BUILD THE HOST PROBE AS PART OF THE GATE SET, not only when you need it.
No gate compiles `tools/perf_probe/`, so a change to a suite file it shares
can break it silently: adding the cache counters pulled `esp_cpu.h` into
`suite_sand_perf.c` and left the probe unbuildable on `main` for two merges
before anyone tried to run it. One line, and it fails loudly:

```sh
bash launcher/main/apps/sand/tools/perf_probe/build_probe.sh out/probe
```

RUN THE STATIC-RAM GATE TOO, whenever a round adds a scene or a suite.
Every scene costs `.bss` in the full-scope diagnostics build, and the
margin between that build and the "one real grid still fits" cliff has
been as thin as 432 bytes (beads `esp32c6-bix`). Two things make a local
reading lie: `check_static_ram.py --self-test` tests the script, not this
tree, and a `build.diag` left configured for `--perf-scope` compiles one
suite instead of all of them and reports thousands of bytes of headroom
that CI will not find. Delete the config, rebuild, read the number:

```sh
rm -f launcher/build.diag/sdkconfig
./launcher/tools/build_diag_check.sh   # "predicted largest after POST"
```

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

## Instrumenting a round

Moved to [`Perf-Instruments.md`](Perf-Instruments.md) — the gates, the
ceiling probe, the single-step technique, the control pass, the four
measurement rules, and how the scaffolding comes back out.

The short version, for a round that just needs to get going:

```sh
python scripts/add-pass-gate.py --name <phase>   # declare it
sh scripts/capture_ref.sh <ref> --perf-scope --no-restore COM3
python scripts/strip-pass-gates.py               # at the end
sh scripts/verify-gate-strip.sh <gated-ref> <stripped-ref>
```

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
  against `RUN_TEST()` in the current `suite_sand_*.c` files before
  trusting any number from it — a stale capture has cost this campaign real time more
  than once.
- **Check that an artifact exists for the SPECIFIC ref before reading any
  number out of it.** `ls -t results/` hands you the most recently
  generated file, which is not the same claim as "the file for the ref I
  am asking about" - it can be a neighbouring ref's report, and it reads
  entirely plausibly right up until a conclusion is drawn from it. This
  has cost this campaign real time twice in one session; name the ref you
  expect and confirm the artifact actually says so before trusting it.
- **The present-cost rows are far noisier than the frame-budget rows, and
  the controls do not vouch for them.** In the esp32c6-u2g pair, whose two
  frame-budget controls were byte-identical, the present-cost rows still
  swung -28.2%, -4.9% and +4.8% - on rows containing no liquid at all,
  against a one-file change to the liquid sweep. Read a present-cost delta
  as evidence only when it is large, repeated, and paired with a reason it
  should have moved. **It is not `gfx_present()`'s placement.** That was
  the standing suspect and it was measured out on 2026-09-10: five
  addresses, offsets 16/20/24/28/30 within a 32-byte block, moved the
  three rows by 1.4% at worst and by nothing at all in four of the five.
  Pinning the function narrows the spread to 0.13% and never crosses the
  0.5% floor, so no pin shipped and the mechanism behind swings like
  u2g's -28.2% is still unidentified. Start from the strip-send counts in
  the row's own log line - the rows are bus-bound at ~2,550 µs per full
  band, so a big move means the scene sent a different number of bands.
- **Size a delta against the row's own history before calling it a win.**
  Pull the same row out of the last several captures first: water has read
  20,882 / 21,093 / 21,314 / 21,942 / 20,882 us across nearby builds, a
  spread near 5%, so a 2.5% improvement on that row is not bankable from
  one pair however clean the controls look. Identical controls prove the
  layout did not shift between two builds; they do not shrink the row's
  historical spread.

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
pass owns more than 44% of it). Build a pass-decomposition map like that
one before designing anything, on any new failing scene — it has caught a
wrong-pass experiment every time it's been skipped.

Do this with the four `sand_step_gate_*` volatiles (`sand_priv.h`, bd
esp32c6-8zx), not four separately-built stub-each-pass images: one binary,
five configurations (all passes on, then each disabled in turn) in ONE
device capture, so there is zero layout difference between configurations
to confound the comparison — the failure mode four separate images cannot
avoid, since each one draws its own flash-layout ticket. Default true at
runtime (every pass on), gated behind its own Kconfig option,
`CONFIG_LAUNCHER_SAND_PASS_GATES` — opt-in on top of
`CONFIG_LAUNCHER_DEVELOPMENT`, for the same reason a plain `build.diag`
should not silently carry them: they measure TIME on device, and the counters
(injected by `compare_counters.py`, not an in-tree option) measurably perturb
codegen, so the two are kept apart to make sure measuring one never perturbs
the other. The gates cost release nothing and, with the option on, are
available in a diagnostics build.

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
