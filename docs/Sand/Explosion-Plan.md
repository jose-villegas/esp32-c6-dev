# Plan: Explosions, without a velocity field

**Status**: planned, not built. Written 2026-08-27.

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
void sand_enable_blast(sand_t *s, blast_t *buf, int max);
```

Opt-in, so anything that never explodes pays nothing - not even the
struct space - and a test can size it small to exercise the cap
deliberately. Without it, `sand_explode()` is a no-op.

### Exhaustion is a ROLL, not a counter

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

**It gets the arc for free.** Gravity pulls down in the sweep; the impulse
pushes outward in the flight pass; the impulse decays. Down, plus out,
decaying, **is** a ballistic path - so grains arc without anything
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
- **No buffer, no blast.** Without `sand_enable_blast()`, `sand_explode()`
  does nothing and nothing crashes.

---

## Device: how to trigger it, and what to look at

### Wiring

`erasing` is already a mode that PWR toggles, and the paint call site
already branches on it into `sand_erase()`. Make PWR cycle three modes -
**PAINT -> ERASE -> DETONATE** - reusing the existing mode label banner.
Touch gives the position for free.

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
