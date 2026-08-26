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

This file is the authority, and it is a long read by design - every
derivation and negative result is kept in full. For a first read, or to
find which attempt taught a half-remembered lesson, start with the visual
map in [`Tuning-At-a-Glance.md`](Tuning-At-a-Glance.md) and come back here
for the details.

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

## The ninth attempt: the bookkeeping was bigger than the thing it protected

The eighth attempt ended by measuring, and then declining to act on, one
number: stubbing `mark_rows()` to a no-op took the settled-pile flip from
8931 us to 5343. Per-successful-move row bookkeeping was **40% of the
failing benchmark**, spent clearing three `row_state` bytes to invalidate
`ROW_NO_LIQUID` - a "this row holds no liquid" cache - on a screen with no
liquid anywhere on it. It left the narrowing "as a decision rather than a
change," with a warning attached: the same one-line-shaped idea had already
burned three attempts on this sleeping machinery, so whatever came next
needed its derivation done before any code was written.

This attempt did that, twice, and then found the real answer was neither of
the two narrowings it had derived. **The cache was not worth having at
all.** Removing it outright - not narrowing what clears it, deleting the
whole mechanism - beat every version of keeping it, on every benchmark, and
took the device from `failures=3` to `failures=1` without a budget moving.

### Experiment A: skip the clearing when there is provably no liquid

The derivation, written before the code and reproduced in full above
`mark_rows()` in `sand_priv.h` at the time, is a five-step chain and every
step is load-bearing:

1. `row_state` carries exactly one bit and has exactly one reader,
   `equalise_liquids()`. A stale bit can only ever mislead that pass.
2. While `may_have_liquid` is false there is no liquid in the grid, so
   every `ROW_NO_LIQUID` bit is trivially still true whatever moves - a
   swap cannot conjure liquid into a dry row.
3. The flag only goes false in `equalise_liquids()`, when a **full** sweep
   found none. The rows that sweep skipped were skipped because their own
   bit was set, i.e. proved dry earlier and untouched since - so "found
   none" really does mean none anywhere. This is the step the whole
   argument rests on, and it is the one worth re-reading, because it looks
   circular and is not: the bit's own invariant is what makes the skip
   sound, and the skip is what makes the flag sound.
4. Liquid can only ENTER an empty-of-liquid grid through `sand_set()` or
   `try_spawn_one()`, the only two outside writers, and both set the flag
   BEFORE calling `mark_move()`. The very first liquid placement is made
   with the gate already open and clears its own rows.
5. While the flag is false `equalise_liquids()` never runs at all, so
   nobody reads the bits until the placement in (4) turns it back on.

Measured, two identical captures: the settled-pile flip went **8996 ->
6207 us and passed its 6500 budget for the first time**. But
`full_size_step` crossed the other way, 5801 -> 6121, from passing to
failing, and water and mixed both got ~3-4% worse - all reproducible.

**Why that happened, and the fix, are worth more than the experiment.**
The first version spelled the gate `if (s->may_have_liquid && s->row_state
!= NULL)`. That is one more load than the `if (s->row_state != NULL)` it
replaced, paid on every call - and on a screen of water the flag is always
true, so the extra load is paid ~11,000 times a step and never saves
anything. Folding both conditions into a single derived pointer
(`row_state`, or NULL while there is provably no liquid) kept the hot path
at exactly the one load it always did. Rewritten that way: flip 6069,
`full_size_step` 5980 (passing again), water 17860, mixed 16043.

Water and mixed had now moved by more than the first version had moved
them, in the same direction, which needed explaining rather than shrugging
at. **A control build settled it**: same code, same structure, gate wired
permanently open - baseline semantics exactly. It measured water 17860 and
mixed 16044, *identical* to the real build. The gate costs those two
scenarios nothing whatsoever; their movement was the flash-layout
sensitivity this project has characterised before, produced by the extra
code existing at all. On that same layout the gate is worth 9240 -> 6069 on
the flip.

One follow-up probe, a negative result worth recording so nobody repeats
it: moving the new struct field to the END of `sand_t`, so no existing
field's offset shifts, measured **identical to the microsecond** on every
benchmark. The layout cost is the added code, not the added field.

Shipped. Device failures stayed at 3, but a different 3 - the flip came in,
and it came in by 32%.

### Experiment B: clear only when the move actually moved liquid

The finer narrowing. After A, `mark_rows()`'s remaining cost falls entirely
on scenes that DO hold liquid, where a screen of sand beside a pool pays
full price for every grain of sand that moves, protecting a cache only the
pool can invalidate. Only a move that relocates liquid can make a
`ROW_NO_LIQUID` bit wrong.

**The correctness trap, derived first.** `move_to()` is a SWAP: sand
entering a water cell sends that water back UP into the row the sand came
from, which may have been proved dry moments earlier. So the displaced cell
matters as much as the mover. `move_to()` was changed to report which it
was - free, because `can_enter()` already does the material lookup for a
non-empty target, and an empty target settles it on a value already in a
register. The mover being a liquid cannot happen on this path, and that is
a structural fact rather than luck: `step_one_grain()` sends `KIND_LIQUID`
to `move_liquid_grain()` and returns before these primitives are reached,
and `sand_step_gas()` only ever hands them cells it has checked are
`KIND_GAS`.

**No existing test reached that trap.** The sleeping/liquid tests use water
alone; the sand-through-water tests run with sleeping off, so `row_state`
did not exist in them and no bit was ever set. Nothing in the suite
combined the two.
`test_sand_pushing_water_up_wakes_the_dry_row_it_lands_in` was written to
close that gap, built so only cross-flow can spread the displaced water (a
full pool leaves it no room below and none down either slope), and
**verified to fail with the clear suppressed** - it reported exactly 1
water cell, frozen, against 2 or more when correct. That verification is
the part that matters: a new test that has never been seen to fail is not
yet a test.

Result, two identical captures: flip 5684, `full_size_step` 5593, water
17643 - and **mixed 16043 -> 18330**, a 14% regression on the one scenario
the experiment existed to help.

**A host probe, ten seconds, no flash cycle, explained it, and is the
finding of this section.** Driving the mixed benchmark's exact scenario on
a laptop with counters compiled in, narrowed versus un-narrowed:

| | narrowed | un-narrowed |
|---|---:|---:|
| final grid checksum | 1026727129 | **1026727129** |
| row-clearing calls / step | **0** | 6160 |
| equalise rows scanned / step | **93.0** | 115.0 |
| `give_mass()` transfers / step | 6282 | 6282 |

Identical simulation, and the narrowed build does **strictly less work** -
6160 fewer row-clearing calls and 22 fewer 184-cell row scans per step -
while measuring 2701 us slower. Not a different evolution doing more work
elsewhere; not the branch either, since respelling the comparison as a bit
test produced byte-identical function sizes. Just code placement: that one
condition moved `try_slide_impl` from 838 to 998 bytes and `sand_step()`
from 2724 to 2822, and this target's 32 KB instruction cache did the rest.

**Reverted.** The lesson is this file's oldest one wearing its third set of
clothes. "Provably correct is not provably fast" was the batching attempt;
"provably safe is not provably worth it" was staggering; this one is
**provably less work is not provably faster** - and it is the sharpest of
the three, because here the work reduction was counted exactly, on the real
scenario, and still lost. The test survived the revert.

### What actually won: deleting the cache

Experiment C was briefed narrowly - measure `mark_rows()`'s share of the
water benchmark first, and if it is significant, try the deferral shape
`equalise_one_row()` already uses (mark once per row instead of once per
transfer). The measurement was taken first, on the host, and killed the
proposed fix before it was written:

| water benchmark, per step | |
|---|---:|
| `mark_rows()` calls | 11,142 |
| ... of which from `give_mass()` | 11,130 (99.9%) |
| `move_liquid_grain()` calls | ~11,130 |
| `row_state` bytes written | 33,426 |
| equalise rows scanned / skipped | 120.5 / 103.5 (of 224) |

The per-transfer marking is **already** one call per grain - a liquid grain
averages a single successful transfer - so deferring per-transfer to
per-grain saves no calls, and would write five row bytes where three were
written. Dead on arrival, for free, with no flash cycle.

**But the same table asks a better question, in two numbers sitting right
next to each other:** 33,426 row-state bytes written per step, to save
scanning 104 of 224 rows. That is not a hunch, it is the arithmetic of the
probe that had just been run - and those row scans are *cheap*, because
`equalise_one_row_cell()` bails on a bitmask test per cell. Paying
O(moves) to save O(dry rows) is the wrong shape when moves outnumber rows
by fifty to one.

