# Shading and Colour

How a cell's material and variant become a pixel, what has gone wrong doing
that, and what is still unfinished - written so a fresh session (yours, or
another agent's) can pick up shading or colour work on *any* material
without re-discovering any of this from scratch. Read
[`Sand-Simulation.md`](Sand-Simulation.md) first if you have not; this
assumes you already know what a cell byte and a material row are.
[`Adding-a-Material.md`](Adding-a-Material.md) is the sibling document -
the checklist for adding a whole new material. This one is narrower and
deeper: it is entirely about how an *existing* material is painted, the
traps specific to that, and the one item still open.

Everything below was paid for on the device, not reasoned from a desk.
Where a number appears, it was measured - either on a host-side probe
against the real palette and the real `material_colours()`, or on the
board itself. Several sections describe a design that shipped, was found
wanting on the device, and was replaced - said plainly, because the
replacement is usually more instructive than the thing it replaced.

---

## The pipeline, as it stands

One byte per cell: a material id in the high nibble, a variant in the low
one. `material_palette()` (`material.c`) is a flat, `const` 256-entry
table indexed by the *raw cell byte* - for a `MATERIAL_FLAT` material,
painting a cell really is one array read, no branch, no arithmetic. That
is the fast path and it stays free; everything below is about the cells
that need more than that.

`material_colours(cell_t c, unsigned hash, unsigned mask, unsigned depth,
gfx_color_t out[3])` is the one function every non-trivial cell goes
through, called from `paint_row_n()` in `app_sand.c` - the hottest loop in
the app, painted per cell per dirty row. Its four inputs, and what each
one costs to produce:

- **`hash`** - `material_grain_hash(cx, cy)`, a stable per-cell scramble so
  a speckled material shows the same grain in the same place frame to
  frame. Computed once per cell, cheap (a couple of multiplies and
  shift-xors).
- **`mask`** - an 8-bit "which of my neighbours are empty" reading: 4
  cardinal bits (`MATERIAL_EDGE_LEFT/RIGHT/UP/DOWN`), unconditional for
  every cell, and 4 diagonal bits computed only for a water cell that is
  already a rim (see the foam lesson below for why that gate exists and
  why it is safe).
- **`depth`** - a liquid's own notion of how deep this cell sits inside its
  body of liquid, 0 (surface) to 255 (deep), used only by a liquid's
  interior. See the local-depth lessons below; this is the one signal that
  has been rebuilt the most times this session, and the backlog item at
  the end is about it.
- **`out[3]`** - body / diagonal-line / crossing colour. A flat or
  speckled material sets all three the same; `MATERIAL_HATCHED` (glass) is
  the only pattern that uses all three for real.

The return value, `material_pattern_t`, is `MATERIAL_FLAT`,
`MATERIAL_SPECKLED`, or `MATERIAL_HATCHED` - purely a hint to the painter
about how much per-pixel work this cell needs, never read by anything
else.

**The cost discipline that governs every change in this file:** `depth`,
`hash`, and the diagonal half of `mask` are each computed *once per frame*
where possible (a gravity-derived value, a blend weight) and reduced to a
plain per-cell read, comparison, or multiply-add-shift - never a per-cell
divide, never a per-cell trig call, never a second full-grid pass. Every
lesson below that adds a signal says, explicitly, what it cost per cell
after landing; if you add one and cannot state that cost in one sentence,
you have probably not looked hard enough for the frame-level version of
it.

---

## What the variant means, and why that decides how it paints

The low nibble's meaning is fixed per material *kind* (see
`Sand-Simulation.md` for the full table) and that meaning is *why* each
material paints the way it does - not a separate, independent choice:

