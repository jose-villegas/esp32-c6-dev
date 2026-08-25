# Performance Tuning Attempts

Part of the falling-sand app's own documentation folder - see
[`README.md`](README.md) for the full set.

Continues directly from [`Simulation-Lessons.md`](Simulation-Lessons.md):
that file is the discovery narrative for how the simulation was
originally built and shipped; this one picks up once it already worked,
and is the sequential record of every real-hardware performance attempt
made against it since - numbered where the attempts themselves were
numbered at the time, so the numbering here is historical, not a table
of contents. Split out on its own once the combined file passed 800
lines and the two halves stopped being one story: build-time correctness
lessons in one file, a chronological tuning campaign in the other.

---

## A guard chain can cost more than what it guards

Continuing directly from the section above (same two failing benchmarks,
same tests, no new scenario). The proposal on the table was amortising
neighbour-wake checking across frames - skip it on alternating steps, the
same shape of idea as row-sleeping itself. Before designing that, the
same "measure by deleting" discipline used throughout this file was
applied to the proposal's own premise first: stub `wake_blocks_points()`
into an unconditional no-op (`TEMPORARY EXPERIMENT`, reverted after
reading the number) and see whether neighbour-wake checking was even a
meaningful share of the 15733/22789 us total. It was - more than half:
flip dropped to 8153 us, water to under 16000 us, just from removing the
function entirely. Isolating further (stub only the
`point_reach()`/bounding-box-clear half, keep everything else) showed
that was NOT where the removed time went - only ~1.85 ms of flip's drop
and ~0.88 ms of water's came from that half. The rest was the ten-
condition "is this the common, nothing-to-do case" guard chain sitting
above it, paid on **every single grain move**, whether or not the move
ever turned out to need a neighbour woken.

That guard chain computes four block indices by dividing `x`/`y` by
`SAND_BLOCK_W`/`SAND_BLOCK_H` - both compile-time-constant powers of two,
and both plain `int`. The same bug as the division lesson in
[Optimization-Playbook.md](../Notes/Optimization-Playbook.md): the values are
always non-negative grid coordinates in practice, but the compiler cannot
prove that from a plain `int` parameter, so it falls back to signed
division's round-toward-zero correction sequence instead of a shift. An
unsigned cast at the one call site (`wake_blocks_points()`) recovered
~2.7-3.1 ms on its own, no correctness change - real, isolated, and
committed on its own. The same bug, same fix, was sitting a second place
too - `equalise_one_row()`'s own `wake_blocks_range()` call, dividing the
same way - fixed alongside it for consistency, though its blast radius is
far smaller (once per row, not once per grain).

**The frame-split idea itself turned out to have a real correctness
trap**, found before any code was written for it, not after:
`wake_blocks_points()` only ever fires when a grain actually moves - it
is event-driven, not polled. A "skip" frame that lets a boundary-crossing
move go unchecked has no guaranteed retry, because the grain that made
that move very often settles immediately and never moves again. Skip the
check on exactly the step where that happens, and the neighbour that
should have woken never learns it needs to - not delayed by a frame,
*stuck asleep indefinitely* - which is precisely the failure
`test_undermining_a_sleeping_pile_collapses_it` exists to catch. A safe
version needs real "guaranteed eventual delivery" machinery (a pending-
recheck list that survives even if the triggering grain never moves
again), which is real complexity for an unproven win - banked rather than
built.

**The safer alternative tried instead, and why it lost anyway.** Rather
than deferring checks across frames, batch several checks within the
*same* step: accumulate the block-index bounding box of every grain move
within one block's row-sweep, and issue ONE `wake_blocks_range()` call at
the end instead of paying the guard chain once per grain - exactly the
pattern `equalise_one_row()` already uses for liquid cross-flow. This is
provably safe in the sense that matters most: `wake_blocks_range()`
always expands its given range by one block in every direction,
unconditionally, so the union of several moves' ranges is a strict
superset of what checking each one precisely would have woken. It cannot
leave anything incorrectly asleep - only, possibly, wake a neighbour that
did not strictly need it. That "only, possibly" turned out to be the
whole story: measured on device, it made every number worse, not better
- flip 13053 us -> 17254 us, water 19703 us -> 20332 us, and it even
regressed `test_a_full_size_step_fits_in_the_frame_budget`, a benchmark
that had been passing. The coarse, unconditional block-neighbourhood
expansion was re-examining settled neighbours often enough to cost more
than the guard-chain evaluations it removed - the exact same
coarse-skip-structure failure mode the original row-to-block rewrite
exists to fix, just relocated from "the tracking unit is too coarse" to
"the wake radius is too coarse." Reverted in full; only the unrelated
division fix survived from that attempt.

