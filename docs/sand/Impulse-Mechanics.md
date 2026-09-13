# Impulse Mechanics: things in flight

A single-page map of everything that throws, dislodges, or displaces a
cell - explosions, thrown chunks, and a liquid's own splash - written in
the same spirit as [`Architecture.md`](Architecture.md): the *shape* of
the system, not the reasoning behind each constant. Those live where they
belong, in the code: `sand_impulse.h`/`sand_impulse.c` for the mechanism,
`sand.h` for the liquid-splash constants, `sand_liquid.c`/`sand_reactions.c`
for the two liquid callers. This page exists because none of those files
individually shows that an explosion, a thrown wall chunk, and a water
splash are **one mechanism with three call sites**, not three unrelated
features.

This replaces `Explosion-Plan.md` and `Liquid-Splash-Displacement.md`,
both process narratives written while the mechanism was still being built
and both explicitly self-described as "not a reference for the finished
mechanism, read the code comments instead." Their play-by-play is in git
history if it's ever needed again; what was still worth keeping from them
is condensed into this page's "Lessons worth keeping" section below.

---

## It is a picture of a physics, not a physics

Same rule [`Sand-Simulation.md`](Sand-Simulation.md#momentum-and-the-wall-rebound-splash) sets for
tilt momentum: there is no per-cell velocity, because a byte-per-cell grid
at 41,216 cells can't afford a second field just for things that are
usually standing still. A "flying" grain is a caller-provided list of
`{index, cell, dir, speed, ramp}` entries (`impulse_t`, `sand_impulse.h`),
capped and opt-in - the same bounded-transient-list shape `crack_run()`
already uses - not a property of the grid itself. Nothing about a cell
byte changes because something nearby is mid-throw.

The **arc** a blast produces is bought the same cheap way: gravity's fall
is a flat one-cell-per-step drop, never accelerating, so decaying only the
*horizontal* half against that constant is enough to bend a straight throw
into a curve, no trigonometry involved:

```
 speed 255 ┐\                     ← launch, steep horizontal push
           │ \
 speed 160 │  \___                ← push weakening, fall still constant
           │      \___
 speed  40 │          \______     ← push nearly gone, path steepens
           │                 \
 speed   0 └──────────────────●   ← pure vertical fall from here on
             one cell of fall, every row, the whole time
```

The direction ring both the throw and the splash use is the same eight
compass points the rest of the simulation already shares
(`ring_dir()`/`ring_of()`, `sand_priv.h`):

```
        7  0  1
         \ | /
      6 ── + ── 2        0 = up, 2 = right, 4 = down, 6 = left,
         / | \           odd indices are the diagonals
        5  4  3
```

---

## The primitive stack

```
sand_impulse(x, y, dir, speed)          ← ONE grain, ONE step's worth of push
   │  refuses KIND_STATIC outright - a wall has no leverage to move BY this
   │
   ├── sand_impulse_dislodge(x, y, dir, speed, ramp)
   │      bypasses the toughness roll below for a guaranteed single-cell
   │      push - today exercised only by suite_sand_impulse.c; no game
   │      trigger calls it yet, the same way sand_explode() started
   │
   ├── sand_displace(cx, cy, radius) / sand_displace_material(..., mat_id)
   │      ring-seeded radial disc built on sand_impulse() per occupied
   │      annulus cell; KIND_STATIC CAN be dislodged here, at a
   │      density-scaled chance (see "Walls, chunks and toughness" below)
   │      │
   │      ├── sand_explode(cx, cy, radius)
   │      │      thin wrapper: fills a small core with fire, then calls
   │      │      sand_displace() for the rest - see "Why a core of fire"
   │      │
   │      └── splash_displace() (sand_liquid.c)
   │             water's landing/rebound splash - calls
   │             sand_displace_material() so it can never fling anything
   │             but the water itself
   │
   └── acid_bubble() (sand_reactions.c)
          NOT built on sand_displace() at all - a single straight-up
          sand_impulse() call with a one-step spread, gated by
          evaporation. Superseded acid's own displacement-based splash
          entirely (see "Water and acid are on different mechanisms now")
```

### Who calls what, today

| Entry point | Trigger | Where |
|---|---|---|
| `sand_explode()` | DETONATE touch mode; a gunpowder 2x2 burning out (`reaction_t.explodes`); a gas pocket igniting; a lava burst | `app_sand.c`, `sand_reactions.c` |
| `splash_displace()` | Water lands hard on an occupied cell, or rebounds off a wall | `sand_liquid.c` |
| `acid_bubble()` | Acid dissolves a neighbour and is exposed to open space above it | `sand_reactions.c` |
| `sand_impulse_dislodge()` | Host tests only - the primitive is built and proven, waiting for a game-facing caller (a thrown-chunk feature, say) | `suite_sand_impulse.c` |

---

## Why the flight pass runs LAST, and verifies before it moves anything

`sand_step()`'s order: main sweep -> `sand_step_liquids()` ->
`sand_step_gas()` -> `sand_step_reactions()` -> `step_impulses()` ->
`finalize_settling()`. The flight pass is deliberately the last thing that
can move a cell.

**Identity, not coordinates, is what a flying grain owns.** If gravity's
own sweep ran after flight, a landed grain and a mid-flight one could both
believe they own the same cell. Running flight last means every entry's
stored index is still exactly what it wrote last step - and every entry is
still re-checked against `cell` (the exact byte it was thrown as) before
it moves, so a cell that changed out from under it - overwritten, reacted,
consumed - silently drops the entry instead of flying whatever is there
now. No per-cell "in flight" bit needed; that would cost the same 40 KB
this whole mechanism exists to avoid.

**Over the cap, a blast simply throws fewer grains, evenly.** `sand_explode()`
seeds every occupied annulus cell in ring order, and if that exceeds the
caller-sized buffer it thins its own density via a DDA accumulator so an
undersized buffer degrades to a smaller, even disc rather than a lopsided
crescent or a hard truncation. See `sand_explode()`'s own "QUEUED BY RING"
comment in `sand.h` for the exact scheme.

---

## Walls, chunks and toughness

Three different answers to "can this be moved," by design, not by
accident:

| Path | KIND_STATIC (wall) behaviour |
|---|---|
| `sand_impulse()` | Hard refusal, unconditional. A wall has no leverage to move a flying grain BY, and none to be moved WITH either. |
| `sand_displace()`/`sand_explode()` | Density-scaled chance (`255 - dislodge_density()` in 256, `sand_impulse.c`) - "tougher, harder to dislodge," not equally fragile. See the fragility table below. |
| `sand_impulse_dislodge()` | Bypasses the roll entirely - a guaranteed dislodge for a caller that has already decided the wall gives way. |

**Every solid has its own toughness**, ordinary materials via
`materials[]`'s own `density` field, extended statics (which otherwise
share one `materials[]` row - see `MATERIAL_ROW`'s own comment) via
`reaction_t.dislodge_density`, which overrides it:

All eight sit on one curve, `density = 221 - 20 * rank` (rank 1 = toughest
through 8 = softest) - a single knob instead of eight independently-tuned
numbers, the same idiom `SAND_IMPULSE_SPEED_RAMP` uses for the speed decay
elsewhere in this file:

| Rank | Material | Density | Chance | |
|---|---|---|---|---|
| 1 | Metal | 201 | 54/256 ≈ 21% | toughest - even stone gives way to it |
| 2 | Stone | 181 | 74/256 ≈ 29% | |
| 3 | Root | 161 | 94/256 ≈ 37% | embedded, tougher than even wood |
| 4 | Wood | 141 | 114/256 ≈ 45% | |
| 5 | Glass | 121 | 134/256 ≈ 52% | brittle - more easily dislodged than wood |
| 6 | Ice | 101 | 154/256 ≈ 60% | shatters |
| 7 | Plant | 81 | 174/256 ≈ 68% | |
| 8 | Leaf | 61 | 194/256 ≈ 76% | softest thing on the board |

The curve is a fit to hand-chosen target percentages (game balance, not a
measurement), not derived from anything physical - see each material's own
`density`/`dislodge_density` comment in `material.c` for the exact rank.

A dislodged glass pane converts to cullet as it's queued
(`queue_flying_grain()`), not a flying pane - see `MAT_CULLET`'s own notes
in `material.h`. Once queued, every entry - wall chunk or ordinary grain -
flies through the identical pass: same drag, same bounce, same transfer
rules (`sand_impulse.h`'s own constants: `SAND_IMPULSE_DRAG_POWDER_SHIFT`,
`SAND_IMPULSE_BOUNCE_MIN_SPEED`, `SAND_IMPULSE_TRANSFER_KEEP`) - toughness
only ever decides whether something gets thrown, never how it behaves
once it is.

**Why a core of fire.** A packed medium blocks every queued entry's very
first move, so `sand_explode()` fills a small central disc with fire
before it queues anything - fire being lighter than nearly everything
else, the ordinary density-swap rule lets the medium collapse into that
cavity on the first step instead of every entry finding itself boxed in
immediately. `sand_displace()` on its own has no core and places no fire
at all - the pure-pressure primitive for a caller (a cracking steam
vessel, say) that must never ignite anything just because it pushed
material around.

---

## Water and acid are on different mechanisms now

Both liquids drive `sand_impulse()`, but no longer through the same code
path - a real divergence, not a stale detail:

- **Water** (`splash_displace()`, `sand_liquid.c`): a radial spray via
  `sand_displace_material()`, masked to `MAT_WATER` so it can never fling
  whatever else happens to be nearby (dirt under a puddle, say). Two
  independent values decay on every trigger - `splash_chance` (whether
  the *next* echo fires at all) and `splash_radius_water` (how far it
  reaches if it does) - so a bounce chain settles instead of rattling on
  at shrinking-but-still-visible strength forever. See
  `SAND_SPLASH_RADIUS_WATER`'s own comment in `sand.h` for the exact
  floors and steps.
- **Acid** (`acid_bubble()`, `sand_reactions.c`): no `sand_displace()`
  call at all any more. A single straight-up `sand_impulse()` per
  dissolve, with a small (-1/0/+1) directional spread, gated by
  `SAND_ACID_BUBBLE_CHANCE` and only when the cell above is genuinely
  open. Reads as acid *bubbling*, not splashing - the earlier
  displacement-based version (radius 5, always-fires, capped per step)
  is gone; nothing in the tree still calls it that way.

