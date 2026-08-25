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
   stays 16 either way - it is already padded. **If the material reacts
   to fire at all** - it can catch, it is itself a heat source, it
   conducts heat, it smokes, it does something other than vanish when
   quenched, or it flares a flame - it also needs a row in the *second*
   table, `reaction_t reactions[]` (same header, same file). Rows not
   given default to all-zero, which reads correctly ("never catches,
   never a heat source, ...") for a material with no reactions at all,
   so most materials never touch this table. See `material.h`'s own
   comment on `reaction_t` for why this is a second table rather than
   more fields on `materials[]`.
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

## A material whose reaction changes its KIND has to latch that kind's flag

This one came from building fire chemistry past a single material
(gas/fire) into several (wood, fire, steam, ember), and it is worth its
own section because it is invisible to almost every test that could
catch it.

`sand_t` carries a `may_have_<kind>` flag per movement kind
(`may_have_liquid`, `may_have_gas`) plus `may_have_burning` for
reactions, and every one of the passes those flags gate **early-returns**
when its flag is clear - that is the entire point of the flag, the cheap
skip that keeps a screen of sand from paying for a gas pass it does not
need. `sand_set()`/`try_spawn_one()` set the right flag(s) whenever a
material is placed *from outside the simulation*. That covers a player
painting materials, but it does not cover a material a *reaction*
creates mid-step: fuel igniting into a gas (wood into a static ember,
but gas straight into fire), or - once heat conduction and quenching
exist - a liquid boiling into steam. Every one of those is a cell of a
NEW kind appearing on the grid from inside `sand_reactions.c`, not
through `sand_set()` at all, so nothing sets its `may_have_*` flag
unless the reactions code does it itself.

Get this wrong - create the cell, forget the flag - and the bug is a
cell that sits frozen on the grid forever: the pass that would move or
react it again gated on that flag and now never runs. It is invisible
to almost every test, because the flag is usually ALREADY set some
other way (another gas cell already on the grid, say), which is exactly
what makes it dangerous - the one test that would catch it is a scene
with *nothing else* of that kind already present, which is not the
scene anyone writes by default.

The fix that shipped: every place in `sand_reactions.c` that creates a
cell goes through one function, `place_reacted()`, which sets the cell
*and* latches every `may_have_*` flag the new material's kind and
`reaction_t.burns` value need, in the same independent-`if` shape
`sand_set()` itself uses (and for the same reason stated there: a
material can need more than one flag at once, so an `else if` chain
would silently shadow one of them). One function, one place to get this
right, instead of re-deriving it at every call site that creates a
cell. `suite_sand.c` has a test built specifically around this failure
mode - a quench that produces steam in a scene with no other gas
anywhere on the grid, so nothing else could accidentally keep the gas
pass armed - see `test_creating_steam_arms_the_gas_pass`.

## The ember decision: when the obvious material is the wrong one

Worth recording as a worked example, because the instinct it corrects
is a reasonable one. The obvious way to make wood burn is: wood touches
fire, wood becomes fire. It is the same shape gas already uses, it is
less code, and it is wrong for wood specifically, in a way that only
shows up once you ask what happens on the very next step.

Fire is `KIND_GAS`. A wood cell that became fire would rise and
disperse on the next `sand_step_gas()` pass, exactly like any other
fire cell - which means a log would dissolve into a rising flame that
drifts away, often before it gets a turn to ignite the log next to it.
The burn stalls or races depending on nothing the player did. The
symptom would not even be obviously wrong in a quick look - fire IS
supposed to rise - which is what makes this the kind of mistake that
ships if nobody asks the second question: not "does this ignite?" but
"does this still look like a burning log ten steps later?"

The fix was a second material, `MAT_EMBER`, that splits wood's burn
into two jobs instead of asking one material to do both: a `KIND_STATIC`
heat source that stays exactly where the wood was (igniting, decaying,
eventually burning out, all without moving), and a separate, ordinary
`MAT_FIRE` flame the ember periodically flares upward
(`reaction_t.flare`) purely for looks. Two materials, each doing one
simple thing, instead of one material asked to be both a heat source
and a moving flame at once. The general question this leaves behind for
the next material: when something reacts into another material, ask
whether the REACTED-INTO material's own movement rules still make sense
for what just happened - "wood catches fire" sounds right until "fire
floats away" turns out to be exactly the wrong behaviour for a log.

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