| Kind | Variant means | How it paints |
|---|---|---|
| Powder (sand, snow) | a shade, picked once when painted and carried with the grain | `MATERIAL_SPECKLED`, indexed by `hash` |
| Liquid (water, oil, lava, acid) | a fill level, 1-15 | flat body colour in the interior, fill-indexed + specular on the rim (see below) |
| Transient (gas, fire, steam, ember) | life remaining | flat, indexed straight by variant - dying is one end of the ramp, fresh the other |
| Glass | heat, 0-15 | `MATERIAL_HATCHED`; the ramp doubles as a visible temperature gauge for free, because the palette was already indexed by the whole cell |
| Stone | a texture temperature/speckle hybrid | `MATERIAL_SPECKLED` |
| Soil | tone (top bit) + moisture (low three) | two wetness ramps, selected by tone |

The recurring mistake this table exists to prevent: **treating a variant's
colour ramp as if it meant something the variant does not actually carry.**
Every lesson below that involves a liquid is a version of this same error -
building a ramp around "depth" or "how much is here" when the variant is
actually recording something else (a transient fill level, a solver
residual, a screen position), and the ramp only ever looking right by
accident, in the one demo scene someone happened to test it against.

---

## Lessons

### A speckled hash needs a real avalanche, or it stripes

`material_grain_hash(cx, cy)` used to be one shift-xor of two multiplied
words. Measured over a 128x128 area, its low three bits - the ones every
speckled material actually reads - came out *nearly constant along a row*:
stone and wood were drawn in flat horizontal stripes with no texture at
all, reported as "the shading seems related to screen position, the
pattern repeats." One shift-xor is not enough for every output bit to
depend on every input bit; it needs a real finalising round (multiply by
an odd, high-avalanche constant, then another shift-xor). After the fix,
adjacent cells shared a shade 2077 times out of 16256 against an ideal of
2032 - close enough that stone finally looked like stone.

**Generalises to:** any new per-cell scramble. Do not assume a hash is
good because it compiles; measure adjacency correlation over a real grid
before trusting it to speckle anything.

### A solver-driven variant can carry almost no information at rest

Water's fill level is 15 (full) for the entire interior of *any settled
pool* - measured directly: 0 of 747 interior cells were anything but full
at 40 degrees settled, 0 of 720 settled flat, and only about 5% varied
even mid-tilt. A ramp built on "shallow is pale, deep is dark" (the
original design) was therefore painting almost nothing, almost all the
time - the fill level only ever deviates from full at a *surface*, in
transit, and even then briefly. Worse: while a pool is actively levelling,
neighbouring interior cells can land on wildly different fill values for
purely numerical reasons (see the next lesson), so an interior ramp built
on fill level does not read as noise-free depth, it reads as a "comb" -
alternating full and non-full cells - the exact bug commit `6a05faa`
fixes by making a liquid's *interior* paint one flat body colour regardless
of its own fill, and reserving the fill ramp for the *rim*, the one place
fill level is actually meaningful.

**Generalises to:** before building a colour ramp on any variant, measure
what that variant's real distribution looks like across an actual settled
scene, not a single hand-picked cell. A ramp tuned against one convenient
value can look completely broken across the range the variant spends 95%
of its time in.

### Coverage and depth are different things - keep them apart in a ramp

A related but distinct trap, hit while fixing the tilted-pool "staircase"
bug (surfaces snapping to 45 degrees, `ef87042`, and its shading
follow-up): water's palette ramped shallow-pale to deep-dark, but painted
a *partially-filled* cell (a real surface, not a depth) at full opacity in
its "pale" colour - so a 1/15-full cell rendered as the single brightest
thing in the pool, brighter than a full one. The fix that was tried and
then reverted was `FILM()` - compositing a partial cell against the
background by coverage, `MIX(BACKGROUND, body, floor + (1-floor)*fill/15,
256)` - which is the geometrically correct answer for "how much of this
pixel is liquid at all," and is preserved, working, on the
`water-wave-fog-depth-banked` branch (see below) for reuse if a similar
coverage problem comes up on a different material. It was reverted here
specifically because the *comb* fix above made it unnecessary for water's
interior (which now never reads fill level at all) while still wanting the
rim to show its true fill-based shade rather than a coverage blend.

