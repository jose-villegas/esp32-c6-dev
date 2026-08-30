# Liquid splash displacement: how it was built

**Status**: BUILT, 2026-08-30, water and acid only. This is a process
narrative - the order things were tried, what broke, and why the design
ended up where it did - not a reference for the finished mechanism. For
that, read `splash_displace()`'s own comment in `sand_liquid.c` and
`SAND_SPLASH_RADIUS_WATER`'s own comment in `sand.h`, both of which are
kept current; this document is not.

---

## Started as a test hook, not a feature

The starting ask was narrow: exercise `sand_displace()` on a real trigger,
not a manual button. Two hooks went into `sand_liquid.c` - water landing
hard on an already-occupied surface (`move_liquid_grain()`), and water
rebounding off a wall (`rebound_one_cell()`) - both firing unconditionally,
both water-only, both explicitly "just for testing." Genuine-impact-only
was a deliberate choice over "any landing at all": routine internal mass
levelling should not look like a splash.

The radius started small, then got deliberately exaggerated on request (5,
then tuned down to 3, then back up) so the effect was obvious enough to
actually evaluate on device, rather than tuned to looking "realistic" from
the start.

## First decay attempt: the wrong thing decayed, and in the wrong place

The first ask for decay was "start strong, taper off" - implemented as a
`speed` value fed into `sand_impulse()` directly, decremented per trigger.
Two problems, found in order:

1. **The counter was a file-scope `static`**, not per-simulation. On the
   host test suite this meant one test's splashes decayed a value the
   *next* test inherited already exhausted - state leaking across
   unrelated `sand_t` instances that should have been independent. Fixed
   by moving it onto `sand_t` itself, reset in `sand_init()`.
2. **A decaying speed still let a bounce cascade rattle on.** A displaced
   grain landing again re-triggers the same hook that threw it - every
   *architecture* variant of "make it weaker over time" still fired
   *something* on every landing, just progressively feebler, so a splash
   chain visibly bounced for far longer than a real one would before
   fading below notice.

## The fix that actually stopped the cascade: gate the trigger, not the force

