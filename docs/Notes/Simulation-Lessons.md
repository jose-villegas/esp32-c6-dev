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
  numbers above) 15733 us. Both still over budget, both real regression
  guards in `suite_sand.c` now, and both improved substantially without yet
  being solved - there is more here if anyone picks it back up, and the
  section below is the place to start rather than re-deriving the same
  three ruled-out theories again.
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
(still over 8000 us). All four are real, load-bearing regression guards now
(`suite_sand.c`'s frame-budget tests), not fixed - there is more here if
anyone picks this back up.

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