**The distinction worth keeping:** "how much of this pixel is filled"
(coverage - use compositing, `FILM()`-style) is not the same question as
"how far into this body of liquid is this pixel" (depth - use the local-
depth mechanism below). Reaching for the wrong one produces a ramp that is
internally consistent and still wrong.

### A cheap "is this disturbed" signal can come from shape, not motion

Foam needed to gather at crevices and break over protrusions on a water
rim, without any new simulation state. The answer: count how many of a
rim cell's 8 neighbours are empty. Exactly 3 is a straight edge;
`abs(count - 3)` is curvature, concave below 3, convex above. Measured on
real water: a still, flat pool has non-flat rim on only 4% of its cells; 2
steps into a 75-degree tilt, 94%. **No motion flag is read anywhere** -
curvature alone is already an excellent disturbance detector, because a
calm surface is smooth by construction and a sloshing one is jagged along
its whole length. This also gave a waterfall its foam for free: a lip
(convex, curvature ~2), the falling stream's edges (convex), and the
plunge point (concave, curvature ~1) all foam without a single line of
code written for "waterfall."

**Generalises to:** before adding a state flag to drive a visual effect,
check whether the *shape* the simulation already produces already encodes
what you were about to flag by hand.

### Screen-position anything reads as a sheen; shape-following reads as depth

Water's interior darkening was, for one iteration, a pure function of
screen position (`row_start + col*step`, an affine ramp across the whole
grid). On the device this read as "almost like platinum" - a uniform
gradient swept across the *screen*, with no idea where the water actually
was, is exactly what a metal sheen looks like, and it painted straight
through an obstacle sitting inside the pool as if the obstacle were not
there. The fix (`f9679df`) is **local depth**: 0 at the boundary of this
cell's own material, one more than the neighbour's own value otherwise -
walked along gravity's dominant axis, using a persistent per-column array
(`col_local_depth[]`, `GRID_W_MAX` bytes) that survives across
`paint_row_n()` calls the same way `row_has_shine[]` already does.
Verified against an irregular pool (a rock plug inside it): the shading
correctly dips right where the rock breaks the surface, instead of
sweeping past it.

**Accepted, deliberate cost:** only *dirty* rows repaint, so a column's
stored depth for a row that has not redrawn in a while reflects whatever
the puddle looked like the last time it did - not a bug, and it costs
nothing extra to accept, because a region that is not redrawing is a
region that is not changing shape either. The one place this matters is
right at an axis transition (see the next lesson).

### A single dominant axis is an approximation with a seam - hysteresis hides the seam, it does not remove it

