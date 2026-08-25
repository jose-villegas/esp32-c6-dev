# Adding a Material

A practical checklist, worked out by actually building the fourth
material (`MAT_GAS`) end to end. Read [`Sand-Simulation.md`](Sand-Simulation.md)
first if you have not - this assumes you already know what `material_t`,
`material_kind_t`, and the main sweep's no-double-move guarantee are.

## The two-part question

Every material decomposes into the same two questions, and they are
independent of each other:

1. **Does it move like something that already exists?** `KIND_POWDER`
   (falls, piles at an angle of repose) and `KIND_LIQUID` (falls, spreads
   flat as an amount) are the two shapes on offer. If your material's
   movement is genuinely one of these, reusing the existing kind and
   just giving it its own row in `materials[]` - different density,
   slip, repose, scatter - is the *entire* implementation. This is the
   common case, and the reason the material table is designed the way
   it is: "adding a material is a row here rather than a branch in the
   movement code" (`material.h`'s own header comment).

2. **Does it need a new KIND?** Only if the movement shape itself is new
   - as it was for gas (`KIND_GAS`, rises instead of falls). This is the
   harder path, covered below, and it was genuinely harder than adding a
   row: it needed a new file, a second sweep pass, and three attempts to
   get the performance right.

If your answer to (1) is yes, stop reading here and go do that - a new
row in `materials[]` plus a palette entry (see "The mechanical part"
below) is the whole job.

## Design questions worth working through before writing code

These came up designing gas, in roughly the order they became unavoidable.
Not all of them apply to every new `KIND` - but check each one, because
skipping one silently is how a material ships broken in a way host tests
might not catch (see "The lesson that generalises" below).

**Does the main sweep's no-double-move guarantee hold for this
material's own primary direction?** The main sweep sweeps *against*
gravity, so any move gravity-ward lands in already-visited territory.
If your material moves gravity-ward too (even partially, like a liquid's
fall-then-slide), it can join the main sweep. If it moves in some other
fixed direction - anti-gravity, like gas, or something else entirely -
it needs its **own pass**, swept in whichever order makes *that*
direction's moves land in already-visited territory. Get this wrong and
the symptom is dramatic: a grain that should move one cell a step
teleports across the whole grid in one frame, because the sweep re-visits
a cell it just moved into.

**Can it reuse the existing movement primitives, direction-inverted, or
does it need genuinely new movement logic?** `try_fall_or_scatter()`/
`try_slide()` (declared in `sand_priv.h`, `_impl` bodies there, real
wrappers in `sand.c`) take a plain `(dx, dy)` direction and are not
hardcoded to "down" - passing a negated direction produces correct
"rise and slide" behaviour for free. Whether this is enough depends on
what the material is meant to look like: reusing them gets you rising/
falling plus diagonal sliding under friction, but **not** flat spreading
- that is what `equalise_liquids()`/`equalise_gas()`'s own *second* sub-pass
does, and skipping it (assuming the reused primitives are the whole
story) is exactly the mistake gas's own design almost made. If the
existing primitives genuinely do not fit the movement shape at all, a
new material needs its own logic - at which point think hard about
whether it is really a new `KIND`, or a variant of an existing one.

**Whole-grain or mass-based?** `KIND_POWDER` moves whole grains
(`move_to()`, a swap); `KIND_LIQUID` moves an *amount* per cell
(`give_mass()`/`pour_into()`, 1-15 via `CELL_VARIANT`). Whole-grain is
the smaller diff if an existing kind's primitives are being reused (as
above) - but it means the visual result is discrete grains, not a
smoothly thinning cloud. This is a real product/visual decision, not one
the code makes for you.

**What is its density, relative to every other material?** `can_enter()`/
`move_to()`'s displacement rule is one-directional: denser only ever
displaces lighter, never the other way round. Pick a density that makes
every displacement relationship you actually want come out true - gas's
`10` (between empty's `0` and water's `30`) lets sand and water sink
*through* it for free, with zero gas-specific code, simply because the
existing displacement rule already runs in the main sweep whenever
something denser tries to move into a gas-occupied cell. The flip side:
gas can only rise through cells *lighter* than itself, so it cannot
bubble up through water sitting above it - a real, accepted limitation of
a one-directional rule, not a bug to chase unless bubbling-through-liquid
is an actual requirement.

**Does it need `slip`/`repose` to mean "no resistance", like a liquid,
even if it is whole-grain?** Gas's `slip = 255`/`repose = 0` copy water's
values exactly, even though gas moves through the powder primitives, not
the liquid ones - `slide_chance()` and `driven_by_gravity()` both already
treat those values as "never held back", so a whole-grain material can
still get liquid-like freedom of movement just by setting the same
numbers.