Deleting the mechanism entirely is trivially correct - it only ever let a
pass SKIP rows, and scanning every row is always safe - so this needed no
derivation, only a measurement. Two identical captures:

| | before | after |
|---|---:|---:|
| screen of water | 17860 | **13130** |
| mixed scene | 16043 | **12096** |
| settled-pile flip | 6069 | 5630 |
| full-size step | 5980 | 5540 |
| settled screen | 260 | 258 |

Every number improved. Water passed its budget for the first time in this
project's history.

**Shipped as a real deletion, not a disabled flag.** With `ROW_NO_LIQUID`
gone, `row_state` has no bits left, so the entire row-shaped buffer went
with it: `sand_enable_sleeping()` takes only `blocks` now, `wake_span()` is
deleted, `mark_rows()` is two byte writes into `dirty_rows` and nothing
else, and experiment A's own derived pointer came out too - A narrowed
bookkeeping that no longer exists to narrow. Block sleeping is untouched:
that one costs O(blocks) per step, not O(moves), and has always earned it.
Re-measured after the cleanup - the deletion moved flash layout again, ~4%
in the same direction across the board, by now an expected tax rather than
a surprise:

| Test | Budget | Round-2 baseline | Shipped |
|---|---:|---:|---:|
| settled-pile flip | 6500 | 8996 **FAIL** | **5947 PASS** |
| screen of water | 16000 | 16141 **FAIL** | **13698 PASS** |
| mixed scene | 12000 | 15144 **FAIL** | 12675 **FAIL** (-5.6%) |
| full-size step | 6000 | 5801 PASS | 5855 PASS |
| settled screen | 300 | 259 PASS | 260 PASS |
| fire cascade | 350000 | 321339 PASS | 304455 PASS |
| screen of fire | 250000 | 221396 PASS | 213871 PASS |

`SELFTEST_COMPLETE failures=3` -> **`failures=1`**, no budget touched.

### The lesson, and where the remaining gap is

Three experiments aimed at making a cache's invalidation cheaper, and the
answer was that the cache should not exist. That is the fourth attempt's
push-to-pull lesson in a new key - *when a hot path resists being optimised
in place, question the path* - except the escape here was not relocating
the work but deleting the thing the work served.

**A skip structure has two costs, and this file had only ever accounted for
one of them.** The coarse-skip-structure lesson in
[Optimization-Playbook.md](../Notes/Optimization-Playbook.md) is entirely
about the cost of skipping at the wrong *granularity*. It says nothing
about the cost of *maintaining* the structure, which is charged to whoever
changes anything, not to whoever benefits from the skip. `ROW_NO_LIQUID`
was correct, and its skip was real, and it lost anyway because the
maintenance bill was 33,426 byte-writes a step and the benefit was 104
cheap row scans. The question to ask of any dirty/settled/proved-empty flag
is not only "is this the right unit" but **"who pays to keep it true, how
often, compared to who reads it."** Block sleeping passes that test
easily - a fixed O(blocks) pass per step, read by the whole sweep.
`ROW_NO_LIQUID` never did, and nobody had checked, because the structure
predated the measurement culture that would have caught it.

**The remaining gap.** The mixed scene misses by 675 us (5.6%), down from
3144 us (26.2%). From the eighth attempt's phase split it is ~74% main
sweep / ~25% liquid pass, and from this attempt's host counters its sweep
does 6160 sand moves and 6282 liquid transfers a step across 39,244 cells
examined. There is no bookkeeping left in that path to remove -
`mark_rows()` is two stores, and `dirty_rows` is NULL in the benchmark, so
in practice it is two predicted branches. What is left is the movement
itself, which the eighth attempt already priced at roughly 3.2 ms per step
per 10,304 grains moved. Closing 675 us means moving fewer grains or
examining fewer cells, not accounting for them more cheaply - a different
kind of change from anything this campaign has tried, and the honest next
question rather than a fourth pass at the same machinery.

Two smaller things flagged rather than fixed. `full_size_step` passes by
2.4%, thin enough that an unrelated change can flip it on flash layout
alone - it has done so twice in this file already, including once inside
this very attempt. And the flip's new 8.5% margin is itself inside the
swing this target has been measured to produce from code placement, so "the
flip passes" is true of this build rather than settled for ever.

---

## The tenth attempt: the last 675 us, and where they actually were

Three attempts had ended by saying the same thing in different words: what
is left is the movement itself, and closing the gap means *examining fewer
cells*, not accounting for them more cheaply. This attempt took that at
face value and went looking for cells that did not need examining. It
found 17,024 of them per step, in the pass nobody had profiled since it
changed shape, and the mixed scene came inside its budget for the first
time - taking the device to **`SELFTEST_COMPLETE failures=0`**, every
frame budget met, with no budget touched.

It also spent three device rounds re-learning the eighth attempt's oldest
trap, on a change that altered no semantics whatsoever. That part is at
the end, and it is the more transferable half.

### Step 0: the phase split had moved, and nobody had re-taken it

The ninth attempt's closing summary described the mixed scene as ~74% main
sweep / ~25% liquid pass. That number came from the eighth attempt, taken
*before* `ROW_NO_LIQUID` was deleted. Deleting it made the cross-flow pass
walk all 224 rows every step instead of ~120, and the split moved a long
way:

| mixed scene, per step | wake | **sweep** | **liquid** | gas | react | settle |
|---|---:|---:|---:|---:|---:|---:|
| eighth attempt (pre-deletion) | 2 | **11322 (74%)** | **3804 (25%)** | 1 | 1 | 8 |
| re-taken here | 2 | **7071 (57%)** | **5215 (42%)** | 1 | 1 | 7 |

The pass that had just been made *correct*ly cheaper in absolute terms had
become the second-biggest thing in the step in relative terms, and the
stale figure would have pointed the whole attempt at the sweep. **A phase
split is invalidated by any change to the phases, including a change that
made one of them faster.**

Host-side counters on the exact device scenarios (no flash cycle) then
priced what each pass looks at:

| mixed scene, per step | main sweep | cross-flow pass |
|---|---:|---:|
| cells examined | 39,244 | **41,216** (the whole grid) |
| ... empty | 25,961 (66%) | 27,888 |
| ... static/gas | 841 | - |
| ... non-liquid occupied | - | 7,046 |
| ... liquid | 6,282 | 6,282 |
| ... powder | 6,160 | - |

Two numbers sitting next to each other decided the rest of the attempt:
the cross-flow pass reads **every cell of the grid** and only 15% of them
hold anything it can act on. Dividing the measured phase times by these
counts across the mixed and water scenarios gives ~95 ns for a cell the
pass rejects and ~300 ns for one it works on - so the 34,934 rejections
cost ~3.3 ms of the 5.2 ms pass. **That was the target.**

### Experiment 1: the material table into DRAM. Genuinely neutral.

`materials[]` is `const`, so it lived in flash `.rodata` behind the same
32 KB cache as all the code, and `material_of()` reads it per occupied
cell in `step_one_grain()` and per non-empty target in `can_enter()`. The
second attempt had planned "const tables to DRAM" as a later IRAM stage
and never reached it; `sand_liquid.c`'s own `liquid_mask()` comment
records a per-cell read of this table costing five and a half
milliseconds. It looked like the cheapest possible win.

`DRAM_ATTR` on the definition, `#ifdef DEVICE_BUILD`-gated so the host
build still compiles, placement verified by address before timing
anything - `materials` moved from `0x420xxxxx` to `0x40810834`, 192 bytes
of a very comfortable ~347 KB of free DIRAM. Two identical captures:

| | baseline | DRAM | |
|---|---:|---:|---|
| mixed scene | 12675 | 12635 | -0.3% |
| screen of water | 13698 | 13628 | -0.5% |
| settled-pile flip | 5947 | 5944 | -0.05% |
| full-size step | 5856 | 5854 | -0.03% |
| settled screen | 260 | 260 | 0% |

Measured again later on top of experiment 3's build, it reproduced the
same tiny margin (mixed 11295 -> 11256). Consistent, always in the same
direction, and **an order of magnitude smaller than this target's
documented flash-layout swing**, which is to say indistinguishable from
zero by anything this project can measure. **Reverted**, because a change
that cannot be shown to do anything is churn, and the shipped tree should
be the tree the numbers came from.

The finding is not "DRAM is useless" but the same one the eighth
attempt's IRAM experiment reached about code: **the 32 KB cache was not
missing on this table either.** 192 bytes read constantly stay resident.
The five-and-a-half-millisecond disaster `liquid_mask()` commemorates was
a sixteen-entry *scan* touching the whole table per cell - a completely
different access pattern from one indexed read, and the lesson does not
transfer between them.