Local depth's first version picked ONE axis - vertical or horizontal,
whichever gravity's magnitude favoured - each frame. At exactly 45
degrees the two magnitudes tie, and a hand holding a device at that angle
does not hold it perfectly still: modelled realistic wobble flipped the
axis on 6-16% of frames, each flip wiping the whole depth array and
swapping between two *different measurement schemes* (a column's own
running total vs. a row's) - "fighting multiple values," reported almost
verbatim.

**First attempt, `0f09801`:** hysteresis - a Schmitt trigger requiring the
other axis to win by 15% before flipping. This reduced the flip
*frequency* to zero under realistic wobble. It did nothing for the flip's
*severity*: vertical-mode and horizontal-mode depth are different
quantities computed from the same grid, disagreeing by a measured mean of
3.4 cells and a max of 11 near any obstacle - against only `DEPTH_RANGE`
(4) visible brightness steps, an 11-cell disagreement landing on every
affected cell at once, however rarely it fires, still pops. Leaving a
device sitting near 45 degrees long enough guarantees at least one flip
eventually, because real sensor noise is not perfectly bounded.

**Second attempt, `05caadb`, current:** stop switching at all. Compute
*both* vertical depth and horizontal depth for every liquid cell, every
frame, unconditionally, and blend them by a continuous Q8 weight -
`256 * |gx| / (|gx| + |gy|)`, one divide per *frame*, then one
multiply-add-shift per *cell*, no per-cell divide. Modelled at the same
worst-case, 11-cell-disagreement cell: sweeping gravity 30 to 60 degrees
in 3-degree steps moves the blended value by at most one shade index per
step - a crossfade, not a pop. This also directly answers "single source
of truth": the blend weight depends on nothing but gravity itself,
continuously - no boolean state that can fall out of step with it.

**What this does NOT yet fix - see the Backlog section below.** The blend
interpolates between two *cardinal-axis* measurements (straight up/down,
straight left/right); it does not measure along the true diagonal gravity
actually points. That is deliberately left open, not overlooked.

### Any distance-based effect must be scaled to what it is actually measuring

A `depth_q` formula divided by 255 throughout the fog/wave era, because
depth used to be a screen-position value that legitimately spanned the
whole 0-255 range. Once depth became *local* (rarely exceeding a few dozen
cells for any real pool in a 224-row grid), the same formula stayed
pinned near one extreme for depth 0-20 - measured: luminance flat at 159
for local depth 0 through 20 cells, "covering essentially every visible
pool," only starting to move past depth 40. The fix was not a sign
correction, it was rescaling the denominator: `DEPTH_SATURATE_CELLS = 24`,
picked by modelling against water's own ramp so a realistic pool's actual
depth range (0-40 cells) spans the visible brightness range, instead of
needing to approach 255 to show anything.

**Generalises to:** whenever a signal's *meaning* changes (screen-position
to local, absolute to relative, whatever), re-derive every constant that
was tuned against its old range - do not assume a divisor survives the
signal it was calibrated for.

### Off-grid is solid, not empty - and screen y increases downward

Two conventions worth stating plainly, because both are easy to get
backwards and neither is optional:

- `sand_at()` reads an out-of-bounds cell as **stone**, never empty - the
  walls are solid for free, and `mask`'s cardinal bits (`paint_row_n()`)
  rely on exactly this: a wall lying against the screen edge does not get
  an outline there.
- Grid/screen coordinates have **y increasing DOWN the screen** throughout
  this app. `MATERIAL_EDGE_UP` is `-y`; a comment that says "above" always
  means `cy - 1`. Local depth's "toward the surface" direction and its row-
  order reversal both hinge on getting this sign right, and it is the kind
  of thing that is silently backwards until someone tilts the device
  upside down.

### A coarsened hash is safe only while it has exactly one consumer

Water's foam wants blobs bigger than a single cell, so `paint_row_n()`
hands water a hash sampled at `(cx >> FOAM_BLOB_SHIFT, cy >> FOAM_BLOB_SHIFT)`
instead of the fine per-cell one every other material still gets - no new
parameter, `material_colours()` stays ignorant of coordinates entirely.
This is only correct because **water has exactly one consumer of its
hash: foam.** Stone's speckle, wood's grain, and glass's hatch all depend
on the FINE, per-cell hash - adjacent cells disagreeing is the entire
point of a speckle. The day water gains a second hash consumer (its own
speckle, say), this coarsening has to move from "every water cell,
unconditionally" to "only where foam actually reads it," or the new
effect will silently stripe in blocks with no test catching why. Nothing
enforces this today; it is a fact about the file, not a type, which is
exactly why it needs saying here.

### Gravity-oriented specular already works here - a reusable reference

Water's rim gets a genuine gravity-driven highlight: `liquid_spec[]`, a
16-entry table (indexed by the 4-bit cardinal mask) filled once a frame by
`material_set_gravity()` from the current tilt, added as a shade shift to
the rim's fill index. It reads well enough on the device that it was
described as "almost like platinum" (in a context where that was a
compliment, before the interior blend fix above addressed the actual
complaint). **The project has separately wanted a gravity-oriented shine
for glass and not managed to land one.** This is a working example of
exactly that mechanism, at a known, modest cost - start here rather than
re-deriving a specular approach from nothing, if glass's shine comes up
again.