**Does the movement direction interact with `driven_by_gravity()`
correctly?** This one is subtle and easy to get backwards.
`driven_by_gravity()`'s friction check is a dot product against the
*real* gravity vector - if a material's own primary direction is not
gravity itself (gas's is the negation of it), its slide vectors must be
checked against a matching negated gravity vector too, or the dot
product comes out negative for every slide and the material never slides
at all. `sand_gas.c` builds its own `driven_gas[][]` table against
`(-gx, -gy)` for exactly this reason - reusing the main sweep's own
`driven[][]` (built against the forward vector) would silently break
every gas slide.

## The mechanical part

Once the design questions above are answered:

1. **`material.h`**: add the new `material_id_t` enum value, before
   `MAT_COUNT`.
2. **`material.c`**: add a `materials[]` row (kind, density, slip,
   repose, scatter, name) and a `palette[]` row (`SHADES(lo, hi)`,
   replacing one `UNUSED` slot, same enum position). `MATERIAL_MAX`
   stays 16 either way - it is already padded.
3. **If reusing an existing `KIND`**: that is the whole implementation.
   Write host tests (see below) and you are done.
4. **If it needs a new `KIND`**: a new file (`sand_<name>.c`), mirroring
   `sand_liquid.c`'s or `sand_gas.c`'s shape - a `sand_step_<name>()`
   entry point declared in `sand_priv.h`, called from `sand_step()`
   after the main sweep and before `finalize_settling()` (so
   `BLOCK_ACTIVE` reflects the whole step, not just the main sweep's
   share of it - see the comment at that call site in `sand.c`). Add a
   `may_have_<name>` flag to `sand_t` (mirrors `may_have_liquid`/
   `may_have_gas`) set wherever the material gets placed
   (`sand_set()`/`try_spawn_one()`), checked both inside the new pass
   (cheap early-out) and, once the pass exists and is measured, at its
   call site in `sand_step()` too (avoids marshalling arguments for a
   call that will immediately return - see `sand_step_gas()`'s own call
   site for the pattern).
5. **`step_one_grain()`** (`sand.c`): the new `KIND` needs a branch, even
   if it is just "skip - handled by its own pass", exactly like `KIND_GAS`
   is skipped there with a comment explaining why.
6. **`app_sand.c`**: add the new material to the paintable-materials
   brush list if it should be usable in the real app. Separate,
   app-level concern from the core simulation - easy to forget since
   none of the simulation's own tests exercise it.

## The lesson that generalises: shared code across two hot call sites

If step 4 applies - a new pass reusing an existing kind's movement
primitives - the exact way those primitives get shared between the main
sweep and the new pass matters more than it looks like it should. Three
attempts, each measured on real hardware, each wrong in a different way:

1. Just remove `static` from the primitives so the new file can call
   them. **Loses inlining at the ORIGINAL call site** (the main sweep's),
   which had been relying on the compiler inlining a `static` function
   used once per grain - regressed two frame-budget tests by ~26%,
   exactly reproducible, with neither test ever touching the new material
   at all.
2. Move the whole primitive chain to a header as `static inline`, so both
   files get their own independently inlinable copy. Fixes (1), but now
   the NEW call site (the new pass) also gets a full inlined copy of a
   large call graph - measured to nearly double a worst-case test's time,
   again exactly reproducible, again without that test touching the new
   material.
3. **What actually works**: keep the primitives `static inline` in the
   header (so the hot, original call site stays fully inlined, exactly
   as before), but give the new pass an ordinary, non-inline wrapper
   function - defined once, real linkage, calling the inline version
   internally - to call instead. One genuine function-call's worth of
   overhead from the new pass, zero code duplicated into it, and the
   original hot path untouched.

The general shape, for whatever the next material needs: **inlining a
function into MORE call sites is not free just because inlining into
ONE call site was necessary.** Flash is a cache-constrained resource on
this chip (32 KB instruction cache, see the "Performance discipline"
section of `Sand-Simulation.md`), and a function's own compiled size at
each call site is part of that budget, not just its execution time. If a
shared hot-path function needs to be called from a second place, measure
whether that second place is hot enough to *need* its own inlined copy
before giving it one - a plain function call across translation units is
often the right default, not the fallback.

The full numbers, and the exact code, are in
[`Simulation-Lessons.md`](Simulation-Lessons.md)'s gas material section
and `sand_priv.h`'s own comments above `try_fall_or_scatter_impl()`.

## Testing

Follow `suite_sand.c`'s existing conventions - host-portable, direct
assertions, not visual inspection (see `docs/Testing-Guide.md` for why).
The gas tests are a reasonable template for a new `KIND`:

- Basic movement under ordinary gravity, and under at least one other
  gravity direction (inverted, and/or tilted/diagonal) - this project's
  simulation supports full 8-direction tilt-steered gravity, and a
  material's movement code needs to be genuinely direction-generic, not
  just correct for straight down.
- Blocked by a solid material with no gap (mirrors
  `test_nothing_displaces_stone`).
- Density-based displacement, both directions if both are meant to work
  (mirrors `test_sand_sinks_through_water`/`test_water_does_not_sink_through_sand`).
- Grain/mass conservation across every one of the 8 gravity directions
  in one test (mirrors `test_grains_are_never_created_or_destroyed`).
- If the material needs its own pass: a wake-propagation test confirming
  the pass explicitly wakes the blocks it moves through, with sleeping
  enabled - the class of bug that a missing `wake_block_and_neighbors()`
  call produces is invisible to every OTHER test, because the block-
  sleeping system is opt-in and most tests do not enable it.
- If the material spreads/disperses: a test confirming it actually
  spreads across more than its starting position after many steps, not
  just that it moves at all - a material that only reuses gravity-ward
  primitives without a proper spread pass will pass every "it moves"
  test while still piling into a heap instead of dispersing, which is
  wrong for a gas or a smoke and easy to miss without a test built
  specifically to catch it.

## Related

- [`Sand-Simulation.md`](Sand-Simulation.md) — how the simulation works
  today, including gas's own section.
- [`Simulation-Lessons.md`](Simulation-Lessons.md) — the full discovery
  narrative, including the exact numbers behind the inlining lesson
  above.
- `docs/Testing-Guide.md` — the host/device test split this guide's
  testing section assumes.
- [`Architecture.md`](Architecture.md) — the "what KIND should this be"
  decision diagram there is this guide's step 2 as a picture, plus the
  exact device-verification commands step 6 assumes you already know.