### Experiment 2: letting the cross-flow pass skip settled blocks. Killed by its own derivation.

The obvious skip, and the one this round was briefed to try: the main
sweep already skips settled blocks via `block_state`; give the cross-flow
pass the same test using the same bits. The derivation was written before
any code, and it found two holes - one that the suite already catches, and
one that nothing catches and that no guard makes cheap enough to be worth
having.

**Hole A, staleness.** The pass runs *after* the main sweep and *before*
`finalize_settling()`, so it reads last step's settled bits - the same
contract the sweep lives with. It is not the same risk, though, and the
asymmetry is the point. The sweep's skip is safe because a settled block's
contents provably could not move; by the time the cross-flow pass runs,
the sweep has already moved things, possibly *into* a block still marked
settled. `test_sand_pushing_water_up_wakes_the_dry_row_it_lands_in` -
written in the ninth attempt for a different reason - fails immediately on
a build with this skip in it. Guardable (also test `BLOCK_ACTIVE`), but
not for free.

**Hole B, the perpendicular.** This one is fatal and nothing in the suite
sees it. The settled bits are indexed by *gravity's dithered direction*
(`BLOCK_SETTLED_NEAREST`/`OTHER`). Cross-flow does not use that direction
at all: it alternates along the perpendicular every step (`liquid_flip`),
independently. Under straight-down gravity the dither never varies, so
every step sets the same settled bit while the perpendicular flips every
step.

Now consider a pool that is imbalanced in only one direction - two cells,
mass 15 and 5, with a wall outside them. Under `perp = -x` nothing in it
has a legal move: the left cell looks left at a wall, the right cell looks
left at a *higher* neighbour. Nothing moves, nowhere near it moves, and
`finalize_settling()` gives the block its settled bit at the end of that
step. The next step uses `perp = +x`, where the right cell *would* hand
over 5 - but the block is now settled and the skip would step over it.
Forever.

Measured on the host rather than argued, with a throwaway build (both
numbers are the same fixture, 12 steps):

| step | baseline: left/right, block | with settled-block skip |
|---:|---|---|
| 0 | 15 / 5, SETTLED | 15 / 5, SETTLED |
| 1 | **10 / 10**, awake | 15 / 5, SETTLED |
| 2-11 | 10 / 10, SETTLED | **15 / 5, SETTLED** |

A permanently frozen imbalance, in a scenario that reads as completely
ordinary. Guarding it would mean making the settled bits perpendicular-
aware as well as gravity-aware - four combinations instead of two, a block
qualifying only after being quiet in all of them.

**And the payoff was never there anyway**, which the step-0 counters said
before the derivation finished. On the mixed scene only **1,971 of 41,216
examined cells (4.8%) fall in settled blocks, and exactly zero of them
hold liquid.** The skip would cost 1,344 block-state loads a step to avoid
1,971 cheap cell tests. Not written. **Two reasons to stop, either of
which was sufficient, and both were free.**

### Experiment 3: what the sweep already knows

The counters had also priced the alternative, in the same run. Two
candidate skip structures, both "examine fewer cells", measured as
ceilings before either was coded:

| mixed scene | cells skippable per step | of examined |
|---|---:|---:|
| per-block occupancy counts, main sweep | 4,300 | 11.0% |
| per-block liquid presence, cross-flow pass | **17,024** | **41.3%** |

Four times the ceiling, on the test that was actually failing - and with a
maintenance bill in the *shape the ninth attempt says to demand*. Per-block
occupancy counts are O(moves) to keep true, across a dozen mutation sites,
with count drift as the failure mode. Per-block liquid presence is O(blocks):
**the main sweep already reads every cell of every awake block**, so noting
"there was liquid in this one" is a register OR per occupied cell and a
single store per block - the same shape as the `moved_here` flag sitting
next to it. Nobody is charged per move. The briefed experiment 3 (occupancy
counts in the sweep) was dropped on those two numbers without a device
round.

`BLOCK_HAS_LIQUID` is cleared each step for exactly the blocks the sweep is
about to re-establish it for, and left alone for a settled block the sweep
will skip - safe, because nothing in a settled block moved.

**The one non-obvious part is that the bit cannot be read raw.** Liquid can
arrive in a block *after* the sweep has walked it: the sweep runs bottom-up,
so a grain falling across a block-row boundary lands in a block already
scanned and found dry. So a second bit, `BLOCK_LIQUID_NEAR`, is the first
expanded to a block's 8 neighbours by one pass over the 24 blocks. That is
sufficient because liquid moves at most one cell in the sweep and at most
`SAND_LIQUID_SIGHT` (8) in the cross-flow pass, both well inside a 32-wide
block - so wherever it can arrive, the block that was *seen* holding it is
that block or an immediate neighbour. The invariant, written above
`BLOCK_HAS_LIQUID` in `sand_priv.h`, is therefore not "the bit is exact"
but **"every liquid cell sits in a block whose NEAR bit is set"** - and the
contrapositive is what the skip uses, including for the `found_any` test
that arms `may_have_liquid`.

**The suite did not cover that, and the gap had teeth.** With the expansion
removed, all 194 existing tests still passed. The failure it hides is not a
one-step delay: a single water cell falling out of its block empties its
source, so the pass finds liquid *nowhere*, sets `may_have_liquid = false`,
and switches cross-flow off permanently with water still on the screen.
`test_water_falling_into_the_next_block_down_still_spreads` is built to
produce exactly that - two block-rows, one cell of water, a stone shelf
below the boundary so only cross-flow can move it - and was **verified to
fail before it was kept**, reporting a single frozen cell against several
when correct.

Two identical captures, and the three benchmark scenarios evolve to
byte-identical grids before and after:

| Test | Budget | Round-3 baseline | Shipped |
|---|---:|---:|---:|
| mixed scene | 12000 | 12675 **FAIL** | **11167 PASS** (-11.9%) |
| screen of water | 16000 | 13698 | 13288 |
| settled-pile flip | 6500 | 5947 | 5969 |
| full-size step | 6000 | 5856 | 5876 |
| settled screen | 300 | 260 | 259 |
| fire cascade | 350000 | 304455 | 316117 |
| screen of fire | 250000 | 213871 | 217221 |

`SELFTEST_COMPLETE failures=1` -> **`failures=0`**. Every frame budget this
project has ever written is met, and not one of them was ever raised.

### The three rounds this cost, and why they are the useful part

The change above measured 11295 us the first time it ran. Then it was
tidied - a stale comment, a genuine bug in a rarely-taken branch, and a
split-up of the two functions the project's own
`tools/cognitive_complexity.py` had started flagging - and re-measured at
**12702**, back over budget, with the water benchmark up 14%.

Nothing about the simulation had changed. Host checksums were identical.
The temptation was to write it off as this target's documented flash-layout
noise and start bisecting blindly. **What made it diagnosable was a control
already in the capture**: the three benchmarks that touch no liquid at all
came back *byte-identical* - 5876 / 5969 / 259, to the microsecond. A
global layout shift cannot do that. Only the liquid path had moved, so the
cause was in the liquid path.

It was one line, and it was this file's own eighth attempt wearing new
clothes. The tidy-up had turned

```c
if (s->block_state == NULL) {
    return equalise_one_block(...);   /* early return */
}
...the block loop, calling equalise_one_block again...
```

into an `if`/`else`, purely for readability. **`equalise_one_block()` is
`static inline` with two call sites either way** - but the restructuring
was enough to change what GCC did with it, and the eighth attempt has the
general form of this already written down: a `static` function with exactly
one call site is inlined unconditionally
(`-finline-functions-called-once`); add or reshape call sites and only the
size heuristics are left, and they decline bodies this large. There it cost
2 ms on a function the failing test never even called. Here it cost 1.8 ms
on the pass the whole attempt was about.

The fix was to stop having two call sites at all: express "no block bits to
consult" as a NULL `brow` pointer inside the *same* loop, rather than as a
separate whole-row branch. One call site, one loop-invariant test.
**11137 us** - better than the version before the tidy-up, and the
complexity score came down as a side effect rather than at a price.

Three things worth carrying:

1. **Keep a control in every capture.** The seven frame-budget tests are not
   seven measurements of the same thing; three of them never touch liquid.
   That accident is what turned "the layout moved, who knows" into "the
   liquid path moved, go look at the liquid path" in one capture.