---

## Backlog: liquid depth still is not gravity-*continuous*

Banked explicitly, not forgotten. The current blend (`05caadb`) fixed the
discrete-switch pop and is a real, shipped improvement - but it is still,
underneath, an interpolation between two axis-*locked* proxies (a pure
vertical walk and a pure horizontal walk), not a true measurement along
the actual direction gravity points. At a strongly diagonal angle, each of
the two proxies is itself a coarse stand-in for "distance toward the
surface along this exact ray" - the blend smooths the *seam* between them,
but a softer, lower-magnitude version of the same axis-aligned bias can
still be there, because neither proxy ever steps diagonally.

**What "fully gravity-aware" would need:** genuine diagonal stepping -
walking each cell's local depth along the actual gravity ray rather than
along the screen's own x/y axes. `build_xflow()` (`sand.c`) already solves
a closely related problem for the *simulation's* own cross-flow levelling,
bracketing a true diagonal with two rays (the dominant axis and the
diagonal beside it) rather than snapping to one - the same idiom local
depth already borrows for its *axis choice*, but not yet for the per-cell
*walk itself*. That is very likely the shape the eventual fix takes:
either a genuine two-ray accumulation (more state, more per-cell work,
modelled cost unknown - has not been attempted) or some other continuous
walk that does not commit to the screen's own grid axes at all.

Before starting on this: re-read the "single dominant axis" lesson above
in full, re-run the same worst-case-cell modelling technique (an
irregular pool with an obstacle, swept through a range of angles,
measuring the actual rendered luminance step) against whatever new
mechanism is tried, and compare it numerically to the current blend's
already-measured one-index-per-3-degrees figure - a change here is only
worth shipping if it measurably beats that, not merely if it sounds more
correct.

The richer, reverted water-only version - fog colour blend plus animated
sum-of-sines wave bands, both driven by the pre-blend local depth - is
preserved, working, on the `water-wave-fog-depth-banked` branch (same
base commit the simplification in `fde5769` builds on). It is not the fix
for this backlog item - it was reverted because it read as a flat pale
wash on any realistic pool and because its wave bands were a separate,
resolved problem of their own - but the wave-table baking technique
(`tools/gen_wave_table.c`) and the fog-blend arithmetic are both real,
working code worth mining if a future effect (on water or elsewhere) wants
either trick again.

---

## Reference: gravity's numbers, and who actually reads them

- `gx, gy` are signed ints, produced exactly once a frame by
  `read_gravity_input()` (`app_sand.c`), which is `tilt_x()`/`tilt_y()` -
  the SAME smoothed pair, unmodified, feeds `sand_step()` (via
  `run_sim_steps()`), `material_set_gravity()`, and local depth's own
  per-frame setup. Verified directly by tracing the call sequence, not
  assumed - there is exactly one gravity source reaching shading, and it
  already goes through the tilt filter.
- Magnitude: `IMU_COUNTS_PER_G` is 4096; this session's own host-side test
  fixtures use gravity pairs of magnitude ~1000 (e.g. `(500, 866)` for 30
  degrees) as "the same order of magnitude," not an exact unit match -
  close enough that overflow/precision reasoning transfers, not so close
  that a test result should be read as a device-calibrated number.
- The tilt filter itself (`tilt.h`): `TILT_TAU_STILL_MS` 260,
  `TILT_TAU_MOVING_MS` 40 (an exponential-moving-average time constant,
  not a hard delay), `TILT_MAX_DT_MS` 100 (caps a stall from teleporting
  the filter), trust gate 70-130% of 1g, free-fall below 30%, shake
  threshold 50% of 1g, shake's own smoothing tau 120ms.
