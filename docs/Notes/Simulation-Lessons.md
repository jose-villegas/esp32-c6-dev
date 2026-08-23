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
  whole screen is moving.~~ **No longer holds.** That was sand alone, before
  water existed as a material. Measured since: `sand_step` (powder) ~5.5-6 ms
  worst case, but the liquid cross-flow and rebound pass now costs up to
  ~15 ms against its own 16 ms budget - water, not sand, is the actual
  bottleneck whenever a body of it is moving. If more simulation performance
  is worth chasing, that pass is where it is.
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

## Related

- [`../Sand-Simulation.md`](../Sand-Simulation.md) — how the simulation works
  today; this file is how it got there.
- [Display-and-Rendering.md](Display-and-Rendering.md) — the dirty-tracking
  machinery this depends on.
- [Input-and-Sensors.md](Input-and-Sensors.md) — the tilt and shake signals
  consumed here.
