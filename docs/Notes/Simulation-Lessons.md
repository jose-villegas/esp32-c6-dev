# Simulation Lessons

Part of the platform notes for the Waveshare ESP32-C6-Touch-AMOLED-1.8 - see
[`README.md`](README.md) for the full set.

This is the discovery narrative for the falling-sand app - the bugs found and
the reasoning behind each fix, in the order they came up. For how the
simulation actually works today, see [`../Sand-Simulation.md`](../Sand-Simulation.md)
instead; the two are deliberately kept separate rather than merged, since one
is a reference and the other is a history.

---

## Falling sand: what made it fit

The automaton runs a 184x224 grid (a cell per 2x2 pixels - a cell per pixel
would be 165 KB of grid, and the framebuffer has already taken 322 of the
chip's ~424 KB, see [Board-and-Memory.md](Board-and-Memory.md)). Worst case is
a grid half full of *falling* grains, where every one attempts a move; a
settled pile is far cheaper.

| Change | Step time |
|---|---|
| First working version, -Og | 9035 us |
| -O2 | 6664 us |
| Random number moved off the common path | **2857 us** |

3.2x in total, and the second change was worth more than the compiler.

The trick in the last one is worth remembering. The original drew a random
number for every grain, to decide which way to try sliding - but a grain in
open air just falls, and never needs it. Trying the gravity-ward move *first*
and only reaching for the generator when that fails skips it for almost every
grain. Two supporting changes: the three destination row pointers are computed
once per row instead of once per grain, and the bounds check became a single
unsigned compare.

---

## A variable framerate needs a fixed timestep

Worth writing down, because partial updates *created* this problem (see
[Display-and-Rendering.md](Display-and-Rendering.md)).

A grain moves one cell per step, so steps-per-second is literally how fast sand
falls. Stepping once per frame ties that to the framerate - survivable while the
framerate is flat, and not once it swings between 60 and 230 fps depending on
how much is moving. Sand would have fallen fastest when least was happening,
which is exactly backwards.

The fix is the standard accumulator: add the frame's elapsed time to a running
total, run whole fixed-size steps out of it, carry the remainder. Cap the
catch-up, or a long frame schedules extra steps that make the next frame longer
still.

The general lesson: **anything whose rate matters must be driven by elapsed
time, not by frame count.** The tilt filter already was, for the same reason -
see [Input-and-Sensors.md](Input-and-Sensors.md).

---

## Resting sand was the most expensive thing on screen

Adding a lot of sand dropped the framerate even though most of it was sitting
still - which is backwards, and the profile says why. A settled grain runs the
*whole* decision path every step to conclude nothing: the gravity-ward move
fails, a random number is drawn, its load column is walked, and both slides
fail. A falling grain succeeds on its first attempt and costs a fraction of
that.

Measured on a host, before any sleeping:

| Grid | Cost per step |
|---|---|
| empty | 0.015 ms |
| settled, half full | 0.163 ms |
| settled, screen completely full | 0.323 ms |
| half a screen actively falling | 0.020 ms |

A motionless screen cost **twenty times an empty one**, and sixteen times a
screen of falling sand.

The fix is the [sleeping-chunk idea](https://80.lv/articles/noita-a-game-based-on-falling-sand-simulation)
from Noita, applied per row rather than per chunk - sand settles in horizontal
layers, so rows are the natural grain here and the dirty-row machinery already
existed. A row is skipped once it has been examined with nothing to do, and is
woken again by any movement in it or either neighbour. A grain can only move
one row, so nothing further away can change what it is resting on.

On device, a completely full settled grid now costs **15 us per step**, against
3.9 ms for a screen of falling sand.

**The subtlety that broke the first attempt:** the gravity direction is
dithered, and the two directions it alternates between do not offer the same
moves. Straight down allows the slides (-1,+1) and (+1,+1); down-right allows
(0,+1) and (+1,0), and that last one is purely sideways. A row that had nothing
to do under one direction may well have somewhere to go under the other, so a
single "settled" flag froze grains on a slope. The state has to be one bit per
direction. A test that settles the grid with sleeping on and then re-runs it
with sleeping off, requiring nothing to move, is what caught it.

---

## Falling sand looked like a falling brick

Every grain in open air took the same move on the same step, so a poured blob
kept its shape all the way down and landed as a blob. Real sand disperses,
because no two grains fall at quite the same rate or in quite the same line.

`sand_set_scatter()` gives a falling grain a small chance of doing something
other than falling straight: either **lagging** a step, which spreads the
stream vertically, or **drifting** to one side, which spreads it horizontally.
Neither invents a move that was not already legal, so it cannot push a grain
through a wall or into another grain.

Two things worth keeping in mind:

- **It is off by default.** Most tests want to say "a grain falls one cell per
  step" and mean it exactly. The randomness is an aesthetic choice, so the app
  makes it rather than the simulation assuming it.
- **A lag must still count as activity.** A grain that *chose* not to move
  looks identical to a settled grain, so without care the sleeping optimisation
  puts its row to sleep and strands it in mid-air for ever. Caught by a test
  that turns scatter up to 200/256 and requires the grain to reach the floor
  anyway; confirmed to go red when the flag is removed.

---

## Anything with a rate must be driven by elapsed time

A third instance of the same bug, found by the framerate swinging after partial
updates landed. Pouring spawned sand once per **frame**, so holding a finger
down delivered three times as much sand when the screen was quiet as when it
was busy - and the sand arrived faster than the simulation could move it,
piling up under the finger. Now on its own fixed-rate accumulator at 60 Hz.

The list of things that have needed this: the tilt filter's smoothing, the
simulation's step rate, and now the pour rate. The rule is worth stating
plainly - **if it has a rate, drive it from `dt_ms`, never from frame count** -
because each time it has been missed the symptom looked like something else
entirely.

---

## A note on measurement noise

`sand_step` measured between 3.2 and 3.9 ms across builds that did not touch
that code path at all. The ESP32-C6 runs code from a 32 KB read-only flash
cache, so adding unrelated code shifts the layout and changes the hit rate.
Treat differences under about 20% as noise unless they reproduce, and leave
performance assertions enough margin that they are not a coin toss.

One thing worth being precise about, because it is easy to reach for
instinctively and does not apply on this chip: **there is no data cache in
front of RAM here.** The 32 KB cache above is an *instruction* cache for code
and constants running from flash-mapped `.text`/`.rodata` - it is why the
material table and the colour palette are `const` (see `material.h`), and it
is the whole reason the IRAM finding below measured nothing. The simulation
grid itself lives in DIRAM (plain heap SRAM) with no cache tier above it -
SRAM access already runs at the speed a cache would give on a bigger CPU, so
"lay the grid out for better cache hits" has nothing to bite on here. The only
way to go faster on the RAM side is to touch fewer bytes per step, which is
what the row-sleeping and `ROW_NO_LIQUID` skips already do.

**LP SRAM is not a faster tier either, in case that is ever tempting.**
Checked directly against Espressif's own docs rather than assumed: the 16 KB
LP SRAM region is "slightly slower to access" than regular DIRAM from the
main CPU, and exists for deep-sleep wake stubs, not as a performance
resource. Moving hot data there would be a regression, not a gain - and since
DIRAM sits at ~80% free, there is no capacity pressure to move anything cold
there either.

Two things NOT worth doing, measured or reasoned - true when written, no
longer entirely true for the first one, which is why it is worth restating
rather than deleting:

- ~~Micro-optimising the simulation further. At 3.2 ms worst case, against a
  blit that is usually now under 2 ms, it is the bottleneck only when the
  whole screen is moving.~~ **No longer holds, and this time it actually got
  chased** (see "`static inline` is a suggestion, not a guarantee" below for
  the full story) rather than staying a someday-item. ~~That was sand alone,
  before water existed as a material. Measured since: `sand_step` (powder)
  ~5.5-6 ms worst case, but the liquid cross-flow and rebound pass now costs
  up to ~15 ms against its own 16 ms budget - water, not sand, is the actual
  bottleneck whenever a body of it is moving.~~ **Also stale** - those numbers
  predate the row-to-block sleeping rewrite and the register-spilling fix
  below, both of which touch sand and water alike. Current reference points,
  same 16000 us water budget and a new 8000 us flip-gravity budget that did
  not exist yet when the above was written: water-collapse 22788 us,
  gravity-flip (a big settled pile, gravity reversed outright - the real
  worst case pouring and tilting produces, not the old single-material
  numbers above) 15733 us. ~~Both still over budget, both real regression
  guards in `suite_sand.c` now, and both improved substantially without yet
  being solved - there is more here if anyone picks it back up, and the
  section below is the place to start rather than re-deriving the same
  three ruled-out theories again.~~ Moved further still, and picked back up
  twice more since - see "A guard chain can cost more than what it
  guards" for the latest numbers (13053/19562-19703 us) and a dead end
  worth not re-trying.
- **IRAM placement of the hot loops.** Espressif
  [recommends it](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-guides/performance/speed.html)
  for hot functions, and the C6 runs code from a 32 KB read-only flash cache so
  it would help something - but not the part of the frame that actually costs.
  Still holds: this is about the *code* cache, unaffected by which function
  in `sand.c` happens to be hottest this month.

---

## Falling sand needs two kinds of friction, not one

The symptom was a floor of sand skating sideways on the faintest tilt. The
cause was that nothing in the rules distinguished "this grain is being poured"
from "this grain is being dragged".

**The angle of repose** is the one that fixes the reported behaviour. A grain
on a slope stays put until the slope exceeds the friction angle - the block on
an incline, `tan(theta) > mu`. Restated for a candidate move, which descends by
`m . g` and travels across the slope by `|m x g|`, it may only move when

    descent > mu * lateral

with `mu = 0.7`, about 35 degrees, which is dry sand. At zero tilt that permits
a diagonal topple while forbidding a purely sideways shuffle, and sideways only
becomes possible once the board is genuinely tipped. That is the difference
between sand tipping and sand being dragged.

It costs nothing: the test depends only on the direction, so it is evaluated
twice per step rather than per grain.

**Burial** is the second, and is what makes a deep bed behave differently from
a thin one. A grain counts how many grains are stacked against gravity above
it, and each one halves its chance of sliding; past five it cannot slide at
all. Falling is never affected - an unsupported grain drops whatever is on top
of it, which is what unsupported means.

Measured, pouring a bed down a slope well past the friction angle:

| Bed depth | Base ends at (with burial) | (without) |
|---|---|---|
| 4 rows | x = 3.0 | x = 8.1 |
| 1 row | x = 10.0 | x = 10.0 |

Burial moves the deep bed a long way and the thin bed not at all, which is
exactly the distinction it exists to draw.

One subtlety worth keeping. Load must be measured against the NEAREST gravity
direction, not the dithered one. The dithered direction looks up-and-left on
its diagonal steps, which reads empty above a vertical column - so a buried
grain was treated as a free surface grain about one step in eight, which is
more than enough to walk the base of a pile sideways. How much weight rests on
a grain is a property of the pile, not of how a particular frame rounded.

---

## A flat device, and why the simulation needs a throttle

Lying on a table, gravity points into the screen and the in-plane component
really is zero. Sand *should* stop - on a level tray it does - but it stopped
between one frame and the next, which reads as a crash rather than as settling.

The cause was not the sensor. **A grain moves one cell per step however strong
gravity is**, so the simulation had exactly two speeds: full and stopped. Sand
poured at the same rate down a 5-degree slope as a vertical one.

`tilt_strength()` supplies the missing dimension - how much of a g lies in the
plane, which is sin of the tilt (see [Input-and-Sensors.md](Input-and-Sensors.md))
- and the frame loop scales its step rate by it. That is the actual physics (a
grain on a tray is driven by `g·sin(theta)`), and it means tipping the device
flat brings the sand smoothly to rest.

It also disposes of an ill-conditioning problem quietly. Near flat, the flow
direction is the ratio of two noise values and is meaningless - but it is also
barely used, because the rate approaching zero is precisely what makes it
meaningless.

Free fall is a separate state, and needs the through-screen axis to detect: a
device lying flat and a device in free fall have *identical* in-plane readings,
and only the total magnitude tells them apart. In free fall the estimate is
deliberately held stale, so the flow rate has to be zeroed explicitly rather
than inferred.

Still not modelled: per-grain drag. Grains have no individual velocity, which
would need per-grain memory. Scaling the whole simulation rate covers what it
would have bought, and the catch-up cap (2 steps per frame) is the speed limit
- a grain moves one cell per step, so that cap *is* the maximum distance
anything can travel between two things the eye sees.

---

## `static inline` is a suggestion, not a guarantee - and guessing at why is a trap

Row-shaped sleeping (the section above) had the same blind spot as row-shaped
dirty tracking once did: an awake row forced a full-width walk over every
occupied cell in it, including deep, provably-stuck pile-interior grains, just
for sharing a row with something active at its edge - measured climbing
step cost ~2.2x over 45 s of continuous pouring while the *count* of awake
rows stayed flat. Generalising it to a 2D block grid (see the git history
around `sand_priv.h`'s `wake_blocks_points()`/`point_reach()` for the design)
fixed that - and then made the worst cases *worse*: a settled screen that used
to cost 300 us now cost 613 us, and reversing gravity on a big settled pile,
the actual worst case pouring and tilting produces, cost 17-21 ms against an
8000 us budget depending on which fix attempt was live.

Several plausible-sounding theories got ruled out **by measurement, not by
inspection** - the same discipline as the random-number finding at the top of
this file, worth restating because it kept paying off:

- `sand_load_above()`'s buried-grain load loop - bypassed entirely, timing
  unchanged. Not it.
- `try_slide()` as a whole - bypassed entirely, timing unchanged. Not it.
- The neighbour-wake logic firing too often - true, but even with it
  correctly gated to only fire when genuinely needed (an edge-aware
  neighbour reach, occupancy-checked), the cost barely moved.

What finally explained it needed a tool this project had not reached for
before: **disassembling the actual built object file.**

```
riscv32-esp-elf-objdump.exe -t sand.c.obj | grep wake_blocks
```

showed `wake_blocks_points()` and the `point_reach()` it called sitting in
their own `.text.point_reach` / `.text.wake_blocks_points` sections - real,
separately-compiled functions, despite both being declared `static inline` in
a header. `static inline` only *offers* a function up for inlining; at `-O2`
the compiler is still free to decline, and here it had, for both of them.
Disassembling `wake_blocks_points()` itself showed why that mattered: a
96-byte stack frame, ten callee-saved registers pushed on entry and popped on
exit (`s0`-`s9`), and two real `jalr` calls into `point_reach()`. Every one of
`bx0/by0/bx1/by1/lx0/ly0/lx1/ly1` had to survive that call boundary, and
"survive a call" on this ABI means "live in a callee-saved register, which
means the prologue must save it and the epilogue must restore it" - paid on
every single grain move, whether or not that move ever needed to wake a
neighbour.

`__attribute__((always_inline))` on `point_reach()` - not just `static
inline`, the actual forcing attribute - collapsed that to zero calls and a
32-byte, two-register frame. Confirmed by re-running the same objdump, not
assumed from the timing alone: the standalone `point_reach` symbol disappeared
entirely, folded into `wake_blocks_points()`. That cost ~1.9 ms off a 15.7 ms
worst case.

**It does not compound by repeating it.** Inlining `point_reach()` into
`wake_blocks_points()` made *that* function too large for the compiler to
keep inlining at its own call site (`mark_move()`), so it went back to being a
real out-of-line function - one level of the problem re-appeared one level up
the call chain. Forcing `wake_blocks_points()` inline too fixed that one.
Chasing the same trick one level further - forcing `mark_move()` inline at
all ~10 of its own call sites in `sand.c`/`sand_liquid.c` - was tried, and
made *everything* worse: full-occupancy went from passing to 10568 us over an
8000 us budget, and the flip test got slower too, not faster. `mark_move()`
inlined everywhere bloats the already-large inlined `sand_step()` past
whatever fits in the chip's 32 KB instruction cache (the same cache the
measurement-noise section above already identifies as the thing that made two
untouched builds measure 3.2 ms and 3.9 ms) - and the resulting cache misses
cost more than the saved calls were worth, on paths that barely touch this
function too.

There is a real ceiling on how far "just inline it" goes on this chip, and it
was found by measuring past it, not by reasoning about where it should be.
The rule this leaves behind: when a hot path crosses what should be a free
`static inline` boundary and the cost does not add up from the logic alone,
disassemble it before guessing further - and when forcing a call inline wins,
measure one more level up the call chain before declaring victory, because the
same problem can simply have moved rather than disappeared.

Net result of the whole investigation: full-occupancy passing again,
settled-screen 613 us -> 319 us (still over a 300 us budget), water-collapse
30454 us -> 22788 us (still over 16000 us), gravity-flip 17666 us -> 15733 us
(still over 8000 us). ~~All four are real, load-bearing regression guards now
(`suite_sand.c`'s frame-budget tests), not fixed - there is more here if
anyone picks this back up.~~ Picked back up - see "A guard chain can cost
more than what it guards" below for the next round, and its current numbers.

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
[Optimization-Playbook.md](Optimization-Playbook.md): the values are
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
very end of every step - roughly 644 blocks at the real screen size, a
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

---

## Related

- [`../Sand-Simulation.md`](../Sand-Simulation.md) — how the simulation works
  today; this file is how it got there.
- [Display-and-Rendering.md](Display-and-Rendering.md) — the dirty-tracking
  machinery this depends on.
- [Input-and-Sensors.md](Input-and-Sensors.md) — the tilt and shake signals
  consumed here.
- [Optimization-Playbook.md](Optimization-Playbook.md) — several of the
  findings above (the objdump-driven inlining fix, the register-spilling
  call boundary, the coarse-skip-structure shape, the test-memory-footprint
  bug) generalised into board-agnostic techniques.