The reframe: instead of decaying how *hard* each splash hits, decay
*whether it hits at all*. `sand_t::splash_chance` starts at 255
(guaranteed first splash) and drops sharply toward a low floor on every
successful trigger - a chance-in-256 roll, the same idiom `tick_decay()`
already uses elsewhere. Most echoes in a bounce chain now do nothing at
all instead of something weak, which is both the visually correct
behaviour (a real splash doesn't re-splash itself indefinitely) and
cheaper on average (a failed roll skips the whole displacement call).

This also settled the earlier back-and-forth on radius: with the
trigger itself gated, the displacement could go back to the original,
fully exaggerated `sand_displace()` radial spray instead of the narrower
directed-impulse fan that had been tried as an intermediate step to make
"decaying force" mean something. Reverted once force stopped being the
thing that decayed.

## Making it official surfaced real interference, not just test noise

Extending the same mechanism from "water, for testing" to "every liquid"
broke two existing, deliberately-tuned host tests:

- `test_acid_eats_metal_between_stone_and_sand` - acid's own splash was
  flinging some of its mass away from what it was actively dissolving
  during the initial pour (a bulk drop settling onto a floor triggers the
  landing-splash hook on nearly every column), measurably slowing its
  dissolve rate.
- `test_the_thermal_shock_scene_shatters_in_both_directions` - lava is a
  heat source, and the exaggerated radius flung live lava cells into a
  region a carefully-timed test did not expect, well before its window
  closed.

Both were genuine mechanical side effects of the radius, not noise from
adding an extra `rng_next()` call - confirmed by checking the actual
before/after step counts rather than assuming. Scope was narrowed to
water and acid only; oil and lava were left out.

## The masking bug, found by watching, not by a test

Separately: pouring water over water sitting on dirt was flinging the
dirt too. `sand_displace()` throws whatever it finds within its radius,
regardless of material - fine for an explosion, wrong for a splash that's
supposed to be water splashing itself. Fixed with a new primitive,
`sand_displace_material()` (`sand.c`/`sand.h`), threading an optional
material filter down through `queue_outward_impulse()` into
`queue_flying_grain()` - both gained a parameter, but `sand_displace()`'s
own public behaviour is unchanged (filter -1 = any material, what every
existing caller, including explosions, still gets).

## Acid still broke the metal balance after masking - so the balance changed instead

Masking stopped acid from flinging *dirt*, but acid flinging *itself*
away from the metal it was dissolving was still real, and the acid-vs-
metal-vs-stone ordering test still failed on numbers that were always
close (single-digit step counts). Rather than patch around it, the
decision was to revise the balance directly: metal's `dissolvable`
dropped from 110 (acid's documented "counter", eating it faster than
stone) to 1 (resists acid almost entirely) - see
`Metal-Smelting-Plan.md`'s Numbers table for the full account, including
why 1 and not 0 (immune materials drop out of the generated reaction
docs). The test's assertions were rewritten to match the new intended
ordering, not just patched to pass.

## Acid needed the same radius as water, but not the same decay

Once the balance was settled, the ask was specific: acid's splash should
match water's *radius*, but must **not** decay the way water's does -
decaying the trigger chance would make acid's dissolve-balance mechanic
depend on how many *other* splashes had already happened in the
simulation, which is not a property a material balance number should
have. Water and acid ended up on genuinely different gates in
`splash_displace()`: water rolls against the decaying `splash_chance`;
acid always fires, unconditionally, at its own radius.

Water's own radius was independently toned down to 3 shortly after
(acid kept 5), so the two constants split into
`SAND_SPLASH_RADIUS_WATER` / `SAND_SPLASH_RADIUS_ACID` rather than one
shared value.

## Undecayed acid still needed *some* throttle - just not a decaying one

An always-fires trigger at full radius, applied to every column of a bulk
acid pour landing hard in the same step, reads as one chaotic explosion
rather than a splash - confirmed to have nothing to do with density
(`can_impulse_enter()` in `sand.c` ignores density entirely; any flying
grain shoulders aside any non-static occupant). The fix preserves "does
not depend on history": `sand_t::acid_splashes_this_step` caps how many
acid splashes can fire, reset to 0 at the top of *every* `sand_step()` -
throttles simultaneous triggers within one step without threading
anything from one step's outcome into the next, unlike water's
`splash_chance`.

## A dead end worth recording: pushing foam further doesn't work

A later ask - give displaced water a chance to render with the existing
foam dither, the same effect already used for exposed rim cells - turned
out to already be true by construction: a fully isolated water cell (no
occupied neighbour at all) is exactly the shape a flying droplet has, and
already hits the foam system's maximum curvature bucket, the same 7-of-8
dither chance any heavily-exposed rim cell gets. Trying to push that one
case to an unconditional 8-of-8 ("always foams") broke two tests
(`test_foam_moves_between_frames`, `test_foam_never_stalls_between_frames`)
that exist specifically to guarantee foam can always read plain
somewhere in its hash/phase cycle - unconditional foam is not a denser
dither, it is a solid fill, which stops meaning "the water is moving."
Reverted; nothing needed changing, the mechanism already reached this
case.

---

## What shipped

- `sand_displace_material()` (`sand.c`/`sand.h`) - a material-masked
  displacement, alongside the unmasked `sand_displace()` every existing
  caller keeps using unchanged.
- `splash_displace()` (`sand_liquid.c`) - water and acid only, called from
  both the water/acid-lands-hard-on-liquid hook and the wall-rebound hook.
- Water: `SAND_SPLASH_RADIUS_WATER` (3), gated by a decaying
  `sand_t::splash_chance` (guaranteed first splash, sharp falloff after).
- Acid: `SAND_SPLASH_RADIUS_ACID` (5), always fires, capped per step by
  `sand_t::acid_splashes_this_step` (reset every `sand_step()`).
- `MATX_METAL`'s `dissolvable` revised 110 -> 1 as a direct consequence of
  building this feature, not a pre-planned change - see
  `Metal-Smelting-Plan.md`.