2. **Readability refactors are performance changes on this target.** Not
   might-be: this one cost 14% on a benchmark, with the compiler's inlining
   decision as the whole mechanism. The `cognitive_complexity.py` score is a
   hotspot finder, and it is worth acting on - but on a hot path it has to be
   re-measured on device exactly like any other change, and the version that
   ships is the version that was measured.
3. **The final restructuring was better on both axes**, which is the note to
   end on rather than "complexity work is dangerous." Removing the second
   call site removed the duplicated branch *and* the inlining cliff. When a
   tidy-up costs performance, the tidy-up is usually not finished.

### Where this leaves it

There is no failing budget left, so there is no next optimisation this file
can point at with a number attached - and the honest thing is to say so
rather than manufacture one. The thinnest margin is `full_size_step` at
~2.1%, which is a *risk* to watch rather than a target to chase, and the
lesson immediately above is what to reach for the next time an innocent
change moves it.

If a future material or feature does put a budget back over, the
step-0 method here is the part to repeat before anything else: re-take the
phase split (it goes stale the moment a phase changes), then count what each
phase *examines* against what it *acts on*, on the host, in the scenario that
is actually failing. Both experiments this attempt declined to run were
declined on counters that took ten seconds to produce.

---

## The eleventh attempt: a feature wave, and one branch in the wrong place

The tenth attempt ended by saying there was no failing budget left to
point at, and that if a future feature ever put one back over, the
step-0 method was the thing to repeat before anything else. A large wave
of new materials then landed - temperature and conduction, viscosity,
interfacial drag, percolation, convection, acid, lava, glass, ice, snow,
dirt, trees and plants - `sand_reactions.c` went from a few hundred
lines to about 2,800, and four budgets went red at once.

This attempt is mostly about how much of that was a feature and how much
was an accident. The answer turned out to be split cleanly down the
middle: the two liquid budgets were an accident, in one commit, in one
branch, and are recovered in full; the two fire budgets are the feature,
and the honest thing to do with them is re-peg them rather than chase
them.

### Step 0, first half: the baseline was older than the code

Before diagnosing anything, the incoming capture
(`launcher/tools/results/performance_20260825_203707.md`) was checked
against the tree it was supposed to describe, and it does not describe
it. The self-test names in the raw log place the flashed build somewhere
between `b9e8845` and `4fe0d04` - it contains
`test_every_liquid_declares_a_mobility` but not
`test_the_mixed_scene_puts_every_material_pair_in_contact`, and it holds
the every-material flip to a budget above 60,091 us, which only builds
before `a943be9` did. **Fifty commits landed after it**, including
glass, thermal shock, snow, ice, dirt, convection, percolation and the
whole of the plant and tree work.

Two things follow, and both change what the numbers mean:

- The capture's own headline is **`SELFTEST_COMPLETE failures=4`**, not
  the `failures=5` this repo's `Architecture.md` recorded. The fifth is
  a real expectation for HEAD - the every-material flip is held to
  54,000 us now and last measured 60,091 - but it is not something that
  capture observed, and the two were conflated. Corrected there.
- The generated report's every-material row reads "300000" as the
  budget. `suite_sand.c` says 54,000. The number in the report is the
  `300000` that appears in that test's own comment prose, describing an
  earlier estimate that was thrown out. The report tool is matching a
  number near the test name rather than the assertion's argument.
  Flagged rather than fixed here, since it is a tooling bug and not a
  simulation one, but it is exactly the shape of thing that makes a
  capture quietly lie.

**The general point is the tenth attempt's own lesson about phase
splits, one level up: a device capture goes stale the moment the tree
moves under it, and nothing in the file says so.** Every number quoted
below as "device" is that stale capture, and every conclusion drawn from
it is scoped accordingly.

### Step 0, second half: rebuild every commit and re-time it

The eighth attempt's host-counter trick answers "does this code run."
This attempt needed a different question - "which commit made it slower"
- and the same machinery answers it if you are willing to spend the
build time: a harness that reproduces the device benchmarks' exact
scenarios on the host, driven over every commit in the wave, rebuilding
and re-timing at each one. Seventy-five builds, about twenty minutes,
no flash cycle.

Two things make the output trustworthy despite host wall-clock being
only a relative signal. The harness prints a checksum of the final grid,
so a scenario that evolves identically says so; and it carries the same
liquid-free controls the device suite does, so a run where the machine
was busy announces itself.

For the screen of water the result was not a trend, it was a step:

| commit | water, host us/step |
|---|---:|
| ... twenty commits, `0378703` through `ec18186` | 60-70 |
| `726ca63` **Give liquids a viscosity** | **99.8** |
| ... fifty-four commits, `6cfc9c7` through HEAD | 95-105 |

Byte-identical final grid on both sides of that step, and on every
commit of the wave. Nothing else in seventy-five commits moved the water
benchmark at all.

### The cost was one branch, and it was never the logic

`726ca63` adds exactly two things to the liquid path: a call to a new
`liquid_may_move()` at the top of `move_liquid_grain()`, and another
inside `equalise_one_cell()`. Removing them one at a time on the host
put the entire regression on the first one - the cross-flow one costs
nothing measurable, because three cheaper tests run before it and almost
no cell reaches it.

And on a screen of water that gate cannot even say no. `s->mobility`
defaults to 255, water's own figure is 255, so it returns true every
time without drawing a random number - which is why the simulation is
byte-identical with the check present or deleted.

A short series of substitutions, each one variable, found which part of
a load-and-a-branch was expensive, and the answer was: neither.

| at the top of `move_liquid_grain()` | water, host us/step |
|---|---:|
| nothing (control) | 70 |
| `if (mat_id == 0xFF) return false;` - a branch on a live register | 69 |
| `if (s->h == 0x7FFF) return false;` - a load of a cold field | 71 |
| `if (materials[mat_id].mobility == 0x7F) return false;` | 66 |
| `if (s->mobility != 255) return false;` | 98 |
| the real gate | 101 |

A branch is free. A load is free. A load from the material table is
free. That specific comparison is not - and the difference between it
and the others is nothing about the data, it is which side of the branch
GCC decides is the fall-through.

**Confirmed by disassembling the real device object rather than
inferring it.** Compiling `sand_liquid.c` with ESP-IDF's own flags,
unhinted, the cold "too viscous to move" blocks land at offsets `0xb6`
and `0xce` - spliced into the middle of a 1,102-byte function that runs
about 11,130 times a step, so every cache line the hot path fetches
carries bytes it will never execute. This is the same 32 KB instruction
cache that decided attempts 07, 08, 09 and 10; here nobody had written
any new code for it to fetch, the compiler had simply interleaved code
that was already there.

Marking the branch unlikely moves those blocks to `0x24c` and `0x264`,
at the tail, and the hot path becomes contiguous again. Four spellings
were measured back to back, with the liquid-free controls flat across
all of them:

| variant | screen of water | water at the app's per-material mobility |
|---|---:|---:|
| unchanged | 101.1 | 101.5 |
| gate deleted outright (the ceiling) | 74.7 | 74.8 |
| **`__builtin_expect(..., 0)`** | **74.5** | **74.7** |
| cold half split into its own function | 77.5 | 84.4 |
| that half marked `noinline` | 97.8 | 96.8 |

Only the hint reaches the ceiling, and it reaches it on both paths. The
split is worth a note as a near miss: it works, but by GCC's size
heuristic rather than by construction - shrink the callee and the
compiler folds it back in and re-splices the blocks, which is exactly
what happened when a variant of it was tried with a smaller body. That
is the inlining cliff of attempts 07/08/10 wearing its third face, and
this time the fix was to stop relying on the heuristic instead of
arranging to satisfy it. Marking the cold half `noinline` is worse than
doing nothing: then the *call* sits in the hot path.

The second column matters more than the benchmark does. Every device
frame-budget test runs at the default `s->mobility` of 255; the real app
calls `sand_set_mobility(SAND_MOBILITY_PER_MATERIAL)`, so it takes a
different path through that same function, and no test in this repo
measures it. It regressed by the same amount and it is fixed by the same
line - but that was luck rather than coverage, and worth saying plainly.

`__builtin_expect` is the first one in this codebase. It is not the
`always_inline` family that the dead-end list warns about: those force
the compiler to do more, and this only tells it which way a branch goes,
which happens to be a fact - water always moves. It is wrong for oil,
which refuses about two steps in three, and that costs oil the far
branch. Water is what a screen of liquid is usually made of.

### Fire: diffuse, and mostly not where it was expected

The same commit-by-commit walk over the fire benchmarks found no step at
all - just a creep, roughly 1,000 to 1,300 host us/step across the wave,
with the largest single contribution (about 14%) from heat conduction
and the rest spread over a dozen commits. That is what genuine feature
cost looks like, and it is the opposite shape from the water finding.

