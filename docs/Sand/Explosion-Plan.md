# Plan: Explosions, without a velocity field

**Status**: BUILT, across five rounds, on top of what is written below.
Written 2026-08-27, when none of it existed yet; kept as the original
design reasoning rather than rewritten to match the finished mechanism,
because the WHY below is still accurate even where the WHAT has moved on.
Where a specific detail was superseded, a note says so at that spot rather
than silently editing the history out. The two changes worth knowing
before reading the rest:

- **The list is `impulse_t`, not `blast_t`, and `sand_explode()` is one
  caller of a primitive, `sand_impulse()`, rather than the whole
  mechanism.** The plan below was written before a second caller was even
  hypothetical, so it describes the list, the buffer and the flight pass
  as if they belonged to explosions specifically. They do not: an
  explosion is a point that seeds many entries radially, and nothing
  about the entry, the buffer, or the pass that moves it knows that. See
  `sand_impulse()`'s own comment in sand.h for the primitive's actual
  signature, and impulse_t's own comment for why the rename happened
  once there was a second name-shaped trap to avoid, the same one
  material.h documents for `mobility` and `sight`.
- **The "chance in 256, no count to store" reasoning below did not
  survive the arc.** A flat, memoryless chance every turn cannot produce
  a curve - see SAND_IMPULSE_SPEED_RAMP's own comment in sand.h for why
  gravity's constant one-cell-per-step fall makes that structural, not a
  tuning problem. Each entry now carries a `speed` byte that IS that
  turn's chance AND ramps down every turn, which is honestly a count
  after all, just one expressed as a shrinking probability instead of an
  integer - the idiom survived even though the "no count" claim did not.

Nothing in the simulation displaces or destroys in a blast today. This adds
one, deliberately **without a trigger and without gunpowder**, so the
mechanic can be evaluated on its own before any material or slot is spent
on it.

---

## Why this is not the per-cell velocity that was already rejected

The repo has ruled against per-grain velocity twice, and both rulings
stand:

- [`Sand-Simulation.md`](Sand-Simulation.md) - the momentum vector is a
  single shared `(mom_x_q8, mom_y_q8)`, and says why: *"not a per-cell
  velocity field, which would cost another byte per cell."*
- [`Simulation-Lessons.md`](Simulation-Lessons.md) - *"Still not modelled:
  per-grain drag. Grains have no individual velocity, which would need
  per-grain memory."*

The grid is 368x448 at `CELL_MIN` 2, so **41,216 cells**. A byte each is
~40 KB, allocated whether or not anything is moving; a real vector pair is
~80 KB. And the cell byte is 4+4 and fully spent, so it could only ever be
a parallel array.

**This design does not add a field.** An explosion throws a few hundred
grains for a few dozen steps, so the in-flight set is a **bounded transient
list** - exactly the shape `crack_run()` already uses (`CRACK_MAX` 256
entries as `uint16_t` indices, 512 bytes of stack, nothing at all when it
is not running).

At 256 entries of `{uint16_t index, uint8_t dir}` that is **under a
kilobyte**, three orders of magnitude off the rejected design, and it
touches neither the cell byte nor any material row.

*(As shipped, the entry grew twice more - a `cell` byte for the identity
check that turned out to be necessary, and a `speed` byte for the arc -
to `{uint16_t index, cell_t cell, uint8_t dir, uint8_t speed}`, six bytes
with alignment. 256 entries is 1.5 KB rather than under one, still nowhere
near the rejected per-cell field. See impulse_t's own comment in sand.h
for why each addition earned its byte.)*

The one-cell-per-step invariant the whole sweep rests on is also
preserved: a flying grain moves **one cell per step**, in its own pass. It
travels further by flying for more steps, not by moving faster.

---

## The shape

### `sand_explode()`

```c
void sand_explode(sand_t *s, int cx, int cy, int radius);
```

Deliberately the same signature shape as `sand_spawn()` and
`sand_erase()`, which already take a centre and a radius - it belongs to
that family and should read like it. **No material owns it, no reaction
fires it.** Tests call it directly; the app calls it from a temporary
mode. That is the whole trigger story for now.

### The in-flight list is caller-provided

Mirror `sand_enable_sleeping()` and `sand_track_dirty_rows()`, which take
a caller-allocated buffer rather than owning one:

```c
void sand_enable_impulses(sand_t *s, impulse_t *buf, int max);
```

