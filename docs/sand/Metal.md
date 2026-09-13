# Metal

Lava smelts dirt into metal, and metal moves heat further than anything
else on the board.

## What it is

`MATX_METAL`, extended material 3. No new `reaction_t` field, no new
`KIND`, no new sweep flag — it is a reaction row and a palette entry.

Staying in the extended range costs it a **variant nibble**, so metal
cannot glow, cannot hold a temperature, and cannot melt. The design leans
into that rather than fighting it.

## Why it exists

Lava and dirt had no reaction — the one obviously missing pair on the
board. Closing it needed a product, and the product needed a job nothing
else could do. Every other solid stops heat; metal carries it.

| | heat | acid | `conducts` |
|---|---|---|---:|
| stone | survives | slow (60) | 220 |
| glass | **melts** | **immune** | 220 |
| metal | survives | resists (1) | **248** |

One axis of difference each. Metal has **no `heats_to`** deliberately:
with no variant it cannot ramp, so the only way to melt it would be a
memoryless roll — and that would make a metal wall beside lava randomly
turn into lava.

## How dirt becomes metal

Dirt under heat rolls once, then a second roll decides the product:

```
        dirt + heat
             |
      heat_chance (10)
             |
        transforms
         /        \
   flaw 230/256   26/256
        |            |
      STONE        METAL      ~10% of transforms
```

The flaw roll is re-rolled only every fifth cell
(`HEAT_FLAW_CLUMP`, `sand_reactions.c`), so metal arrives in **veins**
rather than salt-and-pepper.

Wet dirt takes a different exit first: `spoils_to = MAT_SAND` at 77/256
(~30%) — soaked ground gives up sand before it ever smelts.

## The rod that grows itself

`conduct_heat()` applies `try_heat_transform()` to whatever sits at the
**far side** of a conductor run. So dirt at the end of a metal bar smelts
into metal, which lengthens the bar, which reaches one cell further:

```
  lava ▓ metal ▓▓▓▓▓▓▓ dirt ░░░░░
              →  each smelt extends the run by one
  lava ▓ metal ▓▓▓▓▓▓▓▓ dirt ░░░░
              →  until the run hits CONDUCT_REACH and stops dead
```

A lava source grows its own **33-cell** rod out of a dirt bed and then
stops — 33, not 32, because the walk can still cross a run already at the
cap, placing one more cell before the next attempt fails to fit
(`test_the_rod_terminates_at_conduct_reach_not_the_far_wall`,
`suite_sand_metal.c`).

It is self-limiting, it falls out of tables already written, and it makes
`CONDUCT_REACH` visible to the player for the first time.

## Why `conducts` is 248

Rolled per cell crossed, so depth *d* succeeds with probability
(c/256)^*d*:

| depth | stone / glass (220) | metal (248) |
|---|---:|---:|
| 8 | 30% | 78% |
| 16 | 8.5% | 60% |
| 32 | 0.8% | **36%** |

248 puts the mean walk at ~32 cells, exactly `CONDUCT_REACH` — so the cap
does real work instead of being slack, and the rod length above is a
designed number rather than an accident.

`dissolvable` is 1 rather than 0 so metal still appears in the generated
reaction docs; 0 would read as "immune" and drop the row.

---

Related: [`Reaction-Table.md`](Reaction-Table.md) for the generated
per-material rules.