Splitting the step into its two passes, at the round-4 baseline and at
HEAD, says where it went:

| full screen of fire, host us/step | round 4 | HEAD |
|---|---:|---:|
| total | 978.9 | 1295.4 |
| `sand_step_gas()` | 720.0 | 776.2 (+8%) |
| `sand_step_reactions()` | **266.7** | **537.4 (+101%)** |

The reactions pass exactly doubled, and it accounts for 85% of the
benchmark's growth. Worth noticing anyway, because it corrects an
assumption this round started with: **the reactions pass is not the
expensive one.** Even doubled it is 41% of the step, and the gas pass -
which barely changed - is 60%. A screen that is entirely fire pays
`sand_step_gas()` to discover, every step, that a fully packed grid of
same-density cells has nowhere to move.

Measure-by-deleting inside the reactions pass, one mechanism at a time
against a 1,295 us baseline:

| stubbed out | us/step | share of the benchmark |
|---|---:|---:|
| the four-neighbour quench scan | 1,175 | **9.3%** |
| `try_heat_transform()` | 1,231 | 5.0% |
| `smothered()` | 1,246 | 3.8% |
| `try_ignite()` | 1,262 | 2.6% |
| `conduct_heat()` | 1,269 | 2.0% |
| the burn-out smoke puff | 1,290 | 0.4% |
| `try_flare()` | 1,292 | 0.3% |

The genuinely new mechanisms - conduction, heat transformation, flare,
smoke - come to about 8% between them. They are not the doubling. Most
of the rest is structural: `f9bc63f` split the reaction properties out
of `material_t` into a second table, for good reasons of struct size, and
the consequence is that every per-cell and per-neighbour question the
pass asks now reads two arrays where it used to read one. That is a real
cost, it is spread everywhere, and it is not something a branch fixes.

One line of it was worth taking. The quench scan is the largest single
item at 9.3%, and on both fire benchmarks it is spent entirely on boards
with no liquid anywhere - four material-table reads per burning cell,
every step, confirming that a thing which is not there is still not
there. `s->may_have_liquid` already answers that, and answers it
soundly: `latch_content_flags()` arms it for every write in the
simulation that can create a liquid, this pass's own snow-melting-into-
water included, and it is only ever cleared by a full cross-flow sweep
that found none.

That correction had to be made in the record before it could be leaned
on. The invariant comment above `BLOCK_HAS_LIQUID` in `sand_priv.h`
claimed that "sand_reactions.c only ever writes MAT_FIRE" - true when it
was written, false since snow arrived. The conclusion still holds, by a
different route (that placement goes through `place_cell()`, which
latches), and the comment now says so.

### What shipped, on the host, with the controls flat

Two commits. All eight scenarios evolve to byte-identical final grids
before and after both of them.

| scenario, host us/step | round 4 | before | after |
|---|---:|---:|---:|
| screen of water | 70.0 | 104.2 | **75.1** |
| water at the app's mobility | 69.5 | 101.5 | **78.3** |
| mixed scene flip | - | 84.0 | **69.3** |
| full screen of fire | 1,006.8 | 1,325.6 | **1,192.6** |
| fire cascade through gas | 1,343.0 | 1,943.8 | **1,833.0** |
| full-size step (control) | 34.0 | 33.9 | 34.2 |
| settled screen (control) | 0.9 | 0.9 | 0.9 |
| settled-pile flip (control) | 35.3 | 35.0 | 32.6 |

The mixed scene follows water without being touched, which is what its
own composition predicts - it is a sweep with a water half bolted on.
The reactions pass turns out to cost the water and mixed scenes about
2 us and 1.4 us a step respectively: the content flags clear after the
first step and the pass stops running, so there was nothing there to
find.

`sand.c` is not touched by either commit, and its compiled device object
is **identical instruction for instruction** before and after - checked
by disassembly, since the object files themselves differ only in DWARF
line numbers shifted by the `sand_priv.h` comment. The three control
benchmarks run entirely in that code. They can still move on a fresh
flash, because the whole image relinks around a `sand_liquid.c` that
grew two bytes and a `sand_reactions.c` that shrank 250 - but if they
move, it is the layout lottery and not this work.

### The two fire budgets: a recommendation, not a change

Neither fire budget was ever a frame-rate promise. Both say so in their
own comments - "not a real-time requirement", "a real regression guard"
- and both were pegged at about 9% over a measured number, on scenes
(edge-to-edge fire, edge-to-edge gas) that the pour-brush UI cannot
practically produce.

A deliberate feature wave moved the thing they were pegged to. The
recommendation is to re-peg them from a fresh capture of HEAD, by the
same method their comments already document: measure, then set the
budget about 9-10% above it. **Not adjusted here** - changing a budget
is not this file's call to make, and a budget moved to accommodate the
code it guards has stopped being a guard.

Two things should go into that decision rather than just a number:

- The stale capture understates HEAD. On the host, the full screen of
  fire gained a further 6.8% after that capture was taken and the fire
  cascade gained **18.7%**, from the fifty commits it never saw. This
  round gives back 8.3% and 4.5% respectively, so fire lands roughly
  where the capture found it while the cascade should be expected to
  come in *higher* than its 390,158, not lower.
- The gas pass, not the reactions pass, is 60% of the full-screen-of-fire
  benchmark, and it is the same gas pass as at round 4. If those numbers
  ever need to be real rather than a guard, that is where the work is,
  and it is a design question - "creeping fire", deferred when fire was
  built - rather than a tuning one.

### What this attempt is worth carrying

**A capture is a measurement of a tree, not of a project.** The single
most useful thing this round did was spend five minutes checking whether
the incoming numbers described the code they were about to be used to
diagnose. They did not, by fifty commits, and every ratio drawn from
them would have been quietly wrong.

**Bisecting on the host is affordable, and it is a different tool from
counting.** The eighth attempt's counters answer "does this run"; a
rebuild-and-retime over a commit range answers "when did this start",
which is the question a regression actually poses. Seventy-five builds
cost about twenty minutes and no flash cycles, and the answer here was
a single commit out of seventy-five, with a byte-identical simulation on
both sides of it - a degree of certainty that no amount of reading the
diff would have produced.

**The inlining cliff has a sibling: block layout.** This file has three
attempts about whether the compiler folds a function in. This one is
about where the compiler puts the code it decided to keep, and it cost
26% of a benchmark with no function boundary involved at all. The
detection is the same - `objdump` the real device object and look, do
not infer - but the thing to look at is which blocks sit between the
entry and the work, not whether a symbol exists.

**And the oldest lesson, in its cheapest form yet:** the change that
fixed a 26% regression adds no state, removes no work, and computes
exactly what it computed before. Every plausible story about *what* that
line was doing was wrong, because it was not doing anything. It was
sitting in the way.

---

## The twelfth attempt: a mask that measured nothing, and a gate that never closed

The eleventh attempt closed with the two liquid budgets recovered on the
host, the two fire budgets diagnosed as genuine feature cost and
recommended for re-pegging rather than chased, and a loose end named
explicitly: the reactions pass had doubled across the feature wave, but
it is the gas pass - unchanged since round four - that is 60% of the
full-screen-of-fire benchmark. This round had two halves. The first gave
the reaction engine its own cross-material benchmarks, which it had
never had. The second went looking for cells the reactions pass could
skip. It found some, built the skip, and measured it at zero. The
attempt's real finding came from a different question entirely - whether
a gate the code already had could ever close - and it could not.

**The most recent device capture is now roughly fifty commits stale**,
the same capture the eleventh attempt diagnosed as such, and nothing
below is device-verified. Every number in this section is host-only or,
where marked, a guess awaiting its first flash.

### Half one: three benchmarks for a pass that had none

The suite's perf tests covered single materials and one sand/water/stone
mix. None of them put the reaction engine under cross-material load, so
the pass that doubled during the eleventh attempt's feature wave had no
benchmark of its own to have caught it doubling.

Each new benchmark's scene builder is factored out of the
`#ifdef DEVICE_BUILD` block and shared with a HOST test that runs the
same scene for the same number of steps and asserts the reactions
actually fired. That pattern already existed for the mixed scene's
all-pairs tiling; it is worth re-using for the reason the eighth attempt
gave it its name: that attempt spent three device rounds optimising a
function the failing benchmark never called, and a host test that proves
the scene exercises what it claims to is the cheap guard against
repeating that. All three host tests were verified to fail when their
scene was deliberately broken.