*(As shipped: `sand_enable_blast`/`blast_t` in this original plan, renamed
once a second caller made "blast" the wrong name for a list that was never
explosion-specific in the first place - see this document's own status
note at the top, and impulse_t's comment in sand.h.)*

Opt-in, so anything that never uses an impulse pays nothing - not even the
struct space - and a test can size it small to exercise the cap
deliberately. Without it, `sand_explode()` is a no-op.

### Exhaustion is a ROLL, not a fixed counter - but it is not memoryless either

`SAND_BLAST_DECAY`, a chance in 256 per step that a grain keeps flying.

Chosen over a per-entry step counter for two reasons. It is the house
idiom - `mobility`, `falls`, `scatter`, `flare`, `heat_chance` are all
chance-in-256 per step, and this will read like the rest of the file. And
geometric decay gives the right *distribution* for free: most grains stop
early, a few carry far, which is what a blast looks like, without tuning a
spread by hand. One knob, meaning "how far things fly", instead of a rate
and a cap that have to agree.

It also keeps the entry down to a position and a direction. There is no
count to store.

**AS SHIPPED, this section's own reasoning did not survive first contact
with the arc.** A *fixed* chance-in-256, rolled identically every turn
regardless of how long a grain had already been flying, cannot produce a
curve: gravity here never accelerates, so the vertical fall is a constant
one cell per step forever, and a parabola needs the horizontal half to
change against that constant instead. A flat roll gives a 45-degree
diagonal for as long as it keeps succeeding, then a vertical drop the
moment it fails - a bent line, not a curve, however the one number was
tuned.

The fix keeps the chance-in-256 idiom but gives each entry its own
`speed` (see impulse_t in sand.h), which both IS this turn's chance and
ramps down by a fixed amount - `SAND_IMPULSE_SPEED_RAMP` - every turn,
moved or blocked or waiting. That is, honestly, a count after all: not an
integer of steps remaining, but a shrinking probability that reaches
exactly zero after a fixed, deterministic number of turns
(`ceil(speed / SAND_IMPULSE_SPEED_RAMP)`), at which point `rng_chance()`
with a zero numerator can never succeed again. "There is no count to
store" was the wrong takeaway from the house idiom; "the count can be
*expressed* as a chance in 256, and read like the rest of the file" was
the part worth keeping. See SAND_IMPULSE_SPEED_RAMP's own comment in
sand.h for the full reasoning, and SAND_EXPLODE_INITIAL_SPEED's for why
"how far things fly" ended up belonging to `sand_explode()` specifically
rather than to the generic mechanism.

---

## Where the pass runs, and why it must be LAST

`sand_step()` ends with, in order: the main sweep, `sand_step_liquids()`,
`sand_step_gas()`, `sand_step_reactions()`, then `finalize_settling()`.

**The flight pass goes immediately before `finalize_settling()`**, after
everything else that can move or replace a cell.

That ordering is the whole design, for two separate reasons.

**It solves identity.** The sweep also moves cells. If flight and gravity
both believe they own a grain, the list's stored index goes stale the
moment anything else lands in that cell, and the pass starts moving
someone else's grain. Running flight last means it records the exact
destination it wrote, so the position is always known at the top of the
next step.

Belt and braces on top of that: **verify before moving.** At the start of
each entry's turn, check the cell at the stored index is still the
material that was thrown. If it is not, drop the entry silently. Grains
get lost occasionally and nobody will ever see it. The alternative is a
per-cell "in flight" bit, which is the 40 KB again.

**It gets the arc for free** - *this ordering is necessary, but, as
shipped, it was not sufficient: see the "Exhaustion is a ROLL" section's
own update note above for why a flat, memoryless decay still produced a
bent line rather than a curve, and what changed to fix it.* Gravity pulls
down in the sweep; the impulse pushes outward in the flight pass; the
impulse decays. Down, plus out, decaying, **is** a ballistic path - so
grains arc without anything
modelling an arc. This is the one thing a pure one-shot displacement blast
could not do, and it costs nothing extra here.

---

## The blast itself

For each cell within `radius` of the centre, assign an outward direction
(away from the centre, quantised to the eight the rest of the simulation
already uses - see `sand_gravity_direction()`) and push an entry.

**Containment falls out of the flight rule and needs no raycast.** A
flying grain moves into the next cell along its direction only if that
cell is empty; if it is blocked, the grain stops and the entry is
dropped. So a blast inside a stone vessel throws its grains one cell into
the wall and they stop. No line-of-sight pass, no radial occlusion test.
Worth stating explicitly because the obvious first instinct - raycast
from the centre - is both more expensive and unnecessary.

**Over the cap, grains simply do not fly.** They stay where they are.
Graceful degradation, exactly as `CRACK_MAX` truncates a crack rather than
failing. A blast bigger than the list is a smaller-looking blast, not a
bug.

### v1 keeps it simple, deliberately

A flying grain **stops on any obstruction**. It does not push, displace,
or swap with a lighter cell. That is a real limitation - a blast under a
pile will not lift the pile - and it is the right v1: it makes the pass a
single bounded walk with no cascade, and the whole point of this plan is
to find out whether the mechanic reads at all before making it cleverer.

**AS SHIPPED, "stops on any obstruction" turned out to mean "does
nothing at all" the first time it met a packed bed or a body of water** -
a real device reported it in exactly those words. Every queued cell in an
incompressible medium starts out surrounded by more of the same material,
so every entry's very first move was blocked, and "stop" dropped it on the
spot before it ever went anywhere. Two changes fixed it without touching
this section's central claim (a flying grain still never pushes,
displaces, or swaps with a lighter cell): a blocked entry now **waits**
for its target to clear instead of being dropped outright, retrying every
turn on the same bounded clock its speed already ages on; and
`sand_explode()` **fills a small core with fire** before it queues
anything, giving the disc a real cavity to collapse into on the very
first step rather than depending on open space already existing nearby.
See `SAND_EXPLODE_CORE_DIVISOR`'s own comment in sand.h for the full
reasoning, including why fire rather than a plain hole.

