# Shading and Colour

How a cell's material and variant become a pixel and what has gone wrong
doing that - written so a fresh session (yours, or another agent's) can
pick up shading or colour work on *any* material without re-discovering
any of this from scratch. Read
[`Sand-Simulation.md`](Sand-Simulation.md) first if you have not; this
assumes you already know what a cell byte and a material row are.
[`Adding-a-Material.md`](Adding-a-Material.md) is the sibling document -
the checklist for adding a whole new material. This one is narrower and
deeper: it is entirely about how an *existing* material is painted and the
traps specific to that.

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
  body of liquid, 0 (surface) to `MATERIAL_LIQUID_DEPTH_BAND` (24, fully
  saturated), used only by a liquid's interior. Both per-axis accumulators
  saturate at that same fixed constant *before* they are combined, not at a
  byte's 255 - see "A value blended before it is clamped is blended on the
  wrong scale" below. The two axes are no longer *blended* by weight - each
  is *projected* onto gravity's own direction and the larger of the two is
  taken - see "Liquid depth is now gravity-continuous" below for why. See
  the local-depth lessons generally; this is the one signal that has been
  rebuilt the most times this session.
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
(now `col_stable_depth[]`, `GRID_W_MAX` bytes - the name at `f9679df`
itself was `col_local_depth[]`; see the "A dead array can survive its own
mechanism" lesson below for why the two names briefly, wrongly, referred to
*different* things in the same file) that survives across `paint_row_n()`
calls the same way `row_has_shine[]` already does. Verified against an
irregular pool (a rock plug inside it): the shading correctly dips right
where the rock breaks the surface, instead of sweeping past it.

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

**Second attempt, `05caadb`:** stop switching at all. Compute *both*
vertical depth and horizontal depth for every liquid cell, every frame,
unconditionally, and blend them by a continuous Q8 weight -
`256 * |gx| / (|gx| + |gy|)`, one divide per *frame*, then one
multiply-add-shift per *cell*, no per-cell divide. Modelled at the same
worst-case, 11-cell-disagreement cell: sweeping gravity 30 to 60 degrees
in 3-degree steps moves the blended value by at most one shade index per
step - a crossfade, not a pop. This also directly answers "single source
of truth": the blend weight depends on nothing but gravity itself,
continuously - no boolean state that can fall out of step with it.

**What this did NOT fix, and how it was actually closed:** the blend still
interpolated between two *cardinal-axis* measurements (straight up/down,
straight left/right) by weight - it never measured along the true diagonal
gravity actually points, and (a separate, later-discovered defect) blending
two axis *counts* has no notion of projecting onto the direction that
matters, so a fixed true depth read differently purely from tilt angle.
This was left open as a backlog item for a long time, on the theory that
the eventual fix would need genuine diagonal stepping (see `build_xflow()`
in `sand.c`). It did not: see "Liquid depth is now gravity-continuous"
below for the combiner that replaced this blend (`PROJECT then MAX`,
current) and closed both defects without ever stepping off the x/y grid.

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

### A value blended before it is clamped is blended on the wrong scale

The same lesson as the one above, one step further along, and it took a
device report to notice: rescaling the *renderer's* denominator to
`DEPTH_SATURATE_CELLS` (24) is not enough if the two things being averaged
into it are still free to run to 255 first.

`material_colours()` clamps `depth` at 24 - past that the panel cannot
tell one depth from another. But the vertical and horizontal accumulators
were blended *before* that clamp, so an unsaturated value got averaged
against one hundreds of cells past saturation, and the far one dragged the
result somewhere neither input would ever have rendered alone. A cell
sitting exactly *on* the horizontal surface (`hdepth` 0) inside a
full-screen-height column (`vdepth` 200) rendered at a blended depth of 7:
a surface cell painted as if it were seven cells under.

What made it visible was the dirty-row optimisation. `col_stable_depth[]`
is a *running* accumulator walked down a column - each painted row's value
is the previous *painted* row's plus one - which is only the cell's real
depth if the rows arrive as a contiguous chain from the boundary.
`draw_dirty_rows()` does not deliver one. A row repainted in isolation
inherits whatever row happened to be painted before it, so it renders a
value describing a different cell entirely while its neighbours still show
what the last wake tick left them: a one-cell-tall horizontal line across
the water, reshuffling every few frames. Reported as "creating lines
inside the water... huge jumps in the depth color".

Landscape-lock only, and that is the tell: there the pool stands against a
side wall spanning the whole screen height, so the vertical walk's range is
the grid's own height and beyond. A settled portrait pool is perhaps 30
cells deep, so a broken chain is wrong by at most 30 - modelled, portrait
shows zero such jumps with or without any of this.

Two things worth taking from the hunt itself. First, **the obvious suspect
was innocent**: the commit immediately before (`882f2e7`, resetting the
boundary debounce on a gravity-direction flip) was assumed to be the cause,
and running the model with and without it produced the same banding to two
decimal places - 2.81 against 2.80 shade transitions per water column,
across flip rates from 0.4 to 3.1 per second. It stays, because it still
does real bookkeeping work in pools *shallower* than the band, but it never
had the blast radius the report described. Second, **the metric has to
match the complaint**: every earlier harness in this chain measured the
frame-to-frame swing in the pool's *mean* depth, which is exactly blind to
a line - a mean does not move when brightness is shuffled between rows.
Only a spatial measurement (disagreement between vertically adjacent
interior cells, against a ground-truth render of the same frame) showed it.

The larger effect was not the one being chased. Dumping the rendered shade
index across the landscape-lock pool, one row per line (4 = surface, 0 =
fully saturated): unclamped reads `.....211111100000000.`, clamped reads
`.....333332222221111.`. The unclamped accumulator was pushing nearly the
whole pool past saturation, so the body rendered as one flat darkest tone
with a thin rim - and the little variation left in it was exactly the
row-to-row noise the report called lines. The device captures show the same
thing from the other end: one shade covering most of the water. That pool
had not merely been banded, it had lost most of the depth cue the feature
exists to provide.

`sand_priv.h`'s `mark_depth_band()` had *already been asserting* the fixed
behaviour as its reason for only dirtying a band of 24 rows around a new
surface: "anything further than that already saturates to the same flat
body colour whether the true depth is one cell more or a hundred". That was
simply not true of the renderer until this fix, so the simulation's
pour-staleness marking was under-marking against an assumption the renderer
broke.

**Generalises to:** clamp each input to the scale it is about to be
rendered on *before* you combine them, not after. And when two files share
a constant so they "agree by construction", check that they actually do -
one of them stating the invariant in a comment is not the same as either
of them enforcing it.

**This clamp stayed fixed at `MATERIAL_LIQUID_DEPTH_BAND`, unscaled, even
after the combiner it feeds changed shape** (blend → project-then-max; see
"Liquid depth is now gravity-continuous" below) - a *scaled* per-axis
ceiling was tried there and rejected after it measurably reintroduced this
exact bug. Worth cross-referencing rather than re-deriving: the reasoning
for why the ceiling had to stay put, once there was a reason to consider
moving it again, lives in that later section, not here.

### A dead array can survive its own mechanism, hidden by comments that kept describing it

By the time the vertical/horizontal blend above had been through several
rounds of fixes, `col_local_depth[]` (the vertical case) and
`h_running_depth` (the horizontal case) had quietly stopped doing anything:
each was still written every cell, every frame, but nothing downstream ever
read the value written into it again - `col_stable_depth[]`'s own climb, the
one that actually reaches the blend, read only *its own* prior stored value,
never the "raw walk" it was extensively commented as "riding on". Two
arrays computed side by side, one of them provably inert, for an unknown
number of refactors, because removing the axis-hysteresis switch and adding
the hold-then-commit debounce had each, separately, made sense on their own
terms without anyone re-deriving whether the array from the *previous*
design was still load-bearing afterwards.

**What let it survive:** the comments were confident, specific, and wrong.
They stated - repeatedly, in several places - that the debounced array
"rode on" the raw one, gave a reason it had to ("or an obstacle breaking a
pool's surface would take extra frames to reveal"), and cited a real test by
name as proof. None of that was true by the time anyone checked the actual
code the comments were describing; the reasoning had been accurate for an
*earlier* version of the mechanism and simply never got re-verified against
the version that replaced it. A comment describing intent, once written, is
never automatically re-checked against the code it sits beside - only a
human (or an agent) actually reading both side by side catches the drift,
and nothing about this file's structure forced that to happen.

**The test mirrors had the same disease, one level up.** The host-side
mirrors of this mechanism (`mirror_local_depth_column()`/
`mirror_local_depth_row()`, `suite_sand.c`) still implemented the *dead*
array's naive semantics - immediate reset on any disagreement, no
hold-then-commit, saturating at a byte's own 255 rather than
`MATERIAL_LIQUID_DEPTH_BAND` - because they were written against the
mechanism that existed when the test was first added and were never revisited
when the mechanism moved on. The tests were green throughout, which is
exactly the trap: a mirror that implements the *wrong* algorithm can still
agree with a *dead copy* of that same wrong algorithm sitting unused in the
production file, and the two wrongs cancel out into a passing suite that
protects nothing.

**Correcting the mirrors surfaced a real, previously unproven limitation,
not just a naming problem.** The hold-then-commit debounce's own "known
limitation" comment claimed a column with two persistent reset points (a
pool's surface, plus an obstacle) has the shallower one merely "lag its own
commit by a frame". Tracing the corrected mirror against the exact test
geometry proved that claim optimistic: two such points sharing one tracking
slot **never** commit, either of them, for as long as both persist - not a
one-frame lag, a permanent oscillation. It happened to read as a survivable,
constant-offset error in the specific geometry this file's tests use (the
non-liquid cells beside the obstacle reset the accumulator to a clean 0
immediately before the held request anyway), but re-deriving that from the
algorithm rather than assuming the old comment's softer framing was correct
is what caught it. A second, separate instance of the same limitation
(rather than a bug in the fix) was found while re-verifying a *different*
test's own regression-proving claim: with a stale test geometry that only
ever achieved a *held* value rather than a genuine commit, a hard
single-axis switch substituted into that test's own blend computation no
longer produced a big enough luminance jump to fail the assertion - the
regression test had gone silently blind to the exact regression it exists to
catch, caught only by re-running the "temporarily break it, confirm red"
proof this file's own testing convention already calls for.

**Generalises to:** when replacing a mechanism, explicitly ask "does the
thing the *old* design needed still get read by anything" rather than
assuming a leftover array is obviously still load-bearing because it is
still being *written*. A comment asserting *why* something is necessary is a
claim to re-verify against the current code, not evidence in itself - "the
comment says so" is exactly how this survived multiple rounds of
otherwise-careful editing. And a test mirror that duplicates an algorithm
instead of linking to the real one (necessary here, since `app_sand.c` is
not host-portable - see "How to test a shading change" below) needs the same
scrutiny the code it protects gets: read it against the CURRENT mechanism,
not just against whatever the mirror already agrees with, and re-run every
test's own "prove this load-bearing" substitution after any change to the
values feeding it - not only when the mirror itself changes shape.

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

### Gravity-oriented specular - two working implementations now

Water's rim gets a genuine gravity-driven highlight: `liquid_spec[]`, a
16-entry table (indexed by the 4-bit cardinal mask) filled once a frame by
`material_set_gravity()` from the current tilt, added as a shade shift to
the rim's fill index. It reads well enough on the device that it was
described as "almost like platinum" (in a context where that was a
compliment, before the interior blend fix above addressed the actual
complaint).

The project had separately wanted a gravity-oriented shine for glass and,
for a while, had not managed to land one - **that has since shipped**,
independently, as `material_shine_direction()`: a pure, stateless function
returning a Q8 unit vector (minus gravity, the same convention
`liquid_spec[]` uses) that turns glass and metal's `MATERIAL_HATCHED`
band from a fixed diagonal into one that sweeps with the tilt. Two
different mechanisms solving related problems - `liquid_spec[]` is a
precomputed per-mask table read by index, this is two numbers computed
once a frame and carried straight into the hatch's own per-pixel walk -
worth comparing both before reaching for either as a template for a third
material's shine.

---

## Closed: liquid depth is now gravity-continuous

This used to be the backlog item at the end of this document - "liquid
depth still is not gravity-*continuous*" - closed by replacing the blend
above with a different combiner, prompted by two device-reported defects
in the blend itself (not in the discrete-axis-switch it had already
replaced):

- **"Weird rectangles"** of wrongly-shallow water beside a submerged
  obstacle, flipping which side with the sign of `gx`. The blend still gave
  the axis whose walk got cut short by the obstacle a weight proportional
  to its own share of gravity, instead of zero, so a cell several columns
  away - genuinely no shallower than its neighbours at that depth - read
  shallower purely because one walk, not the other, hit the obstacle first.
  Measured on the device's own captures: 63.1 vs. 55.1 luminance beside all
  5 obstacles (about 11 cells of visibly wrong depth), mirrored to the
  opposite side when the tilt's sign flipped.
- **"It creates a band to one side or the other and the shade just gains
  length depending on which direction has more magnitude."** Each axis walk
  counts cells along that *axis*, not distance along *gravity* - blending
  two axis counts has no notion of projecting onto the direction that
  actually matters, so a cell at a fixed true perpendicular depth read
  differently purely from tilt angle. Modelled on an ideal planar surface,
  true depth 10 cells, swept 0-90 degrees from vertical: the blend reported
  10.0, 13.2, 14.6, 14.1, 14.6, 13.2, 10.0 - up to 46% inflation from tilt
  alone, no obstacle, no staleness, nothing about the water changing at
  all.

**The fix: project, then max, instead of blend.** Each axis's raw cell
count is scaled by that axis's own share of the gravity *vector's length*
(`256 * |gy| / im_len(gx, gy)` for the vertical count, `256 * |gx| /
im_len(gx, gy)` for the horizontal one - `im_len()`, `util/intmath.h`, an
integer approximate length, already used elsewhere in this app for the
same reason: no square root on a chip with no hardware divider) rather
than compared against the *other* axis's own share the way the blend's
weight was. The two projected values are then combined with `max`, not a
weighted average.

**Why max, and not a blend of the projections - this is the load-bearing
choice, not a stylistic one.** Each axis walk terminates at the first
non-liquid neighbour toward the surface, so its projected value is a
*lower bound* on the true perpendicular depth: it may stop early at an
inclusion rather than at the real free surface, but it can never
*overshoot*. Max-of-lower-bounds is the tighter estimate and recovers the
true depth as soon as *either* axis reaches it; averaging instead lets
whichever axis got blocked drag the combined result down, proportionally
to its own weight - exactly what produced the shadow. On an ideal planar
surface, both projections equal the true depth exactly (verified: 10.0 at
every angle 0-90, not merely close), so max is also exact there, closing
the tilt-inflation defect with no obstacle involved at all. It is also
still a crossfade at the 45-degree tie point that motivated the *original*
blend - both projections are continuous functions of gravity's own angle
and equal each other exactly at the crossing, so their max is continuous
too; re-verified against the same worst-case cell and the same 30-to-60
degree sweep the blend was first proven against (`test_the_blend_has_no_
jump_crossing_45_degrees`, kept its old name because it still pins the
same property).

**A third defect, found AFTER the two above had already shipped and every
test was green: a saturated cell's shade still "breathed" with tilt.**
This first shipped combiner clamped each axis's raw count at a FIXED
`MATERIAL_LIQUID_DEPTH_BAND` (24) *before* projecting - `depth =
max(min(vcount,24)*wv, min(hcount,24)*wh) >> 8`. That closes the shadow and
the under-band tilt inflation above, but a column genuinely saturated on
*both* axes still reports `(24 * weight) >> 8`, which is below 24 whenever
weight is under 256 - i.e. whenever gravity is not exactly aligned with one
axis. Measured through the real ramp, sweeping gravity 0-90 degrees against
a cell saturated both ways: luminance read 55.1 flat from 0-30 and 60-90
degrees, but 63.3 - one whole shade step brighter - from 35-55 degrees: the
*entire deep interior of the pool* visibly brightening and dimming as the
device rotates, no obstacle, no staleness, nothing about the water
changing at all. The two device screenshots that motivated this whole fix
were taken at 33.3 and 37.8 degrees - squarely inside that window.

**Two fixes for the breathing were tried and rejected before the one that
shipped - both are worth knowing by name, because both look obviously
correct and both fail the same regression test for different reasons.**

- *Scale the ceiling per axis* (`ceil(BAND*256/weight)`, capped at a
  byte's own 255), so a low-weight axis's count can climb further before
  clamping and still reach the band once projected: removes the breathing
  completely, verified against an ideal planar surface (10.0 at every
  angle, fixed ceiling or scaled). But scaling the ceiling up for a
  low-weight axis *also* scales up the absolute size of the swing a broken
  accumulator chain on that axis can produce (the same failure "A value
  blended before it is clamped" above bounds), before that axis's own
  weight gets a chance to damp it down - exactly cancelling the "low
  weight damps a corrupted value" protection the scaling relies on.
  Measured directly against the sparse-repaint regression test: 200 banded
  pairs in the worst frame, indistinguishable from the pre-fix (no clamp
  at all) case. Rejected.
- *Move the projection from combine-time to climb-time*: instead of a
  cell count, accumulate PROJECTED DEPTH directly, in eighths of a cell,
  climbing by that frame's own per-step increment and saturating at a
  fixed ceiling in those units - so a fully-saturated axis reads the same
  value at every angle, by construction. This also removes the breathing
  completely - but breaks something more fundamental: a plain cell *count*
  is gravity-agnostic (it means the same thing regardless of when it was
  accumulated), so projecting it fresh at combine time, from whatever the
  *current* frame's weight is, is always correct. An eighths value has no
  such property - each increment already has a specific frame's gravity
  baked into it, so a value built up while gravity pointed one way carries
  that angle, uncorrected, into a later frame where gravity has since
  rotated. Measured directly against the same regression test: 52 banded
  pairs (still failing), and critically, changing the ceiling this test
  pins made *no difference at all* (255, no clamp, and the eighths design's
  own cap both measured 52) - proving the regression was not about the
  ceiling's value but about gravity's angle *drifting* while a stale value
  was still live, confirmed by freezing the test's own gravity sway, which
  dropped the count to 0. Rejected.

**The fix that shipped keeps everything about the first (rejected) design
- a plain count, projected at combine time exactly as before - and changes
only the number the count saturates at**, raising `LOCAL_DEPTH_COUNT_CEILING`
from the band itself (24) to 34 - `ceil(BAND*256/183)`, enough headroom
that the projection still reaches the band at the worst (45-degree) tilt
angle, where a Q8 weight bottoms out around 183 of 256. Because storage
stayed a gravity-agnostic count, the eighths design's fatal flaw cannot
recur - nothing about a count depends on when it was accumulated - while
the raised headroom closes the breathing the same way the scaled ceiling
tried to, without the scaled ceiling's own regression, because the ceiling
is *fixed* (not scaled per axis, so it cannot inflate a stale swing's
absolute size). The combiner now needs an *explicit* clamp to
`MATERIAL_LIQUID_DEPTH_BAND` after the max - the raised ceiling means
`count * weight >> 8` can exceed the band now, unlike the original design
where it never could. Re-verified against every one of the three
alternatives, same regression test throughout: 255 and the eighths design
both measure RED (200 and 52 respectively), the scaled ceiling also
measures RED (200), the raised fixed ceiling measures GREEN.

The exact ceiling value has a floor below which the breathing becomes
visible again, worth recording next to the constant rather than only
here: `material_colours()`'s shade index is `4*(BAND-depth)/BAND`
(`DEPTH_RANGE` is 4, integer division), so a projected depth anywhere from
19 to 24 renders the *identical* shade as a true 24 would - the floor is
the ceiling value whose worst-angle projection still lands at 19, which is
27 (`27*183>>8 == 19`). Any future retune of `DEPTH_RANGE` or that integer
division changes this floor and must be re-measured, not assumed.

**Generalises to two lessons, not one.** First: combining two measurements
taken along different axes requires projecting them onto the axis you
actually mean *first* - a weighted average of the raw, un-projected
measurements is not the same thing and can drift with an angle that has
nothing to do with what is being measured; and when each input is a *lower
bound* that can terminate early rather than an unconditionally trustworthy
reading, `max` is the correct combiner where an average silently
propagates whichever input happened to be wrong. Second, learned the hard
way after the first fix already shipped: when a persisted value must stay
correct while the thing it depends on keeps changing between the moments
it is written and read (gravity's own angle, here, rotating between
frames), keep that value in a form that does *not* bake in the
circumstances of the frame that wrote it - a plain, context-free count
survives being re-interpreted later; a value that already has an angle
folded into it does not, and no amount of clamping fixes that after the
fact. The more "obviously correct" fix (project every step, not once at
combine time) was the one that broke this, and it took a second
regression test - one built to catch a *third*, unrelated defect - to
notice.

**An open methodological question, found while re-proving the shadow
regression test against the raised ceiling, worth recording rather than
silently working around:** that test compares two columns at the *same
screen row* to either side of a submerged obstacle, under tilted gravity.
Close to the obstacle (1-3 cells) the two read within 2-3 units, comfortably
under threshold. Swept further out, the gap does not converge back toward
0 the way a real shadow's own signature should - it keeps growing, past 7
units by 12 cells out, with real side walls added and ruled out as the
cause. The likely explanation: "same row" and "same distance along gravity
from the free surface" are only the same comparison when gravity points
straight down. Under a genuine tilt the two diverge, growing with
horizontal distance from wherever they happen to agree - which would
produce exactly this non-converging drift, independent of any obstacle. If
so, the test's own methodology (fixed-row comparison) measures an honest
geometric fact about tilted-gravity pools once the compared columns are far
enough apart, not a shading defect - and a test asserting two such columns
must render identically would itself be the wrong claim. The regression
test was narrowed to the distances where the obstacle's own effect still
dominates (1-3 cells) rather than resolved further; a cleaner isolation of
the shadow defect alone - comparing against the same scene rendered
*without* the obstacle, or measuring along a gravity-aligned line instead
of a fixed row - is banked for whoever next touches this test.

The richer, reverted water-only version - fog colour blend plus animated
sum-of-sines wave bands, both driven by the pre-combiner local depth - is
preserved, working, on the `water-wave-fog-depth-banked` branch (same base
commit the simplification in `fde5769` builds on). Unrelated to this fix -
it was reverted because it read as a flat pale wash on any realistic pool
and because its wave bands were a separate, resolved problem of their own
- but the wave-table baking technique (`tools/gen_wave_table.c`) and the
fog-blend arithmetic are both real, working code worth mining if a future
effect (on water or elsewhere) wants either trick again.

---

## Closed: the obstacle shadow now follows gravity, not the screen axes

The projection-then-max combiner above closed two defects (the shadow beside
a submerged obstacle, the tilt-dependent inflation on a flat surface) but
left a third that turned out not to be a combiner defect at all: the
shadow's own *direction*. Reported from the device after the combiner had
shipped and closed both of its own defects: the shallow region behind a
submerged obstacle still ran straight down or straight sideways, never
along the actual tilt.

**Why the combiner could never have fixed this.** Both walks the combiner
combines are axis-aligned - one counts cells straight up/down, the other
straight left/right - so whichever one an obstacle blocks resets to 0 and
climbs back up walking along that same fixed axis. No projection or
max-combiner downstream of that walk can rotate a shape neither walk ever
drew in the first place. This is structural, not a tuning problem: an
axis-aligned walk can only ever cast an axis-aligned shadow.

**Measured** - a settled pool with a submerged 3x3 stone dead centre,
comparing the shadow's own deficit-weighted centroid bearing against true
gravity's bearing, across six tilts (SHIPPED = the projection-then-max
combiner above; RAY = the walk that replaced it, below):

```
tilt from vertical   SHIPPED bearing        RAY bearing    true gravity
     33.1 deg          -90.0 (33.1 off)      -124.6 (1.5 off)   -123.1
     33.3 deg          +90.0 (33.3 off)       +57.6 (1.0 off)    +56.7
     37.8 deg          +90.0 (37.8 off)      +128.1 (0.2 off)   +127.8
     45.0 deg          no shadow at all       +45.0 (0.0 off)    +45.0
     16.7 deg          +90.0 (16.7 off)       +72.6 (0.7 off)    +73.3
     73.3 deg           +0.0 (16.7 off)       +17.9 (1.2 off)    +16.7
```

SHIPPED's bearing is always exactly +/-90 or 0, and its error always equals
the tilt angle - the axis-aligned signature stated plainly. At 45 degrees
SHIPPED has no shadow at all, not a small one: both projected axes reach
the true surface equally there, so `max()` hides the shadow completely -
which doubles as the reason the whole effect visibly appeared and
disappeared as the device rotated through that angle. RAY stays within 1.5
degrees of true gravity at every tilt tried, including the tie point where
SHIPPED has nothing to measure at all.

**The fix: one walk that steps along the gravity ray itself, by Bresenham,
replacing the two axis-aligned walks and their combiner outright** - not a
fourth combiner shape layered on top of the third. Two regimes, switching
at 45 degrees on `|gy| >= |gx|`, the same tie-point split the very first
(hysteresis) mechanism used:

- **Vertical-dominant** (`|gy| >= |gx|`): one ROW per step toward the
  surface. The cell one step back along the ray is `(cx + step, cy -
  vdir)` - `vdir = sign(gy)`, `step` a value shared by every column in
  that row (gravity does not change from one column to the next).
- **Horizontal-dominant** (`|gx| > |gy|`): the transpose - one COLUMN per
  step, source `(cx - hdir, cy + step)`, `step` now a per-cell value (0
  most cells, +/-1 wherever the ray's own diagonal drift crosses a row
  boundary).

**Why this regime switch is safe where the first mechanism's own axis
switch was not - the two are not the same shape of risk, even though both
are a discrete pick with a real seam at 45 degrees.** The very first
mechanism's two sides measured *different quantities* - a plain vertical
cell count and a plain horizontal cell count, related to the true depth by
two different, angle-dependent factors - so a flip between them was a jump
in the reported value itself, however rarely hysteresis let it fire (see
"Lessons" above for that mechanism's own history). Both regimes of the ray
walk measure the *same* quantity - distance along the gravity ray, in
cells - so a regime flip changes only *how* that quantity gets computed,
not what it means; the two regimes agree exactly at the 45-degree crossing
by construction, since both are walking the same ray there. Measured
directly, the same 30-to-60-degree sweep `test_the_blend_has_no_jump_
crossing_45_degrees` has used since the blend: worst single-degree step
across the crossing, the old projection-then-max combiner 1, this walk 0.

**Depth in cells is `count * |g| / |dominant axis|`, applied once, at
combine time, to a raw step count** - not baked into the climb itself. The
count staying a plain, gravity-agnostic count (never a pre-scaled
accumulator) is the one property every earlier shape of this mechanism
already proved load-bearing (see "A value blended before it is clamped" and
the eighths-accumulator rejection above) - it survives this rewrite
unchanged, for the identical reason.

**The ceiling needs no raise this time - a load-bearing difference from the
projection-then-max design's own scale, not an oversight carried over.**
That design's own weight was `component / len`, always <= 256 (minimised at
the 45-degree tie, around 183 of 256), so a count clamped at the plain band
could project to *below* the band at every angle except perfect axis
alignment - "breathing" - and the ceiling had to be raised past the band
(to 34) so a saturated count still reached it once shrunk. This walk's own
scale is the *reciprocal* shape, `len / dominant_axis`, always >= 256 (equal
to 256 only at perfect axis alignment, growing at every other angle) - so a
count clamped at the plain band *already* projects to at least the band at
every angle, and the combiner's own explicit clamp to
`MATERIAL_LIQUID_DEPTH_BAND` does the rest. Verified directly: a fully
saturated cell swept across the same 0-90 degree range `test_a_saturated_
liquid_body_reads_the_same_shade_at_every_tilt_angle` uses clamps to a flat
`MATERIAL_LIQUID_DEPTH_BAND` at every sample, ceiling equal to the band,
no raise at all.

**Storage: one shared pair of arrays now, not two** -
`local_depth_row_a[]`/`local_depth_row_b[]` (`app_sand.c`), a plain double
buffer pointer-swapped at the end of every row, replace `col_stable_
depth[]`/`row_stable_depth[]` together; `local_depth_top_row[]` replaces
`col_top_row[]`/`row_top_col[]` together. Net `.bss`: two `GRID_H_MAX`
(224-byte) arrays removed, three `GRID_W_MAX` (184-byte) arrays remain -
552 bytes against the old 816, roughly 260 bytes saved, not spent, plus a
handful of new per-frame scalars (~20 bytes: the frame's own `|gx|`/`|gy|`,
two pointers, two booleans) too small to change that direction.

**The debounce key means something different in each regime - found by
testing, not designed in advance, and worth recording precisely because it
is the one place a straightforward merge of the two old arrays does not
work.** Vertical-dominant keeps the *old* convention exactly:
`local_depth_top_row[cx]` holds the row index of column cx's most recent
boundary request, committing only once the *same row* asks again on a
later painted frame - this still works because `local_depth_row_a/b[]` is
column-indexed and a given column's own row-sweep visits that column's slot
roughly once per frame, so "the row" genuinely identifies a stable physical
location across frames. Horizontal-dominant **cannot** reuse that key:
unlike the vertical case, *every* row-call writes *every* column's slot (a
row-call always walks its own full width), so a row-indexed key compares
against a different row's own index on almost every successive write to
the same slot - a column sitting permanently beside a real wall would ask
for a reset from a different `cy` on every dirty row that touches it, so
`top_row[cx] == cy` would almost never match twice, and the debounce would
HOLD FOREVER instead of ever committing to the single most common case this
mechanism has to get right: a column beside a permanent wall. Measured
directly, reproducing exactly this geometry (a settled pool against a real
side wall, gravity mostly horizontal): a row-indexed key left the
wall-adjacent column's own reported depth stuck climbing indefinitely
rather than reading near 0. The fix: horizontal-dominant instead stores a
plain PENDING FLAG (any value other than 255 means "the immediately
preceding write to this slot was also a boundary request, not yet confirmed
a second time"), re-armed on every subsequent boundary request (so a
permanent wall commits on every row that touches it, not merely the first)
and explicitly cleared back to 255 on a confirmed same-material climb (so a
stale flag from an unrelated earlier blink cannot pre-arm a later, different
blink into an instant false commit). Both regimes share the *array*, not
the convention - a regime flip resets it wholesale, so the two conventions
never have to interpret a value the other one wrote.

**A regime flip gets the same kind of reset the two scan-direction flips
already had, and for the same reason**, extended to a third condition
(`update_local_depth_gravity()`, `app_sand.c`): the same array slot carries
a different recurrence depending on which regime is active, so a value left
behind by one regime is not simply "the same count under different
bookkeeping" to the other regime's own read pattern. Measured, host-side, a
single sharp jostle across the 45-degree line in the middle of an otherwise
steady near-horizontal hold: a small, self-healing blip immediately after -
12 banded pairs the very next measured frame, 6 one frame later, 0 the
frame after that (the periodic wake tick finishes the job) - bounded and
gone within two frames at 30fps, against an artificially instantaneous
gravity step no real hand-tremor input produces (the tilt filter's own
smoothing means a real crossing ramps through several frames, not one).

**Verification.** `test_a_submerged_obstacle_does_not_cast_a_depth_shadow`
became `test_a_submerged_obstacle_casts_a_gravity_aligned_shadow` - the old
name's own claim was the correct requirement for the projection-then-max
combiner and is now the wrong one; the shadow is a kept, deliberate feature,
and the new test asserts it is both present (unlike SHIPPED at 45 degrees)
and aligned with gravity within 5 degrees, at every one of the six tilts in
the table above. Every other local-depth test either transfers unchanged
(the ones exercised only at `gx == 0`, where this walk's vertical-dominant
regime with zero row-drift is exactly the old vertical-only mechanism) or
was rewritten against the new mechanism directly:
`test_the_blend_has_no_jump_crossing_45_degrees` (kept its name a second
time, still pinning the same no-jump-at-the-crossing property against
whichever mechanism ships), `test_a_fixed_depth_reads_the_same_at_every_
tilt_angle`, `test_a_saturated_liquid_body_reads_the_same_shade_at_every_
tilt_angle`, `test_a_sparse_repaint_does_not_band_a_tall_liquid_column`
(0 banded pairs, same as the design it replaced), and the wake-tick test
`test_a_settled_edge_does_not_flicker_stale_to_fresh`.

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
- **Match the metric to the complaint, and render a ground truth to
  measure against.** "Flickering" and "lines inside the water" are
  different measurements, and a probe built for one is blind to the other:
  the frame-to-frame swing in a pool's *mean* depth cannot see a line at
  all, because a mean does not move when brightness is shuffled between
  rows. For a *spatial* complaint, measure disagreement between adjacent
  interior cells - and do it against a **reference render of the same
  frame** (the depth field as it would be if every row repainted, in
  coherent order, this frame), or legitimate depth contours get counted as
  artifacts and swamp the signal. Two ablations are worth running before
  theorising: repaint every row every frame, and force one axis's blend
  weight to zero. If the artifact survives both, it is not the mechanism
  you think it is.
- **A probe that reproduces nothing may just be too tidy.** A pool that is
  an exactly-filled rectangle at equilibrium settles to *zero* dirty rows,
  so every repaint comes from the wake tick, which always walks a whole
  column in order - which hides every sparsity bug there is. Give the
  scene a free surface and a trickle, then check the actual number of rows
  repainted on a non-wake frame before trusting a null result.
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
  "Closed: liquid depth is now gravity-continuous" above.