**Four liquids reacting at once.** Water, oil, acid and lava, painted
upside down - lava on top, then acid, then water, then oil at the
bottom. The reasoning is the interesting part: the four liquids have
different densities, so painted in their own settled order they
stratify and the scene goes quiet within a few steps, measuring almost
nothing. Inverted, every layer has to migrate through every other one,
so the interfaces stay in contact and reacting for the whole window.
Measured on a host harness, the scene is still producing 245 cells of
stone (lava quenched by water), 209 of steam and 204 of fire (oil
ignited by lava) at the end of the measured window, and those figures
are flat from step 10 through step 60 - it reaches a steady churn rather
than a stop. Ten settling steps, twenty measured.

This is also the test that closes a coverage gap, and the record needs
a small correction on the way there. The eleventh attempt recorded that
no performance test takes the `sand_set_mobility(SAND_MOBILITY_PER_MATERIAL)`
path the real app runs. That was not quite right - the every-material
flip already did. What was true, and is the point worth keeping, is that
no test with a real *budget* did; the every-material flip is held to a
deliberate reduction target instead. The four-liquid scene runs at the
app's own per-material scatter, decay and mobility, so the app's liquid
mobility path now has a budgeted benchmark behind it.

**A lava stress scene.** Lava is the reaction-richest material there is:
a heat source, it quenches to stone in water, boils water to steam,
turns sand to glass by heat, ignites wood and oil, and flares. The scene
is a lava reservoir on the floor, repeating six-cell columns of sand,
wood and oil standing in it, and a water slab as a roof. Every fourth
column is left empty on purpose, and that detail is what makes the test
work: without a chute the roof water perches on the columns and takes
most of a minute to reach the lava, so quench and boil - two of the six
reactions the scene exists to exercise - never fire inside the measured
window. With it, water reaches the lava while the scene is still
burning. Across the measured window (thirty settling steps, twenty
measured) glass climbs from about 782 to 1132 cells, fire runs at
1300-2000, and steam and stone reach 79 and 26.

**A screen of smoke and steam.** A full grid of both, checkerboarded,
with one flame. What it catches that nothing else does: the two existing
fire benchmarks are made of fire and gas, and neither has any convection
behaviour; smoke and steam do - they warm what they touch - and no
benchmark in this suite had ever held either in quantity. It runs at the
default scatter, decay and mobility rather than the per-material ones,
because at per-material decay the gases fade inside the window and it
stops being the steady worst case it exists to be. Ten measured steps
rather than twenty, the same choice the full-screen-of-fire test makes,
because a full screen of gas is the most expensive step this simulation
has and the device's task watchdog has a five-second window - the third
attempt already tripped it once with a test that ran too many steps on a
full grid. Conserves all 41,216 cells.

**All three ceilings are guesses and say so in the source.** 150,000 µs
for each of the two liquid scenes and 400,000 µs for the gas screen,
labelled as loose sanity ceilings rather than budgets - wide enough not
to pass as tuned, tight enough to catch something catastrophic like an
accidental quadratic. They are deliberately not extrapolated from host
timings: this file already records one extrapolation that came out four
times too pessimistic, because the host-to-device ratio is dominated by
cache behaviour the host does not model and is scene-specific besides.
**All three must be re-pegged at about 9-10% over the measured number
the first time they run on hardware** - the method this file's other
budgets already document.

### Half two, first: counting what the reactions pass could skip

Counters were taken before any code, on the host, on each benchmark's
exact scenario: of the non-empty cells the reactions pass walks, how
many does its own branch chain leave with nothing at all to do?

| scene | non-empty walked per step | cells the chain leaves idle |
|---|---:|---:|
| full screen of fire | 41,216 | 0 - every cell is burning |
| every material at once, flipped | 22,200 | 4,278 (19.3%) |
| screen of smoke and steam | 41,216 | 0 - all 41,215 gas cells take the convection branch |
| four liquids | 34,491 | 17,111 (49.6%) |
| mixed scene flip, screen of water | - | the pass never runs at all |
| fire cascade through gas | 41,216 | 41,215 at pass entry only - see below |

The fire cascade's figure is the one to read carefully. The counters
sample the grid at pass entry, and during that single step the cascade
converts gas to fire ahead of its own scan pointer, so the pass itself
sees almost none of those cells idle by the time it reaches them. **A
count taken before a pass is not a count of what the pass does.**

Per material, out of the sixteen rows: only water, oil, gas and ice are
incapable of initiating any reaction (empty is skipped before the row is
ever read). Stone and glass are not inert - they carry a heat ramp.
Smoke and steam are not inert - they warm.

### Experiment 1: a per-cell "does this material react at all" mask. Worth nothing.

The idea was the cheapest possible skip: a 16-bit mask, in the shape of
the existing `liquid_mask()`, tested before touching the cell's reaction
row, so an inert cell never pays the second table read the eleventh
attempt identified as the reactions pass's structural cost. Built both
ways and measured on the host:

| scene | change |
|---|---:|
| every material at once, flipped | **−0.1%** |
| full screen of fire | +1.2% |
| four liquids | −1.5% |

All inside the noise - including the every-material flip, the very scene
where the counters said 19.3% of the walked cells were skippable.

The reason is worth keeping. Those 19.3% were already cheap. An inert
cell falls out of the branch chain after a handful of well-predicted
tests on fields the compiler has already loaded; a mask test plus its
own loads costs about the same as what it removes. **This is this file's
oldest lesson in its fourth set of clothes - "provably less work is not
provably faster" - and this time it was cheaper than ever to learn,
because the mask never had to reach a device.**

There is a second, independent reason it could not have shipped as
built. To skip a smoke cell the mask has to know whether
`may_have_temperature` is armed, so the flag-aware version snapshots the
`may_have_*` flags once at the top of the pass. Those flags are armed
*during* the pass, by `latch_content_flags()`, whenever a reaction
creates a cell - so a cell that would have taken the drinks, grows or
warms branch later in the same pass gets skipped by a stale mask.
Measured: different final grids on both liquid scenes. Making it exact
means reading the flags per cell, which is precisely what the unmodified
code already does. **Not shipped.**

### Experiment 2: per-block reactive bits. Declined on the counters, before any code.

The tenth attempt's presence-bit pattern was the fallback if whole
blocks of gas turned out to be idle. The same counters rule it out: the
idle cells are scattered by material - water and oil interleaved through
the scene - not clustered into blocks, and the two fire benchmarks have
no idle cells at all. There is no block-shaped skip to find, so there
was nothing to run the maintenance ledger on.

### Experiment 3: the convection gate. Shipped.

This is the change that came out of the round's original question - can
the pass skip neighbour work for gases and smoke or steam - and the
answer turned out to be yes, but for a different reason than the skip
the round was designed around.

`step_one_warming_cell()`'s own comment promised that on a board with
nothing that can hold a temperature, convection costs "a predicted-false
branch per gas cell and no scan at all". It never did, and the gate it
named cannot close on a board holding smoke or steam, for two
independent reasons: `latch_content_flags()` arms `may_have_temperature`
for any material with a non-zero `warms`, and smoke and steam both have
one, so placing either arms it; and the convection branch reports
`FOUND_TEMPERATURE` unconditionally whenever it is taken, which re-arms
the flag every step. Armed on placement, re-armed by its own use.

So every gas cell ran a four-neighbour scan - two table reads each - to
discover the board holds nothing with a heat ramp. On the new screen of
smoke and steam that is 41,215 cells of it, every step, for ever.

The fix is a second flag, `may_have_heat_holder`, armed by
`latch_content_flags()` for any cell with a non-zero `heat_ramp` at any
variant, ANDed onto the existing gate rather than replacing it. It
answers the question the comment was describing. **It is safe by
construction rather than by measurement:** with the flag false no
heat-ramp cell has ever been written to the grid, and the scan skips
every neighbour whose heat ramp is zero before drawing a random number
or mutating anything, so on such a board the scan provably does nothing
at all - which means suppressing it cannot change the grid or the
random-number stream. Confirmed: all eight scenes evolve to
byte-identical grids.

**The load-bearing detail, and the one that had to be measured rather
than reasoned:** the flag is armed and never cleared. Clearing it at the
end of the pass like the other five flags is the obvious next thing to
write and it is wrong. A heat-ramp cell can be created mid-pass, behind
the scan pointer - lava quenching into stone is exactly that - so the
walk never reports it and the end-of-pass clear would erase what
`latch_content_flags()` armed a line earlier, with nothing left to ever
re-arm it. A build with that clear in it measured a different simulation
from the baseline on both liquid scenes; the arm-only version is
byte-identical on all eight. The maintenance ledger, which this file
demands of any such flag, answers itself: nobody pays any per-step
upkeep at all. The one cost is that a board which once held stone or
glass and no longer does keeps paying the scan - which is exactly what
it pays today without the flag, so it can only fail to help, never hurt.
A test built on the mid-pass creation case guards it, and was verified
to fail without the arming.