---

## Host tests

**Write this one first, because it forces the design decision:** detonate
inside a closed stone vessel and assert nothing outside the wall moved. It
is what makes "stop when blocked" versus "radial displacement" a decision
taken rather than a default fallen into.

Then:

- **Conservation.** A blast MOVES cells; total count before must equal
  count after. `test_dithering_still_conserves_grains` is the existing
  idiom. This catches cells silently dropped on the floor, which no visual
  check would ever notice.
- **Bounds.** Detonate at each edge and at a corner. Nothing out of
  bounds, nothing unbounded - `CRACK_MAX` exists for this reason.
- **A dropped entry never moves someone else's cell.** Throw a grain,
  overwrite its cell from outside, step, and assert the entry was dropped
  rather than relocating whatever now sits there. This is the identity
  rule above, and it is the one that will fail subtly if the pass is
  moved earlier in `sand_step()`.
- **The cap degrades gracefully.** Size the buffer small, detonate large,
  and assert the excess grains stayed put and nothing was lost.
- **Blocks wake.** A settled, sleeping region hit by a blast must wake, or
  displaced grains freeze mid-air. `Adding-a-Material.md` has a whole
  lesson on this failure being invisible to almost every other test.
- **Empty space is a no-op.** No crash, no cells created from nothing.
  *(As shipped: no longer quite true, once the core fills with fire
  unconditionally - see the v1 update note above. Detonating over nothing
  still flashes the core; what stays a no-op is everything beyond it,
  since there is nothing there to queue.)*
- **No buffer, no blast.** Without `sand_enable_impulses()`,
  `sand_explode()` does nothing and nothing crashes.

---

## Device: how to trigger it, and what to look at

### Wiring

`erasing` is already a mode that PWR toggles, and the paint call site
already branches on it into `sand_erase()`. Make PWR cycle three modes -
**PAINT -> ERASE -> DETONATE** - reusing the existing mode label banner.
Touch gives the position for free.

*(As shipped: exactly this three-way cycle, but `erasing`/the mode itself
now lives in `sand_ui_t` (sand_ui.h/.c) as `sand_mode_t`, not as a
file-scope `app_sand.c` variable - a later UI rewrite made the app's
button/touch state machine host-testable, and DETONATE was rebuilt
against that rather than the original `app_sand.c` scaffolding. See
sand_mode_t's own comment in sand_ui.h.)*

**Fire on press, not on the `applications` loop.** That loop runs every
frame while held, so a held finger would detonate continuously - useful as
a stress test, useless for looking at one blast.

This is temporary scaffolding for evaluation, not a shipped feature.

### What to look at, in order

1. **Crater in a flat sand bed.** The one that decides everything: crater
   with a thrown lip, or just a hole? If it is just a hole, displacement
   is not enough and nothing else matters yet.
2. **Airburst above the bed.** Do grains **arc**, or travel straight and
   then drop? Straight lines mean the pass is running in the wrong place
   relative to the sweep.
3. **Detonate in water.** The cavity should collapse and slosh. Fast,
   obvious, and it exercises flight against a material with its own pass.
4. **Detonate inside a stone vessel.** Containment, and you see *how* it
   fails - grains tunnelling versus grains stopping dead.
5. **Full screen of packed sand, then rapid repeat presses.** Worst-case
   frame cost, and whether the cap degrades gracefully under pressure.
6. **Detonate near a tree.** Interaction with static extended materials,
   and the fastest read on whether the mechanic is actually fun.

### Two failure modes to watch for by name

- **Grains frozen mid-air** - blocks not woken. The classic bug here, and
  invisible to most tests.
- **Grains vanishing** - the list dropping entries, or conservation
  broken. The host test catches the arithmetic; on the panel it looks like
  the blast ate material.

### The quantitative half

`launcher/tools/report_performance.sh` for frame-budget numbers, before
and after on the same build. This matters more than usual: a blast is a
**burst** cost, and no existing benchmark scene measures one. The perf
round is open.

---

## What this deliberately does not decide

**The trigger.** No material explodes. Lava meeting water, a heated sealed
vessel bursting, a gas pocket going up, or gunpowder - all of those are
one call site once the mechanic is known to work, and all of them are
easier to judge after seeing it than before.

**Gunpowder's slot.** Gunpowder wants `KIND_POWDER` (to pile) and a
variant (to burn down as a fuse), which is exactly the criterion
`Architecture.md` names for the last full-physics slot. That slot should
be spent *after* this mechanic has been seen working, not before - if
blasts read badly or cost too much, the slot is still banked.