**The lesson worth keeping separate from every inlining/division fix
above:** those were all *safe by construction and fast by measurement* -
free wins, no tradeoff to weigh. This one was reasoned to be safe (a
monotonic superset can't under-wake) and still lost, because safety and
speed are different axes. A change that can only be *correct* is not
automatically a change that is *fast* - the reasoning that rules out one
failure mode (stuck-asleep grains) says nothing about the other
(needless re-examination), and only measuring both settles it. Current
committed numbers: flip 13053 us (still over an 8000 us budget), water
19562-19703 us (still over 16000 us). The frame-split-with-a-real-safety-
net idea is still open and unbuilt if anyone picks it back up; the
batched-range approach above is a dead end at this block size and should
not be re-tried without a new idea for why it would come out differently.

**A third attempt, tried next because it looked strictly safer than
both the temporal idea and the batching one: eliminate the division
entirely, rather than cheapen it or batch it.** `step_one_block()` is
called once per `(row, block-column)` pair, so its own block indices are
loop-known constants, not something that needs re-deriving from raw x/y
by division at all - and a grain moves at most one cell per step, so the
destination's block only ever differs from the source's when the source
was already sitting on that block's edge in the direction taken, a
comparison against already-known local coordinates rather than a fresh
derivation. Threaded the sweep's own `bx`/`by` down to a new
`mark_move_in_block()`, sharing the exact same guard-chain/`point_reach()`
logic as before via an extracted `wake_blocks_core()` - nothing about
*what* got checked changed, only how the inputs were computed. No
coarsening, no superset argument needed at all: this should have been
strictly safer than the batching attempt.

It measured *worse* on real hardware anyway - flip 13053 us -> 14479-
14480 us, water 19703 us -> 21889 us, and it regressed
`test_a_full_size_step_fits_in_the_frame_budget` from passing to failing
at ~8097-8098 us, consistent across two separate device runs. The first
of those runs also appeared to crash the board (a `FAIL` message cut off
mid-line with no newline, followed by a fresh `ESP-ROM:` boot banner) -
serious enough to stop and investigate properly rather than push through.
Three targeted host reproductions (grains driven into every real block
edge at the true 184x224 screen size - a genuine coverage gap this
investigation found: every prior host test used grid dimensions that are
exact multiples of the block size, so the two partial edge blocks the
real screen actually has, at the bottom and right, had never been
exercised by anything) with active bounds assertions on every block index
this path could produce found nothing wrong. **The crash did not
reproduce on a second, identical device run either** - which means it
was never actually confirmed as a bug in this code at all, only
correlated with it once.

**The real lesson of this attempt was about the diagnostic tooling, not
the block-index math.** The first capture had been piped through
`grep -E "FAILED|FAIL:"` to keep the output short - which silently
discarded exactly the lines that would have explained what happened,
including (found on retry) `task_wdt: Task watchdog got triggered`
warnings from the new host-reproduction tests themselves running too
long on-device (250 `sand_step()` calls on a full-checkerboard 184x224
grid, comfortably over the 5-second watchdog window at this
implementation's per-step cost - fixed separately by cutting the test
down to 43 steps, plenty to still touch every edge). **Filtering a
diagnostic capture to "just the part I think I need" can discard the
one line that would have explained the failure** - the same principle as
not reasoning about where the time goes instead of measuring it, applied
to reading crash output instead of profiling numbers. The full,
unfiltered capture is what eventually showed a normal completion with no
crash at all on the second run, which is what settled the question this
attempt could not settle on its own.

Reverted in full a second time - two confirmed-worse device measurements
is reason enough regardless of the unresolved crash. What survived: the
three new host tests exercising the real, non-block-multiple screen size
(a permanent, useful regression guard independent of this specific
attempt), and the tooling lesson above.

## The fourth attempt: stop pushing notifications, start pulling them

Two failed attempts at cheapening the *push*-based mechanism - a mover
telling its neighbour something happened, once per grain move - both
touched the same hot path (`wake_blocks_points()`, called from inside
the sweep) and both regressed. That is the real signal worth reading:
not "this specific fix was wrong" but "this specific *path* has proven
harder to safely modify than it looks, twice in a row." The fourth
attempt does not try a third variant of it. It removes the push
entirely and answers the same question a different way.

`sand_step()` already runs an O(block_count) finalisation pass at the
very end of every step - only 48 blocks at the real screen size (12x4,
at the 16x64 block dimensions settled on earlier; 644 was the 8x8
starting size from the original tuning pass, not the shipped one), a
small, fixed cost regardless of how many grains moved. It decides which
blocks earn the settled bit by checking `BLOCK_ACTIVE`, a bit
`step_one_block()` already sets directly whenever anything moves inside
a block, independent of any wake call. That pass was the natural,
already-existing, already-cheap place to ask a *pull*-based question
instead: for every block that was not itself active, did any of its up
to 8 neighbours become active this step? If so, it does not get to
settle either. `any_neighbor_active()` (`sand_priv.h`) is that check -
a plain, unconditional 8-direction bit test, nothing like the
edge-position-aware, occupancy-gated `point_reach()`/`reach_corner()`/
`should_wake_neighbor()` machinery the earlier per-move mechanism needed
to stay cheap. That entire problem does not exist here: the cost is
already bounded to once per block per step, not once per grain move, so
there is nothing to gain by narrowing what gets checked.

This is not a coarser version of the thing that failed twice - it is a
different mechanism, on a different pass, that the sweep's hot path
never touches at all. A grain only ever moves one cell, so a
destination block, if different from its source, is always that
source's immediate neighbour - which means a sweep-internal move needs
*no* wake call of its own any more: `moved_here` marks the source
active, and `any_neighbor_active()` picks that up for the destination on
its own the moment the finalisation pass runs. The only place still
needing an explicit wake is a touch from *outside* the sweep - `sand_set()`/
`sand_erase()`/`try_spawn_one()`, and liquid's cross-flow/rebound passes -
where there is no `moved_here` for the pull-based check to observe.
Those get a small, unconditional 3x3-block expansion
(`wake_block_and_neighbors()`) instead of the old edge-aware one - safe
for the same reason the batching attempt's superset was safe, but
without that attempt's cost, since these calls are user-interaction/
cross-flow rate, not once per grain move.

**A real correctness trap found while designing this, not after
shipping it:** the first draft only expanded the touched block itself
for external touches, reasoning that the pull-based check would handle
propagation to neighbours "the same way it does for the sweep." It does
not - `sand_erase()` undermining a grain resting in a *neighbouring*
block leaves that neighbour with no `moved_here` activity of its own to
observe (nothing there moved; the cell that disappeared was in the
*other* block), so it would stay asleep forever, silently reintroducing
the exact bug `test_undermining_a_sleeping_pile_collapses_it` exists to
catch. Caught by re-deriving the design from that test's own scenario
before writing code, not by the test itself failing - worth remembering
that the tests catching a regression and the reasoning catching it in
advance are not the same safety net, and having both mattered here.

**This won, cleanly, on the first attempt** - flip 13053us -> 9638us
(-26%), water 19562-19703us -> 16540us (-15%, now within ~500us of its
budget), full-occupancy 6727us (comfortably passing). The only cost:
settled-screen 319us -> 334us, since a motionless screen now pays one
8-neighbour check per block every step instead of nothing - a small,
expected trade (already flagged as the scenario most exposed to this
change's own O(block_count) cost before it was ever measured), not a
surprise. A large amount of code came out at the same time -
`wake_blocks_points()`, `wake_blocks_core()`, `point_reach()`,
`reach_axis()`, `reach_corner()`, `should_wake_neighbor()`,
`cell_occupied()` are all gone, along with every register-spilling/
inlining concern that machinery needed - not just a smaller cost, a
smaller amount of code to have a cost at all.

**Why this one worked where two similar-sounding ideas did not:**
both earlier attempts stayed on the same O(grain_count) path and tried
to make the SAME per-move check cheaper - by batching several into one
call, or by removing its division. Both changes were entangled with
whatever made that path expensive in the first place (in one case
understood - over-waking; in one case never fully diagnosed). This one
did not try to make the expensive thing cheaper at all - it made the
expensive thing *disappear*, by noticing the question it was answering
could be answered somewhere else, already cheap, for free. When a hot
path resists being optimised in place across multiple honest attempts,
that resistance is itself information: it may be evidence the path
should not exist in that shape at all, not that the next attempt needs
to be cleverer.

## The fifth attempt: reshaping when work happens, not how much

With the pull-based mechanism in place, two more diagnostic bypasses
(both temporary, both reverted, correctness-unsafe by design) confirmed
the remaining flip/water cost was genuine physics, not overhead: the
burial-depth friction loop (`sand_load_above()`) and the neighbour-wake
propagation `any_neighbor_active()` correctly triggers both turned out
to be necessary work, not incidental cost - bypassing either broke
something real (a settled pile that never re-settles; an avalanche that
never reaches its neighbours) rather than just running faster.

That left reshaping *when* the unavoidable work happens rather than
reducing how much of it there is. `compute_settled_bit()`'s full reset
on a jostle or gravity-direction change makes every block eligible for
a full walk on the exact same step - almost certainly the single most
expensive step in the whole flip/water measurement. The idea: stagger
that release across a few steps (a new `BLOCK_STAGGER_HOLD` bit, released
one block-row at a time, bounded and unconditional so nothing could be
stranded - the settled-bit clear itself stayed atomic and immediate,
only *walking* the newly-eligible blocks was delayed). Correctly
recognised going in that this could not move the existing average-based
frame-budget tests (same total work, just spread across more of a fixed
step count) - the actual target was the single worst step, which needed
new instrumentation to even see.

**It measured as a real, if mixed, win on the synthetic device tests** -
flip's average dropped 9638us -> 8343us and its worst step (9333us) came
in *under* the old average; water's average dropped slightly (16540us ->
16135us) but its worst step (17662us) went *up*. The real problem showed
up on `test_a_screen_of_settled_sand_costs_almost_nothing`: 334us -> 1307us,
a 4x regression on a test whose entire job is catching exactly this
class of mistake. A host-side diagnostic (reproducing the same scenario
fast, without a device flash) confirmed there was no logic bug - the
stagger released cleanly, exactly on schedule, fully draining in three
steps and staying drained. The mechanism worked exactly as designed. The
design itself was the problem: releasing a whole block-row at once (12
blocks, ~12,300 cells at the real screen size) means re-examining that
much *densely packed, maximally-buried* sand even when nothing there
will ever move - and per this project's very first documented lesson
([`Simulation-Lessons.md`](Simulation-Lessons.md)), that is the single
most expensive category of cell to examine. Three such releases, even
spread thin across fifty measured steps, were enough to blow a 300us
budget by 4x.

**A device got genuinely unresponsive around this same point in the
session** - the sand app would not reliably open, and boot grew visibly
slower. Reverted immediately and reflashed `build.release` to get a
working device back before investigating further, which was the right
call given the severity, but the *cause* turned out not to be what it
looked like in the moment. The suspect at the time was live accelerometer
noise flipping the nearest gravity direction almost every step, each
flip re-triggering a fresh staggered release before the previous one
finished draining - a real risk in principle, since `build.diag`'s
self-test suite had also grown to 14+ seconds around the same time
(unrelated to staggering - just more tests accumulating across the
session) and was the far more likely actual explanation for "boot takes
a while." Reflashing the staggering build as `build.release` afterward
(no self-test, direct interactive use, gravity driven by the real
accelerometer exactly as feared) did **not** reproduce anything close to
unresponsive - it worked, with the expected staggering artifact plainly
visible (a block-row visibly floating for a frame before catching up)
and, if anything, a subjective impression of slightly more stable
framing. The accelerometer-thrash theory was never confirmed; the boot-
time theory was never conclusively ruled in either. Worth being honest
about: a scary symptom got a fast, correct-priority response (revert
first, investigate after), and the investigation that followed did not
actually pin down what caused it.

**The real, live-tested verdict is a partial win, not a clean one.**
Flip's worst step (9333us) beating the *old average* (9638us) held up
under direct interactive testing - a genuine improvement to the worst
case you would ever hit. Water did not improve - its worst step (17662us)
stayed worse than both the old and new average, so staggering's own
target metric moved the wrong way there. Settled-screen's regression
(334us -> 1307us) stands with no offsetting benefit, since a static
screen never had a worst-step problem for staggering to fix. Net
assessment: worth it only if flip specifically is what matters more than
water and idle-cost together - not worth it as a blanket change. Kept on
a branch (`sand-block-row-stagger` off the commit before this section)
rather than merged, exactly for that reason - `main` stays on the
pull-based mechanism without staggering.

**The lesson worth keeping is a repeat of the batching attempt's own
lesson, in a new shape:** a change reasoned to be *correct* (bounded,
provably cannot strand a grain, fully verified with a fast host-side
diagnostic before ever touching the device) is still not the same as a
change reasoned to be *worth it* - the coarse release granularity cost
more, on a test built specifically to catch that class of regression,
than the peak-flattening on the one scenario it did help was worth. A
second, smaller lesson sits alongside it: **when a scary symptom appears
mid-investigation, fixing it fast is the right call even before the
cause is understood - but say so plainly once the cause turns out to be
unconfirmed, rather than letting the first plausible theory harden into
the record as settled fact.**

**A natural follow-up question, asked directly: why release whole
block-rows at all, when the simulation already has finer-grained
spatial structure (individual blocks) to work with?** Fair challenge -
the row grouping was chosen for being the simplest first version, not
because it was reasoned to be the right granularity. Worth trying the
finer-grained alternative properly rather than leaving the question
unanswered.

`hold_non_leading_blocks()`/`release_next_stagger_blocks()` replaced the
row-based pair, releasing a small, tunable batch of blocks
(`STAGGER_BLOCKS_PER_STEP`) per step in `block_state`'s own linear index
order instead of by row. Two batch sizes were measured on real
hardware, alongside the original row-based number (12 blocks/step,
which drains the real screen's 48 blocks in ~3 steps):

| Variant | Settled-screen avg | Flip avg | Flip worst | Water avg | Water worst |
|---|---|---|---|---|---|
| No staggering | 334us (pass) | - | avg 9638us | - | avg 16540us |
| Row-based, 12/step (~3-step drain) | 1307us | 8343us | 9333us (win) | 16135us | 17662us (loss) |
| Block-based, 4/step (~11-step drain) | 1566us (worse) | 6327us | 9733us | 15395us | 16357us (win) |
| Block-based, 8/step (~6-step drain) | 1430us | 8128us (**new failure**, budget 8000us) | 9735us | 15940us | 16411us (win) |

None of the three is a clean win. The intuition going in - smaller
batches should mean less work re-examined per step, so a lighter touch
- turned out to only be half the picture. The other half is *drain
time*: fewer blocks released per step means more steps until every
block is released, and the settled-screen test's fixed-length averaging
window catches proportionally more of that "still recovering from the
last stagger" tail the slower the drain is. That is exactly why 4/step
(11-step drain) is *worse* than 12/step (3-step drain) on settled-screen,
not better, even though each individual release batch is a third the
size. Going the other way (8/step) partly clawed that back but broke
something new instead - flip's *average*, not just its worst step, blew
its own budget outright, a failure that did not exist in either other
variant. The two costs (per-step work vs. total steps to drain) trade
off against each other in a way that does not resolve monotonically by
turning one knob in either direction on this hardware.

**Verdict: dropped.** No variant beats the plain no-staggering baseline
across the board, and the three measured points don't even form a
clean frontier to pick a "best" one from - each wins on some axis and
loses on another, differently each time. Further tuning of this one
knob is unlikely to find a clean win without a genuinely different
release strategy (e.g. one that accounts for drain time and per-step
cost jointly, or picks blocks to release based on how expensive they
actually are rather than fixed linear order) - not attempted, since the
underlying tension had already shown up clearly enough across three
data points to call it. `main` stays on the pull-based mechanism
without any staggering; `sand-block-row-stagger` keeps all three
variants for reference, unmerged.

---

## The sixth attempt: the block size itself was still just a guess

The staggering work above answered "why release whole block-rows, when
the simulation already has finer-grained block structure to work with"
with a finer release granularity - which didn't pay off. But that
question had a second half nobody had asked yet: `SAND_BLOCK_W`/
`SAND_BLOCK_H` (16x64, giving 48 blocks at the real screen size) had
never been measured at all. It shipped as a guess, and every mass-wake
cost this whole investigation had been chasing - the `memset()` in
`compute_settled_bit()`, the `any_neighbor_active()` walk - scales
directly with block count. Fewer, bigger blocks halve that count for
free; the question was always whether re-examining a bigger block once
it wakes costs back what the smaller count saves.

**Automating the sweep paid for itself immediately.** A single
PowerShell script edits the two `#define`s, runs the host suite as a
gate, builds and flashes `build.diag`, resets the device via an RTS
pulse (not `esptool`'s own reset - holding the serial port open
throughout means no gap where early boot output gets missed), captures
the self-test output, and restores everything in a `finally` block
regardless of outcome. Three real bugs in the script itself before it
ran cleanly, all instructive: `bash` isn't on `PATH` outside an
interactive Git Bash session (needs `--login` to source the profile
that puts `coreutils` there, or `dirname`/`cd` inside `run_tests.sh`
fail with no useful error); `$hostOut -notmatch "pattern"` against a
multi-line array return from a native command filters *elements*, not
a boolean - a classic PowerShell trap that made every variant "fail"
the host-test gate even at the unmodified baseline, until piped through
`Out-String` first; and `Set-Content -Encoding utf8` in Windows
PowerShell 5.1 writes a BOM, silently polluting every future `git diff`
of a file it touches - `[System.IO.File]::WriteAllText` with an
explicit no-BOM `UTF8Encoding` was the fix. None of these were sand
bugs, but each one would have produced a confidently wrong sweep result
if it had gone unnoticed. This automation is now checked-in repo
tooling - `launcher/tools/sweeps/` - not a one-off script.

**Two real, device-only bugs turned up along the way, neither a
performance question.** `test_two_separate_active_spots_in_the_same_
block_row_do_not_wake_each_other` declared its comparison buffer as a
*stack* array sized `SAND_BLOCK_W * LOC_H` - 2 KB at the shipped
`SAND_BLOCK_W` (16), unremarkable, but every other buffer in that same
fixture was already `malloc`'d, for a reason this one had quietly
stopped honouring. At `SAND_BLOCK_W=32` the array alone is 4 KB -
bigger than the device's entire 3.5 KB main task stack
(`CONFIG_ESP_MAIN_TASK_STACK_SIZE`) - and the device panic-looped with
a real `Stack protection fault`, symbolised straight to the exact line
via `riscv32-esp-elf-addr2line` against the crashing build's own
`.elf`. A host run cannot reproduce this at all - the host stack is
megabytes - which is exactly why sweeping on real hardware, not just
the host suite, is what this whole project has insisted on throughout.
Fixed by heap-allocating it like everything else in that fixture.

Separately, two *test fixtures* - not the simulation - turned out to
hardcode geometry tied to the shipped block size rather than deriving
it: `pool_fixture()`'s water source was a fixed-height column
calibrated only for `SAND_BLOCK_W=16`, and failed to cross a
`SAND_BLOCK_W=32` pool (twice as wide) even with sleeping disabled
entirely - confirmed via a quick host-side experiment that this was
insufficient water mass hitting the liquid engine's own integer-
diffusion limit (`SAND_LIQUID_SIGHT`), not a wake bug. `loc_fixture()`'s
`LOC_H`/`LOC_W` cap was a flat 128, but `test_a_block_wakes_when_
disturbed_diagonally()` needs `SAND_BLOCK_H + 2` rows of room to place
its boxing-in stones - past `SAND_BLOCK_H~42` the flat cap silently
clipped that room away, and `sand_set()`'s silent no-op on an
out-of-range write (safe - confirmed no memory corruption either way)
meant the stones simply never landed. Both fixed to scale with the
tunable they exist to help tune, rather than the one value that
happened to ship.

**The final sweep, six `(SAND_BLOCK_W, SAND_BLOCK_H)` pairs, real
hardware, both bugs fixed:**

| Pair | Blocks | Settled avg | Flip avg | Water avg |
|---|---|---|---|---|
| 8x32 | 161 | 610us | 9186us | 16374us |
| 16x32 | 84 | 375us | 8869us | 16055us |
| 8x64 | 92 | 528us | 9950us | 21642us |
| 16x64 (old default) | 48 | 333us | 9638us | 16540us |
| **32x64 (new default)** | **24** | **234us** | **9209us** | **16238us** |
| 32x128 | 12 | 215us | 9658us | 16888us |

32x64 is the only pair that clears the 300us settled-screen budget at
all, and it beats the old 16x64 default on every metric - the halved
block count (24 vs 48) roughly halves the mass-wake `memset()` cost,
and re-examining a bigger block once woken did not cost back what that
saved, at least not at this block size.

**Worth being precise about the comparison that actually decided it,
because the first pass through these numbers overstated it:** 32x64
does *not* win outright against every alternative. 16x32 beats it on
flip (8869us vs 9209us, ~4%) and on water (16055us vs 16238us, ~1%) -
both real margins, just smaller than they first looked next to
settled-screen's gap. What tipped the choice to 32x64 was that it is
the *only* pair clearing its budget at all, on the specific case (a
motionless, fully-settled screen) this project's very first documented
lesson exists for - not that it dominated every number. A fair
alternative reading of the same table could reasonably pick 16x32
instead; this was a judgement call between "decisive win on the one
metric currently failing outright" and "small wins on two metrics that
already pass," not a computed optimum.

**Shipped as the new default**, with the full table and its trade-offs
recorded directly in `sand.h`'s own comment above `SAND_BLOCK_W`/
`SAND_BLOCK_H`, not just here - the next person tuning this constant
should not have to reconstruct this sweep from a doc file to know what
was already tried.

**A related sweep, prepared and run:** the same "was this tunable ever
actually measured" question applies to `gfx_dirty.h`'s
`LEAF_REFINE_MAX_RUNS` and `row_runs.h`'s `ROW_MAX_RUNS` (both 2), and
`GATHER_MAX_PIXELS` (8192) - flagged in
[Display-and-Rendering.md](../Notes/Display-and-Rendering.md)'s "still
untapped" list as unmeasured, or measured once but never swept the
systematic way this section describes. The same class of hardcoded-
boundary-test bug turned up there too
(`test_find_gives_up_past_the_cap`,
`test_plan_run_rejects_a_split_over_the_gather_budget`) and was fixed
the same way, alongside a new device test giving `LEAF_REFINE_MAX_RUNS`'s
fallback path a real number to compare against. Both caps confirmed
correct as shipped once actually measured - see
[Display-and-Rendering.md](../Notes/Display-and-Rendering.md)'s "The cap
sweeps" section for the full numbers.

---

## The seventh attempt: a fourth material's inlining cost

Adding gas (`KIND_GAS`, `sand_gas.c` - see
[`Sand-Simulation.md`](Sand-Simulation.md)'s own section on it and
[`Adding-a-Material.md`](Adding-a-Material.md) for the practical
walkthrough) reused the existing `try_fall_or_scatter()`/`try_slide()`
primitives, direction-inverted, rather than writing new movement logic.
Getting the *sharing* of that code right - not the movement logic
itself, which worked correctly on the first try - took three attempts,
each measured on device, each wrong in a different way. Neither
regression this uncovered had anything to do with gas actually moving:
both were flash-footprint effects, visible on frame-budget tests that
never place a single gas cell.

**First: just remove `static`.** `try_fall_or_scatter()`/`try_slide()`
were `static` in `sand.c`, inlined into `step_one_grain()`'s per-grain
dispatch by the compiler as a matter of course. Removing `static` so
`sand_gas.c` could call them directly regressed the flip/water frame-
budget tests by ~26%, exactly reproducible across captures - turning a
`static` function called once per grain into an ordinary `extern` one
was enough, on its own, to stop the compiler inlining it into the call
site that had been relying on that.

**Second: move the whole call graph to `static inline` in a header.**
Fixes the first regression - both `sand.c` and `sand_gas.c` get their
own independently inlinable copy, the exact pattern this header already
used for `dest_row()`/`mark_rows()`. But `sand_step_gas()` is a rare,
mostly-early-returning call (most steps place no gas at all), not a
per-grain hot path - and giving it a full inlined copy of
`try_fall_or_scatter()`/`try_slide()`'s entire call graph grew it to
~3.9 KB, checked directly via `riscv32-esp-elf-nm --size-sort` against
the built `.elf` - bigger than `sand_step()` itself. That alone
regressed `test_a_full_size_step_fits_in_the_frame_budget` (a worst-case,
sleeping-disabled test) from ~7000us to over 15000us, exactly
reproducible again, and again without that test ever placing a gas
cell. Flash footprint, not runtime gas activity, was the cost both
times - the instruction cache is a shared, finite resource, and a
function's own compiled size at every call site it gets inlined into is
part of that budget, not just its execution time.

**Third, what shipped: split the inline version from the callable one.**
`try_fall_or_scatter_impl()`/`try_slide_impl()` stay `static inline` in
the header - `step_one_grain()` calls them directly, keeping the
original hot path fully inlined, exactly as before either regression.
`sand_gas.c` instead calls ordinary, non-inline `try_fall_or_scatter()`/
`try_slide()` wrapper functions, defined once in `sand.c`, each just
forwarding to the matching `_impl()` - real linkage, one genuine
function call from the gas pass, and the shared logic exists in flash
as at most two copies (the fully-inlined one in `sand_step()`, and the
one real out-of-line copy `sand_gas.c` calls into) rather than a third
one duplicated inside `sand_step_gas()` itself. Verified directly: the
same `nm --size-sort` check showed `sand_step_gas()` shrink from ~3.9 KB
to ~2.4 KB, and both previously-regressed tests returned to their
pre-gas numbers - `full_size_step` from over 15000us back to ~6450us
(passing again), flip/water within a few percent of their own pre-gas
baselines (a small, honest residual cost from `sand_step()` now calling
`sand_step_gas()` at all - see below - not a new deterministic
regression).

**A fourth, smaller pass: skip the call outright when there is nothing
to move.** `sand_step_liquids()` has always guarded its own body with
`if (!s->may_have_liquid) return;` - cheap, but still pays for
marshalling every argument at the call site on every step, forever.
`sand_step_gas()`'s call site in `sand_step()` was changed to check
`s->may_have_gas` *before* calling at all, a deliberate small asymmetry
from the liquid pattern, specifically because this call site is reached
on every step of every test in the suite. Measured: it did not move the
numbers - flip/water's small residual cost turned out to already be
flash-layout noise, not the call overhead itself, confirmed by this
change having no measurable effect. Kept anyway: correct, free, and
documented as a deliberate choice rather than an unexplained
inconsistency with `sand_step_liquids()`'s own pattern.

**The general lesson, worth carrying to the next material or the next
hot-path function that needs a second caller:** inlining a function into
one call site being necessary does not mean inlining it into every call
site is free. Measure whether the *new* call site is hot enough to need
its own inlined copy before giving it one - an ordinary function call
across translation units is often the right default for anything that
is not itself in the innermost hot loop, even when the code it is
calling absolutely needs to be inlined somewhere else.

---

## The eighth attempt: three good ideas aimed at code the failing test never runs

This one is almost entirely negative results, and the reason they are
worth writing down at length is that all three of them failed the *same
way*, for a reason nobody checked until the third one had already been
measured. The campaign opened with a genuine discovery, spent three
device rounds acting on it, and only then found out the discovery was
about a code path the failing benchmark does not execute at all.

**Step 0: the phase split, re-taken.** Per-phase `esp_timer_get_time()`
accumulators inside `sand_step()`, summed across the twenty measured
steps and printed once at the end so the timer calls themselves stay a
rounding error (they cost under 70 us of the totals below - the
uninstrumented numbers are 8996/15144/16141). Temporary, `#ifdef`-gated
to device builds, reverted afterward. All three failing tests, at the
commit that added the mixed-scene test:

| Test | Total | wake | **sweep** | **liquid** | gas | react | settle |
|---|---:|---:|---:|---:|---:|---:|---:|
| settled-pile flip | 8931 | 2 | **8912 (99%)** | 1 | 1 | 1 | 7 |
| mixed sand/water/stone-X flip | 15146 | 2 | **11322 (74%)** | **3804 (25%)** | 1 | 1 | 8 |
| screen of water | 16212 | 2 | **11311 (69%)** | **4882 (30%)** | 1 | 1 | 9 |

The mixed scene's split had never been taken before. It lands almost
exactly between the other two, which is what it was designed to do: a
sweep-dominated flip with a water half bolted on. Everything outside the
sweep and the liquid equalise pass is single-digit microseconds in every
scenario - the mass-wake `memset()`, the gas pass, the reactions pass and
the settling finalisation are all, together, under 20 us. **Whatever is
wrong is in the sweep, in all three cases.**

**The discovery: `try_slide_impl()` had silently stopped being inlined.**
`objdump -t` on `sand.c.obj` showed it compiled as a real, standalone
1022-byte function (`.text.try_slide_impl`), entered through a 96-byte
stack frame - the exact register-spill signature that
[Optimization-Playbook.md](../Notes/Optimization-Playbook.md) documents
as having been worth nearly 2 ms the last time it turned up here. Worse,
it directly contradicted `sand_priv.h`'s own comment, written during the
seventh attempt above, promising that the main sweep's hot path stayed
"fully inlined, exactly as it always was."

**Why it un-inlined, which is the part worth carrying forward.** The
seventh attempt's own fix caused it. Splitting the inline `_impl()` from
the callable wrapper gave `try_slide_impl()` a *second* caller inside the
same translation unit - `try_slide()`, the ordinary function `sand_gas.c`
calls. A `static` function with exactly one call site gets inlined by
GCC unconditionally (`-finline-functions-called-once`), no size heuristic
consulted. Add a second call site and that guarantee evaporates, leaving
only the ordinary heuristics, which decline a body this large. The
compiler then emitted one shared out-of-line copy and turned the wrapper
into a 26-byte tail-jump into it. `try_fall_or_scatter_impl()`, given the
identical treatment in the identical commit, has a smaller body and was
still being inlined at both sites - which is exactly why nothing looked
wrong. **The seventh attempt verified its fix by measuring the tests it
had regressed, and they did come back; it never re-ran `objdump -t` to
check the inlining it claimed. Half the claim was true.**

**Experiment 1: force it back inline.** `__attribute__((always_inline))`,
the mechanism the playbook documents. Verified the way the problem was
found: the standalone `try_slide_impl` symbol is gone, `sand_step()` grew
2750 -> 3990 bytes, `try_slide()` grew 26 -> 1068 as it took its own
copy, net +1260 bytes of flash. Full device suite, two captures,
byte-identical to each other:

| | baseline | always_inline | |
|---|---:|---:|---|
| settled-pile flip | 8931 | 9134 | +2.3% **worse** |
| mixed scene | 15146 | 15015 | -0.9% |
| screen of water | 16212 | 15948 | -1.6% |
| full-size step | 5736 | 5809 | +1.3% worse |
| settled screen | 261 | 228 | -12.6% |

A wash, and the one test that is 99% pure sweep moved the *wrong* way.
Reverted.

**Experiment 2: put the hot set in IRAM.** Nothing sand-related was in
SRAM - every symbol sat at a `0x42xxxxxx` flash address behind the 32 KB
instruction cache. A `linker.lf` fragment on the `main` component
(`LDFRAGMENTS`, `noflash_text` so only code moved and the `const` tables
stayed a separate question) mapped `sand_step`, `try_slide_impl` and
`sand_load_above` into SRAM. Placement verified by address before
trusting any timing - `0x4080d39e`, `0x4080df90`, `0x4080d35e` - at a
cost of 4140 bytes of a very comfortable 347 KB of free DIRAM. Two
identical captures:

| | baseline | IRAM | |
|---|---:|---:|---|
| settled-pile flip | 8931 | 9260 | +3.7% **worse** |
| mixed scene | 15146 | 15318 | +1.1% worse |
| screen of water | 16212 | 16239 | +0.2% worse |
| full-size step | 5736 | 5931 | +3.4% worse |

Worse across the board. Reverted, and the planned follow-up stages (the
water-side symbols, then the `const` material tables) were not run - a
first stage that regresses is not a base to add a second stage onto.
**The useful reading is not "IRAM is slow" but "the instruction cache was
never missing."** This loop is small enough and tight enough to sit in
32 KB permanently; taking it out of flash removes a cost that was not
being paid, and gives up the linker relaxations that flash-resident code
gets (`sand_step()` alone links 308 bytes larger in SRAM). Between this
and experiment 1 - one made the hot function 1240 bytes bigger, the other
took it out of the cache entirely, and neither moved the number more than
3.7% - **instruction fetch is not this sweep's problem, and that is now
measured rather than assumed.**

**Experiment 3: the buried-grain early-out (H1).** Compute the burial
load and slide allowance *before* drawing a random number, and return
early when the allowance is zero, so a deeply buried grain that cannot
possibly slide stops paying for the generator. Host suite clean, present
in the build (`try_slide_impl` grew 1022 -> 1134 bytes, `sand_step()`
2750 -> 3092). Device result: `8931 / 15146 / 16212 / 5736 / 261` -
**identical to the baseline in every single test, to the microsecond.**

That is not a null result, it is an impossible one. A change that skips
an RNG draw shifts the generator's stream, and a shifted stream makes the
simulation evolve differently and time differently. Identical-to-the-
microsecond across five tests means the changed code never executed.

**Finding out why is what turned the campaign around.** A throwaway host
probe - counters compiled into the real `sand.c` behind an `#ifdef`,
driving the device test's exact scenario on a laptop, no flash cycle -
reported that during the twenty measured flip steps `try_slide_impl()` is
called **zero** times. During the 300 settling steps that set the scene up
beforehand it is called 3,010,225 times, 95% of which would have taken
H1's early-out. The measured window contains none of them.

The reason is obvious in hindsight and was in `step_one_grain()` the
whole time: `try_slide_impl()` is only reached when the gravity-ward move
*fails*. Reverse gravity under a settled pile and every grain in it is
suddenly in free fall - the gravity-ward move succeeds for all 10,304 of
them, every step, and the slide path is dead code for the entire
measurement. **All three experiments above were aimed at a function the
failing benchmark does not call.** Experiment 1's inlining, experiment
2's placement, H1's early-out: three independent, well-motivated,
correctly-implemented changes to code that never runs in the test they
were meant to fix.

**What the sweep actually does**, from the same host probe, per step:

| | settled-pile flip | mixed scene | water | full-size step |
|---|---:|---:|---:|---:|
| blocks swept / skipped | 1344 / 0 | 1276 / 67 | 1180 / 163 | 1344 / 0 |
| cells examined | 41216 | 39244 | 36211 | 41216 |
| of which non-empty | 10304 | 13283 | 11130 | 10304 |
| `move_liquid_grain()` | 0 | 6282 | 11130 | 0 |
| `try_fall_or_scatter_impl()` | 10304 | 6160 | 0 | 10304 |
| ... of which succeeded | **10304** | **6160** | - | **19** |
| `try_slide_impl()` | **0** | **0** | **0** | 10284 |
| `sand_load_above()` | 0 | 0 | 0 | 10284 |

The last column is the control that makes the rest legible. The
full-size-step test walks the *same* 41,216 cells holding the *same*
10,304 grains as the flip test, and differs in exactly one respect:
nothing moves. Every grain fails its fall, pays a full `try_slide_impl()`
with its RNG draw and its `sand_load_above()` walk, and fails that too -
and the whole step costs **5736 us**. The flip test skips all of that
work and instead lets all 10,304 grains move - and costs **8931 us**.
**Moving a grain is roughly 3.2 ms/step more expensive than failing to
move one, at this grain count.** Every optimisation this campaign and the
last one tried has been aimed at making the *deciding* cheaper, on a
benchmark whose cost is the *moving*.

**Where the moving cost actually is, measured by deleting.** The only
thing on the success path that scales with moves rather than cells is the
per-move bookkeeping: `mark_rows()`, and the `wake_span()` row-clearing
loop inside it. Stubbed to an unconditional `return` (correctness-
breaking, `TEMPORARY EXPERIMENT`, reverted; the water and mixed numbers
under that build are meaningless because `ROW_NO_LIQUID` never clears, so
only the pure-sand flip number is readable) the settled-pile flip went
**8931 -> 5343 us**. Per-successful-move row bookkeeping is **3588 us,
40% of the entire failing test** - an order of magnitude more than
anything else this campaign measured.

**What that bookkeeping is spent on is the part to look at next, and it
is deliberately left as a decision rather than a change.** Since the
fourth attempt moved the settled bits to `block_state`, the only bit left
in `row_state` is `ROW_NO_LIQUID`. `wake_span()`'s comment says as much
and says it was "kept exactly as it was" on the grounds that narrowing it
was unrelated to that move. The consequence, unnoticed until now: a
sand grain moving on a screen with no liquid anywhere on it still clears
three `row_state` entries per move, to invalidate a cache of "this row
has no liquid" that was already true and stays true. The obvious
narrowing - only liquid movement can make a `ROW_NO_LIQUID` bit wrong,
and external placement already wakes explicitly - looks safe, but so did
the batching attempt and so did staggering, and both of those lost to
things their safety arguments said nothing about. It is a one-line-shaped
idea sitting on the sleeping machinery that has now burned three separate
attempts in this file. **It should be its own experiment, with its own
sharpest-existing-test derivation done before any code is written** -
`test_undermining_a_sleeping_pile_collapses_it` and the pool fixtures
being the obvious places to start.

**One micro-item inspected and dismissed without a device round:**
`liquid_mask()`'s 16-entry scan of the flash material table was flagged
as a per-call recomputation worth hoisting. It is called exactly twice
per step (once in `equalise_liquids()`, once in `sand_step_liquids()`),
not once per cell - roughly 32 table reads out of a 4882 us liquid phase.
Reading the call sites was enough to rule it out; flashing a build to
confirm it would have been the measurement, not the discipline.

**Committed from this attempt: nothing but this section.** Every
experiment was reverted, and a fresh `tools/report_performance.sh`
capture on the restored tree reproduces the incoming baseline exactly -
5801 / 16141 / 15144 / 8996 / 259, `failures=3`, unchanged. **The lesson,
and it is a repeat of this file's oldest one wearing new clothes:**
"measure, don't reason" is normally applied to *where the time goes*.
It applies just as hard to *whether the code you are optimising runs at
all*. A profiler-shaped phase split said "the sweep," which was true and
far too coarse; an `objdump` finding said "this function is un-inlined,"
which was also true and completely irrelevant. Three device rounds and a
day would have been saved by the cheapest possible check - a counter, on
the host, in a scenario that takes ten seconds to run - asking not "how
expensive is this function" but "is this function called."

---

## Related

- [`Simulation-Lessons.md`](Simulation-Lessons.md) — the discovery
  narrative this file continues from.
- [`Sand-Simulation.md`](Sand-Simulation.md) — how the simulation works
  today, including the gas material's own section.
- [`Adding-a-Material.md`](Adding-a-Material.md) — the practical
  checklist this campaign's gas-material findings fed directly into.
- [`Architecture.md`](Architecture.md) — the device-verification workflow
  section there is the "how to reproduce a number in this file" version
  of the numbers here, written down as one exact command sequence.
- [Display-and-Rendering.md](../Notes/Display-and-Rendering.md) — the
  parallel `gfx_dirty.h`/`row_runs.h` cap sweeps.
- [Optimization-Playbook.md](../Notes/Optimization-Playbook.md) — several
  of the findings above generalised into board-agnostic techniques.
- `launcher/tools/sweeps/` — the sweep automation this campaign built
  and now keeps as repo tooling.