Host, best of six runs with the two builds interleaved: **the screen of
smoke and steam 890.8 → 827.9 µs/step (−7.1%)**, every other scene
inside the noise. The win is narrow and scene-shaped, and it is worth
being explicit about that: it lands on boards that have never held
stone or glass, and a board with a wall on it pays exactly what it
always paid.

### Experiment 4: where the fire benchmarks' cost actually is. Measured, not built.

The eleventh attempt ended by pointing at the gas pass - 60% of the
full-screen-of-fire benchmark, and unchanged since round four - and
calling it a design question. This round put a number on the largest
piece of it, by deleting it: `find_nearest_empty()` stubbed to return 0
immediately (temporary experiment, reverted).

| scene, host µs/step | committed | without the sight scan | |
|---|---:|---:|---:|
| fire cascade through gas | 1834.0 | 1497.2 | **−18.4%** |
| full screen of fire | 1249.7 | 1061.6 | **−15.1%** |
| screen of smoke and steam | 825.0 | 700.7 | **−15.1%** |
| smoke and steam over stone | 913.4 | 783.6 | −14.2% |
| every material at once | 1485.9 | 1371.1 | −7.7% |
| four liquids | 700.2 | 678.9 | −3.0% |
| mixed scene flip | 71.6 | 71.7 | +0.1% |

**That is the single biggest item any round has found in the fire
benchmarks.** The mechanism: on a packed screen `has_room_above()`
fails, `neighbour_is_open()` then succeeds - because the neighbour is
more of the same gas - and `find_nearest_empty()` walks the material's
full `sight` (5 cells for fire, 20 for smoke, 24 for steam) through
identical gas to return 0.

State plainly that no cheap early-out is available, and why: knowing in
advance that the scan will fail means knowing whether an empty cell lies
within sight, which is an occupancy question, and every structure that
answers it - a per-block "this block holds an empty cell" bit maintained
by the sweep, with the tenth attempt's NEAR ring to make it sound - is a
design rather than a one-line guard. Left as the next round's target,
with the number attached. This also sharpens the eleventh attempt's
re-pegging recommendation, which stands unchanged: it is the gas pass,
not the reactions pass, that these two budgets are really measuring.

### The round-five deferred item, closed rather than deferred again

Merging the burning cell's separate four-neighbour walks was carried
over from the eleventh attempt as worth about 5% on the host. Re-read,
it is not available: `try_ignite()` and `try_heat_transform()` already
share one walk. What remains is the quench scan and `smothered()`, and
each returns from the whole function when it fires - a cell being
quenched must not first ignite its neighbours - so merging them changes
what each mechanism observes, which is an observable ordering change
rather than a refactor. Closed with the reason rather than carried
again.

### Two corrections to the record

Both belong in this section rather than a footnote; this project's
convention is that a fact discovered to be wrong gets fixed in the same
change that would otherwise build on it.

1. **The `report_performance.py` budget mis-parse does not exist.** The
   eleventh attempt recorded that the tool reads the every-material
   test's budget as "300000" - a number appearing in that test's comment
   prose - rather than the 54,000 in its assertion, and flagged it as a
   tooling bug that would make captures quietly lie. Run against the
   current source the tool returns all eleven budgets correctly, 54,000
   included. The stale capture's report said 300,000 because the build
   that was flashed *asserted* 300,000 at the time; the tool was reading
   the assertion correctly and the source has changed since. There was
   never a bug. The general point is the eleventh attempt's own, turned
   on itself: a report is a measurement of a tree too, and "the tool is
   wrong" was diagnosed from a report generated against different
   source.
2. The per-material mobility coverage claim, described in Half One
   above.

### A note on the host harness that every number above depends on

Worth recording as a limit on the method rather than buried: within a
single run of the harness the same static scene drifts upward by as
much as 40% from the first scene measured to the last, and a
single-step scene varies by about ±7% between runs. Every comparison in
this section is best-of-N with the two builds *interleaved* in the same
session; a host wall-clock number compared across sessions is not
usable at all. Host timing was already documented here as a relative
signal only - this is how relative it is.

### Plants, ruled out of scope mid-round

The plant materials - `MATX_PLANT` and the mechanisms around it
(`drinks`, `grows`, `sprouts`, withering, the whole-tree support walk) -
were taken out of this round's scope while it was running, because
they are under active visual development and their behaviour is about
to change. Two consequences, both deliberate:

**No new benchmark measures a plant, and the one scene that could have
grown one now says so.** The lava stress scene contains wood, and sand
that could in principle become wet soil, which is everything a sprout
needs. It never actually grows one - the roof water reaches the lava
and flashes to steam rather than wetting the sand, so no soil is ever
made and the wood stays dry for the whole run. That was an accident of
tuning rather than a property anyone checked, and a benchmark whose
cost quietly started including plant growth would be pegged against
unfinished behaviour with nobody the wiser. Its host test now asserts
that no extended-range cell exists at the end of the run, so if the
plant work changes that, it announces itself instead of drifting into
a budget.

**Nothing plant-specific was optimised, and the counters that touch
plants are recorded here as observations for whoever picks that up.**
On the every-material flip, 523 cells per step - 2.4% of the non-empty
cells the pass walks - reach the `drinks` branch, and none reach
`grows` or `sprouts` in that scene. Of the sixteen extended reaction
rows only two are non-inert, `MATX_ICE` and `MATX_PLANT`; the other
fourteen are all-zero. That is the whole of what this round can say
about plant cost, and it is deliberately thin: the counters here
classify each cell by the *first* branch it matches, and the plant
branches sit at the end of the dispatch chain, so this method
systematically under-reports them. Pricing them properly wants
measure-by-deleting per mechanism, the way the eleventh attempt priced
the reaction mechanisms - which is a round of its own, once the
behaviour has stopped moving.

### What this attempt is worth carrying

**The round's designed optimisation measured zero, and its real finding
came from a comment that turned out to be false.** Reading the gate a
comment claimed to implement, and checking whether it could ever close,
found more than the skip structure the round was built around.

**Counting what a pass could skip is not the same as counting what
skipping would save.** 19.3% of cells were skippable and skipping them
was worth −0.1%, because the cells were cheap. Ask what the skipped work
*costs*, not just how much of it there is.

**A count taken at pass entry is not a count of what the pass does,
when the pass mutates as it walks** - the fire cascade row above is
exactly that trap.

**Measure-by-deleting keeps earning its place.** The biggest number this
round found came from stubbing one function and reverting it.

---

## The thirteenth attempt: two scenes for the shape of load nothing measured

Everything the suite had a benchmark for by the twelfth attempt was a
transient of one kind or another - a burst of activity that ran its
course and then went quiet. Nothing measured a heat source left running,
and nothing had put the thermal shock mechanism in front of the
reactions pass at any scale bigger than a single pane. This attempt adds
two scenes, committed together because they are designed as a pair and
say so in each other's source comments: a burst of damage that runs its
course, and a sustained steady state that has to keep producing without
exhausting either its fuel or its water.

**The most recent device capture is still the one the eleventh and
twelfth attempts already called stale, and this attempt did not add a
new one.** Nothing below is device-verified. Every number in this
section is host-only.

### Scene one: the thermal shock lattice

The lattice is 480 glass-ringed compartments, 20 columns by 24 rows of
9x9 tiles on the 184x224 grid. Each ring is 20 cells - the perimeter of
a 6x6 square - well under `crack_run()`'s `CRACK_MAX` of 256.

Every ring is painted at variant 2, 3 or 4 - strictly between
`SAND_SHOCK_COLD` (1) and `SAND_SHOCK_HEAT` (5) - for both families
identically, so no compartment is born already qualifying for a crack.
An earlier draft used an asymmetric range reaching down to 0 and 1 and
had one family born pre-shattered, through the shock direction it was
not meant to be exercising.

The left ten columns pair a burning-wood trigger outside the ring with a
cold payload (ice or snow) inside; the right ten pair an ice trigger
outside with a hot payload (burning wood or lava) inside. The family
split is a payload/trigger matrix, not proof that either half exercises
one direction only. Cross-contamination is real physics here, not a
scene bug: each ring's own contents conduct around the ring (glass
`conducts` is 220), so both shock directions run in both halves from
step 1. Measured on step 1, the intended-direction counters read 14
(left, cold arriving at hot glass) and 26 (right, heat arriving at cold
glass); the unintended ones read 12 and 10 in the same step.