- **`gravity_quarter_turn()` (`app_sand.c`) is a SEPARATE, discrete (0-3)
  mechanism** - a plain snap-to-nearest-90-degrees, used only to turn the
  mode label's text to follow whichever edge is physically "up." It is
  not read anywhere in the shading pipeline and must not be confused with
  it; `display.h` has yet a third, independent orientation decision (with
  its own hysteresis) for the whole shell's rotation. If "orientation"
  comes up again, check which of these three is actually meant before
  assuming they are the same thing.
- The "pick the single dominant axis" idiom recurs project-wide -
  `build_xflow()`'s liquid cross-flow levelling, `sand_gravity_direction()`'s
  load direction, and local depth's axis choice all use it. It is a cheap,
  accepted approximation everywhere it appears, but every one of those
  call sites has a seam at the tie point; whether that seam needs
  smoothing (as local depth's did) or is fine left sharp (as the settling-
  angle fix's dithered-in-space approach turned out to be) is a case by
  case, measured judgement, not a rule to apply blindly.

---

## How to test a shading change

- **A host-side probe, no device needed.** Every investigation this
  session used the same shape: a small throwaway `.c` file linking
  `sand.c`, `sand_liquid.c`, `sand_gas.c`, `sand_reactions.c`,
  `material.c`, `row_runs.c` directly -

  ```
  gcc -std=c11 -O1 -I <main> -I <main>/apps/sand probe.c \
      <main>/apps/sand/{sand,sand_liquid,sand_gas,sand_reactions,material,row_runs}.c \
      -o probe -lm
  ```

  - call `material_palette()`/`material_colours()` directly to inspect
    exact colours and luminance;
  - build a real `sand_t`/`sand_step()` scene (a settled pool, an
    irregular one with an obstacle, a tilt sweep) to measure the actual
    signal a fix depends on before writing any code against it.
- **`panel_luminance()`** (`suite_sand.c`) is the Rec.601 luminance helper
  already used throughout the suite - reuse it rather than writing a
  second one.
- **`app_sand.c` is not linked into the host suite** (`run_tests.sh`
  deliberately excludes every `app_*.c`) - anything living only in
  `paint_row_n()`/`sand_frame()`'s own per-frame accumulators (the wave
  phase that was, the foam phase, local depth's row-order logic) needs a
  test-local mirror of the algorithm, not a real link, the same way
  `test_local_depth_follows_the_puddles_own_shape` and its neighbours do.
- **Prove every new cosmetic test load-bearing.** Every fix in this
  document was verified red-then-green: make the minimal edit that should
  break the fix, confirm the new test actually fails (and fails for the
  stated reason, not a compile error), restore the fix, confirm green. A
  palette or shading test that cannot be made to fail is treated as no
  test at all on this project.
- **The device is still the final judge.** Host probes catch regressions
  and prove a mechanism does what it claims; whether a colour or a
  gradient actually *looks right* is a question only the board can answer,
  which is why nearly every change in this document's history was flashed
  and looked at before being called done. Expect to do the same.

---

## Related

- [`Sand-Simulation.md`](Sand-Simulation.md) - the "why" behind the
  material table, the water model, and the performance discipline all of
  this sits inside.
- [`Architecture.md`](Architecture.md) - the single-page shape of the
  whole app, including the exact hops from a `.c` change to a real number
  on the device.
- [`Adding-a-Material.md`](Adding-a-Material.md) - the checklist for a
  brand new material; read that first if the material in question does
  not exist yet, this document second once it does.
- [`Simulation-Lessons.md`](Simulation-Lessons.md) - the same
  discovery-narrative format, for the simulation's own movement rules
  rather than how any of it is painted.
- `water-wave-fog-depth-banked` (git branch) - the richer, reverted
  fog/wave version of water's interior, preserved working for reuse; see
  the Backlog section above.
