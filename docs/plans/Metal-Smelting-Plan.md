# Metal, and what lava does to dirt

**Status**: shipped. This is no longer a plan - it is kept for the two
numbers tables the code cites (`material.c`, `material.h`,
`material_palette.c` and `suite_sand_metal.c` all point here) and for the
design record of why metal behaves as it does. The implementation is the
live authority; where they disagree, the code wins.

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
  mixed scene in `suite_sand_scenes.c`;
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
`suite_sand_metal.c` for the measurement. It is self-limiting, it falls out of
tables that are already written, and it makes `CONDUCT_REACH` legible to
the player for the first time, to within one cell. It is also the thing
most likely to surprise someone, so the bound gets a test.

---

## Numbers

| field | value | why |
|---|---|---|
| `conducts` | 248 | see below |
| `dissolvable` | 1 | balance revision 2026-08-30: metal now resists acid instead of being its counter (was 110, deliberately above stone's 60); stone 60, sand 200 unchanged. 1 rather than 0 (immune) so metal stays in the generated reaction docs |
| dirt `heat_chance` | 10 | slower than sand into glass (16). Smelting should be a project |
| dirt `flaw_to` / `flaw_chance` | MAT_STONE / 220 | added 2026-08-31 at 40, rebalanced twice same day (40 -> 90 -> 220) - metal is meant to be genuinely rare now, see "Decisions taken" - starting point, tune on device |
| dirt `spoils_to` / `spoils_chance` | MAT_SAND / 235 | added 2026-08-31 at 24, rebalanced twice same day (24 -> 128 -> 235) - wet dirt reaching metal or stone at all should be a rare surprise, see "Decisions taken" - starting point, tune on device |
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