What proves each direction fires is a pair of counters in the host test
that read the two crack branches' own preconditions directly - glass at
or above `SAND_SHOCK_HEAT` with a cardinal neighbour that chills
(`step_one_cold_cell()`), glass at or below `SAND_SHOCK_COLD` with a
cardinal neighbour that is burning (`try_heat_transform()`). Both
branches are roll-free once their precondition holds, which is what
makes a precondition count meaningful rather than a probability. Both
stand on all ten measured steps.

Ten measured steps, and the reason is the point of the scene: new cullet
has to keep arriving across the window. Charging each step's new cullet
to a third of the window - steps 1-3, 4-6, 7-10 - ten steps split 54.1 /
28.9 / 17.0 per cent (8,061 / 4,313 / 2,535 cells of 14,909). Twelve,
fifteen and twenty steps put the last third at 11.8, 11.5 and 13.6 per
cent as the first crack wave takes over more and more of the total; eight
steps split honestly into thirds leave it at 11.0. Ten is the only window
measured where the tail still clears 15%.

The cullet tally needs a sticky "was ever cullet" mask rather than a live
count: sand's `heats_to` is glass, so a fallen shard near a hot payload
re-fuses and can crack again, and a naive per-step delta on a live count
goes negative exactly when the scene is busiest.

At the end of the window: 2,568 cullet cells in the left half and 3,618
in the right, spread across 190 of 240 left-hand compartments and 238 of
240 right-hand ones; 2,362 water, 2,253 steam, 4,329 fire, 3,414 glass;
the cell count has grown from the 23,040 painted to 29,435 (burning wood
and lava flare fresh fire into empty neighbours, so the assertion is a
floor, not conservation).

The left half's rings do not melt inside the window, and that is
measured rather than assumed: the first left-half lava cell appears at
step 16, and there are 123 of them by step 40. The right half's own hot
payloads do legitimately melt some of its glass - that glass reaches the
top of its heat ramp inside the window.

The scene grows no plants and the host test pins that, in line with the
standing decision to keep the plant materials out of perf scenes. The
pin has to name `MATX_PLANT` specifically rather than reuse the lava
stress scene's plain `cell_is_extended()` form, because this scene
paints ice - an extended cell - on purpose.

### Scene two: the boiler

The boiler from `test_the_boiler_end_to_end` scaled from one column to
the whole grid: 3-cell stone side walls the full depth of the basin, an
11-row stone slab, 30 rows of water above it, and 4 rows of burner
underneath - lava on the left half, burning wood on the right.

Eleven rows is the pour brush's real thickness, the same figure the
small end-to-end test uses. `conduct_heat()` attenuates at about 0.86
per cell of depth it crosses, so slab thickness is the throttle on the
boil rate: eleven rows is what makes the rate last the window instead of
exhausting the basin partway through. Measured, a one-row slab drops the
basin to 2,570 cells of water by the end of the same window.

Two burners on purpose: lava never decays, so it is the steady source;
wood burns down (`burn_decay` 24) so the ember path is covered by the
same scene. All 356 wood cells painted are still lit at the end of the
window.

Twenty settle steps, then thirty measured, sampled at 0, 7, 15, 22 and
30 steps in. Twenty rather than the ten the four-liquid scene uses
because a basin takes longer to reach a steady boil than a liquid stack
takes to start mixing: at ten steps the board is still filling with its
first flush of steam (295 cells), at twenty it is boiling at a rate that
then holds.

The four quarters lose 206, 211, 215 and 157 cells of water. 3,958 cells
are left at the end - 83% of the 4,747 the window started with, 74% of
the 5,340 painted. Steam goes 593 to 1,445, and the first steam above
the basin's rim appears late in the window, reaching 80 cells by the
last step. Both halves boil at the same rate within five cells (392 lost
on the lava side, 397 on the wood side).

The heat-holder count is constant at 2,228, exactly the stone count, for
every step of the window.

The cell count is asserted as a floor, not an equality, and that is a
finding rather than a preference. The enclosure leaves flare almost
nowhere to put fresh fire, and the count does sit exactly at its
window-start 8,280 for the first twenty steps of the window - then the
boil opens gaps in the water above the slab, flare reaches them, and it
climbs to 8,343 by the end. At the shorter ten-step settle an earlier
draft used, exact conservation held by a single step: total step 41 is
where the count first moves, and that draft stopped at 40.

### The ceilings

There are now five provisional sanity ceilings in the file, not three,
and thirteen device frame-budget tests, not eleven. The five: four
liquids 150,000 µs, lava stress 150,000 µs, screen of smoke and steam
400,000 µs, thermal shock lattice 400,000 µs, boiler 80,000 µs. None has
ever run on hardware; all five must be re-pegged at about 9-10% over the
measured number the first time they do.

What is new is how the two new ceilings were chosen. They are not
extrapolated from the host timings - this file already records one
extrapolation that came out four times too pessimistic - and they are
not chosen from the host figures at all. The step counts are fixed by
the host guards, and the ceiling is then whatever keeps the whole run
inside the device's five-second task watchdog with a second to spare:
ten steps at 400,000 µs is four seconds, fifty steps (20 settle + 30
measured) at 80,000 µs is four seconds. The step count and the ceiling
are one decision; raising either alone breaks the arithmetic. An earlier
draft of the boiler used a round 100,000 µs, which at fifty steps is
exactly the watchdog's own five seconds, which is not a margin.

### Host timings, and what they are worth

Best-of-5 with the five scenes interleaved in one session, on the same
host harness the twelfth attempt characterised as drifting up to 40%
within a single run: thermal shock lattice 1,199 µs/step, lava stress
824, four liquids 763, smoke and steam 647, boiler 218. A relative
signal only - the lattice is the most expensive of the five and the
boiler the cheapest - and deliberately not converted into a device
prediction.

### Verification by breaking

Both host guards were checked the way this suite requires: 24 of their
33 assertions were individually watched fail, most by breaking the scene
rather than the threshold - removing every chilling material kills the
cold-onto-hot counter; painting every ring at the same temperature
instead of the {2,3,4} spread drops the last third below its floor
(which is what makes the spread a stagger lever rather than decoration);
a one-row slab exhausts the basin; a lava-only burner leaves the ember
assertion nothing to find; stretching the window to twenty steps trips
the no-melt assertion at step 16. The nine not individually reddened are
the two tests' warming-gate assertions (they can only go red on a
regression in the simulation's own `may_have_*` bookkeeping, which is
exactly their purpose) and one right-half water-loss assertion that is
the mirror expression of the left-half one, which was reddened.

### The first device run measured nothing at all

The first device capture of these five benchmarks,
`launcher/tools/results/performance_20260826_144811_raw.txt`, produced
no numbers at all - not a bad number, no number. At that point the
thermal shock host guard's `ever_cullet` mask was still a byte per
cell, so the guard's fixture asked for 82,456 bytes - the 41,216-byte
grid, its 24-byte block map, and a second 41,216-byte buffer for the
mask - against the roughly 68,188 the device has free once the display
framebuffer is carved out of its heap. The third malloc came back
NULL, the test's `TEST_ASSERT_NOT_NULL` aborted straight past its own
frees, and the resulting 41,240-byte leak starved every allocation
after it for the rest of that boot: all thirteen frame-budget tests in
the run failed on a null grid, not on a bad frame time, and the
capture that was supposed to characterise this attempt's two scenes
came back empty. The host guards here are registered the same way as
every other test in this suite - unconditionally, inside the self-test
image the board itself boots - so a fixture sized for host convenience
is really sized for the board whether it meant to be or not, and this
attempt's own guard is what broke it.

### What this attempt is worth carrying

**A window length can be a measured decision rather than a round
number**, and here it was the only one that kept the tail of the
timeline honest.

**Bucketing arithmetic and window length are one decision, not two:** an
earlier draft graded a 2/2/4 split as if it were thirds, and the
assertion still passed.

**An exact-conservation assertion that holds by one step is not an
assertion, it is a coincidence with a countdown on it.** A floor said
the true thing.

**Cross-talk between two halves of a scene can be real physics rather
than a scene bug;** the fix is to assert the intended direction fires,
not that contamination is zero.

**A guard that runs on the host runs on the board too, unconditionally,
and has to fit the board's budget even though it was written and
proved out on a machine where that budget does not exist** - the mask
above passed every host run right up until the device tried it.

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
