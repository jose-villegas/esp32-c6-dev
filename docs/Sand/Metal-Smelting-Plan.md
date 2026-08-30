# Plan: Metal, and what lava does to dirt

**Status**: planned, not built. Written 2026-08-26.

Lava and dirt have no reaction, which is the one obviously missing pair on
the board. This closes it by making dirt smelt into a new material, and
gives that material a job nothing else on the board can do: **move heat a
long way**.

---

## Why the gap exists

`reactions[MAT_DIRT]` (`material.c`) has `soaks`, `dries` and
`dissolvable` and nothing thermal at all. It cannot simply be given the
good kind of heat reaction either: dirt's variant is fully spent, 1 bit of
tone plus 3 of moisture (`SOIL_MOISTURE_BITS`, `material.h`), so there is
nowhere to bank a `heat_ramp`. Any dirt/lava reaction has to be a
memoryless `heat_chance` roll, like sand into glass.

That constraint shapes the whole design below.

---

## The shape

`MATX_METAL` as extended material 3. No new `reaction_t` fields, no new
`KIND`, no new sweep flag, `MAT_COUNT` unchanged.

*(This plan briefly said "material 5": leaf ageing landed
`MATX_LEAF_DRY`/`MATX_LEAF_DEAD` into slots 3 and 4 between writing and
building, then that chain was removed before metal shipped, freeing 3
back up. Metal is 3, and twelve extended slots remain. Corrected
2026-08-27.)*

Consequences of staying in the extended range:

- the last full-physics slot stays banked (see `Architecture.md`, "the
  budget") for something that actually has to move or carry a variant;
- every `MAT_COUNT`-derived test is untouched, in particular the all-pairs
  mixed scene in `suite_sand.c`;
- metal gets its own colour and its own reaction row, because
  `reaction_of()` decodes the extended range and the palette is indexed by
  the whole cell byte;
- metal gets **no variant**, so it cannot glow, cannot hold a temperature,
  and cannot melt. That is the price, and the design leans into it rather
  than fighting it.

### The rod that grows itself

`sand_reactions.c` already runs `try_heat_transform()` on whatever sits at
the **far side** of a conductor run (the `br->heats_to != 0 &&
br->heat_chance != 0` gate in `conduct_heat()`'s tail). So dirt at the end
of a metal bar smelts into metal, which lengthens the bar by one, which
reaches one cell further - until the run hits `CONDUCT_REACH` and stops
dead.

**A lava source grows its own 33-cell metal rod out of a dirt bed and then
stops** - measured on host, not 32. `conduct_heat()`'s walk can still cross
a run already AT the cap (its depth counter only needs to reach
`CONDUCT_REACH - 1`, which satisfies `depth < CONDUCT_REACH`), so one more
cell gets placed before the next attempt finally fails to fit - see
`test_the_rod_terminates_at_conduct_reach_not_the_far_wall` in
`suite_sand.c` for the measurement. It is self-limiting, it falls out of
tables that are already written, and it makes `CONDUCT_REACH` legible to
the player for the first time, to within one cell. It is also the thing
most likely to surprise someone, so the bound gets a test.

---

## Edits, in order

### 1. `material.h`

Add `MATX_METAL` to `material_extended_t`, after `MATX_LEAF`.

### 2. `material.c`

- `extended_names[MATX_METAL] = "Metal"`.
- Palette entry `[MAT_EXTENDED * MATERIAL_VARIANTS + MATX_METAL]`, and drop
  one `0xFF00FF` from the magenta tail so the count stays 16. The tail is
  thirteen entries today (`MATX_LEAF` is slot 2); it becomes twelve.
- A `metal_grain[8]` table beside `ice_grain` / `plant_grain` /
  `leaf_grain`, and `MATX_METAL` added to the ternary chain in
  `material_colours()`.

  Starting ramp: a cool blue-grey, roughly `0x7C8794` to `0xB9C4D2` -
  brighter and cooler than ambient stone so the two walls separate at two
  screen pixels per cell, and greyer than ice's `0xB6E4F2` so they do not
  merge either. Needs eyes on the panel; this is a guess, not a
  measurement.

  **The chain in `material_colours()` is a measured shape.** Its own
  comment says a respelling of that branch is a performance change - an
  if/else rewrite elsewhere cost 14% through the inlining cliff. Extend the
  existing chain; do not convert it to a switch.

  That chain is **three** deep (`MATX_PLANT`, `MATX_LEAF`, `MATX_ICE`), and
  metal makes it a fourth. The existing comment's "adding a third material"
  phrasing is still very nearly right - adjust it to say a fourth rather
  than reopening the switch-versus-chain question, which a single added
  condition does not justify.

- `extended_reactions[MATX_METAL]` row - see Numbers below.

### 3. `reactions[MAT_DIRT]`

Gains:

```c
.heats_to    = MATX(MATX_METAL),
.heat_chance = 10,
```

`heats_to` is a `uint8_t` and `MATX(k)` is `0xF0 | k`, which
`place_reacted()` already routes to the extended path. Nothing to change
there - that routing exists precisely so a reaction can produce an extended
material.

### 4. `try_heat_transform()` in `sand_reactions.c` - the wet stage

Dirt can never have a `heat_ramp`, but wet earth should not smelt as though
it were dry, and the drying is the part worth watching. Spend the **same
successful** `heat_chance` roll on driving one moisture level off as steam:

```c
if (r->heats_to == 0 || r->heat_chance == 0) return false;
if (roll fails) return false;
if (r->dries != 0 && CELL_MOISTURE(n) != 0) {   /* heat works on the water first */
    s->cells[at] = CELL_WITH_MOISTURE(n, CELL_MOISTURE(n) - 1);
    /* emit MAT_STEAM into the first empty cardinal */
    return true;
}
place_reacted(s, nx, ny, at, r->heats_to);
```

Two reasons it lands exactly there:

- `dries != 0` is already the canonical "this variant is moisture" marker
  (see the `spread` block in `sand_reactions.c`). Sand soaks but does not
  dry, so `sand -> glass` is unaffected. Do not invent a new field for
  this.
- the branch sits **after** the roll, on a path that has already succeeded,
  so it costs two reads off an already-loaded `r` in the cold pass.

Saturated dirt therefore needs 8 successes to reach metal instead of 1, and
each of the first 7 is visible as steam.

The steam emit is `try_flare()`'s body with a different spec. Factor out
`emit_into_empty_neighbor(s, x, y, w, h, spec)` and have `try_flare()` call
it - **but** `Adding-a-Material.md` ("inlining into more call sites is not
free") is explicit that this can regress. Measure. If it moves, duplicate
the six lines instead and say why in a comment.

### 5. Brush list (`app_sand.c`)

**Do not add metal to the brush.** Decided - see "Decisions taken" below.
The list is already 14 button presses long and its own comment says only
materials someone actually paints belong. Metal is the first material you
have to *make*, which this sim has never had.

Note for whoever writes the device tests: this means metal cannot be
painted by hand on the panel, so any on-device check of it has to build a
smelt - lava against a dirt bed - rather than drawing a bar.

---

## Numbers

| field | value | why |
|---|---|---|
| `conducts` | 248 | see below |
| `dissolvable` | 1 | balance revision 2026-08-30: metal now resists acid instead of being its counter (was 110, deliberately above stone's 60); stone 60, sand 200 unchanged. 1 rather than 0 (immune) so metal stays in the generated reaction docs |
| dirt `heat_chance` | 10 | slower than sand into glass (16). Smelting should be a project |
| everything else | 0 | never catches, never a heat source, never melts |

All of these are starting points, to be tuned on device like every other
constant in this app.

### `conducts`

Rolled per cell crossed, so depth *d* succeeds with probability
(c/256)^*d*:

| depth | stone / glass (220) | metal (248) |
|---|---:|---:|
| 8 | 30% | 78% |
| 16 | 8.5% | 60% |
| 32 | 0.8% | 36% |

248 puts the mean walk at ~32 cells, exactly `CONDUCT_REACH` - so the cap
starts doing real work rather than being slack, and the rod length above is
a designed number rather than an accident.

### Deliberately no `heats_to` on metal

With no variant it cannot ramp, and a memoryless roll would mean a metal
wall beside lava randomly turning into lava. So metal is heatproof, and the
three walls read:

| | heat | acid | conducts |
|---|---|---|---|
| stone | survives | slow (60) | 220 |
| glass | melts | immune | 220 |
| metal | survives | resists (1) | 248 |

One axis of difference each, which is the standard this codebase already
holds stone and glass to - though metal's acid column is a balance
revision (2026-08-30, was "fast (110)") rather than the original design,
see the `dissolvable` numbers table above.

---

## The performance problem nobody would catch

Extended materials appear in **no benchmark scene**. The mixed scene
enumerates `MAT_EMPTY + 1 .. MAT_COUNT - 1` only. So metal's entire cost -
a conductor walk with a mean roughly 5x stone's - would ship unmeasured,
into a campaign where all thirteen budgets are currently red and round five
is open.

Minimum before merge: a host test that heat crosses a 20-cell metal run and
does **not** cross a 20-cell stone one, plus a counter on walk depth.

Whether a full device benchmark scene is worth adding mid-round is a
scheduling call, not a technical one - but this plan does not get to pretend
the cost is zero.

---

## Tests

- dry dirt beside lava becomes metal
- saturated dirt takes roughly 8x as long, and emits steam on the way
- watered dirt beside lava steams **before** it smelts - assert the
  sequence, not just the endpoint
- `sand -> glass` still works; the `dries` guard must not catch sand
- heat crosses a metal run far further than a stone one (the pair above)
- **the rod terminates**: lava at one end of a long dirt bed grows a metal
  run that stops at `CONDUCT_REACH`, not at the far wall
- acid eats metal slower than stone and much slower than sand (balance
  revision 2026-08-30 - metal now resists acid rather than being its
  counter, see the `dissolvable` row above)
- add `MATX_METAL` to the grained set in
  `test_the_right_extended_materials_are_speckled` - it asserts the
  negative half too, so it fails loudly if the grain table and the palette
  entry disagree
- `test_every_material_has_a_palette_block` covers the new entry for free

---

## Docs to correct in the same change

Both of these are stale *now*, and both are things this change builds
directly on:

- `Architecture.md` describes the extended range as "`MATX_ICE` and
  `MATX_PLANT`". `MATX_LEAF` has existed for some time and is missing.
- the reaction-chain mermaid in `Adding-a-Material.md` still has EMBER as
  its own node. Ember folded into wood's variant, which that same document
  explains at length two sections earlier.

Plus the ordinary additions: metal and dirt into that diagram, and
`conducts` 248 with whatever the walk-depth measurement says into
`Tuning-At-a-Glance.md`.

---

## Decisions taken

**Yield: all-metal.** A bed of dirt under lava becomes a bed of metal.
Abundance is tuned with `heat_chance` alone. A partial slag yield - some
cells coming out stone - was considered and rejected for now: it would read
as ore veins, but it needs a new `reaction_t` field serving exactly one
material, and `heat_chance` is the knob that already exists. Revisit only
if metal turns out too cheap on device.

**Renewable, deliberately.** `sand + water -> dirt` already exists at rate
8, so the chain sand, soak, smelt regenerates metal without limit. This is
intended: metal is a production chain, not a resource you exhaust. Do not
"fix" it later by mistake.

**Brush: metal is not paintable.** The only way to get metal is to smelt
it, which makes it the first material in this sim you have to make rather
than paint. The cost is real and accepted: testing metal on device means
setting up a smelt each time, and you cannot simply draw a heat pipe across
the board. See section 5.
