#include <stddef.h> /* NULL - material.h does not pull it in, and
                      * whether anything else does is a property of
                      * the toolchain rather than of this file */
#include "material.h"
#include "util/intmath.h" /* im_len() - see material_set_gravity() below,
                             * which measures gravity the same way
                             * build_xflow() in sand.c does */

/*=============================================================================
 * The table.
 *
 * `const`, so it lives in flash rather than RAM. Adding a material is a row.
 *===========================================================================*/

const material_t materials[MATERIAL_MAX] = {
    [MAT_EMPTY] =
        {
            .name = "empty",
            .kind = KIND_NONE,
            .density = 0,
        },

    [MAT_SAND] =
        {
            .name = "Sand",
            .kind = KIND_POWDER,
            .density = 60,
            /* Buried sand locks up quickly - this is what stops a floor of it
         * skating sideways on the faintest tilt. */
            .slip = 96,
            .repose = 7, /* about 35 degrees, dry sand */
            .scatter = 40,
        },

    [MAT_WATER] =
        {

            .name = "Water",
            .kind = KIND_LIQUID,
            .density = 30, /* lighter than sand, so sand sinks through it */

            /* Unused by a liquid: it does not slide, pile or scatter, it flows
         * between neighbours as an amount. Left at the values that mean "no
         * resistance", so that anything reading them generically still gets a
         * sensible answer. */
            .slip = 255,
            .repose = 0,
            .scatter = 0,

            .mobility = 255, /* VISCOSITY, inverted - see material.h's own
                              * comment on the field. Water is the runny
                              * one and moves on every step it can, which
                              * is exactly what every liquid did before
                              * this field had a second reader, so water's
                              * behaviour is unchanged by its arrival. */
        },

    [MAT_STONE] =
        {
            .name = "Stone",
            .kind = KIND_STATIC,
            .density = 200, /* nothing displaces it */
            .slip = 0,
            .repose = 0,
            .scatter = 0,
        },

    [MAT_GAS] =
        {
            .name = "Gas",
            .kind = KIND_GAS,
            .density = 10, /* between empty (0) and water (30), so sand and
                               * water sinking through it in the main sweep
                               * displace it automatically - not sensitive,
                               * anywhere from about 1 to 25 works the same */

            /* Same "no resistance" values as water, and for the same reason:
         * gas rises and slides via sand's own try_fall_or_scatter()/
         * try_slide() (see sand_gas.c), inverted, and a real angle of
         * repose or load resistance would stop it from spreading at all,
         * the opposite of what it is for. */
            .slip = 255,
            .repose = 0,
            .scatter = 120, /* well above sand's 40 - a visibly turbulent,
                               * wispy rise rather than a rigid column */

            /* THE LONGEST-LIVED thing in the air, where it had been the
         * shortest.
         *
         * The three airborne materials already agree about weight and
         * speed: steam is lightest and quickest (density 5, mobility
         * 160), smoke sits between (7, 120), gas is heaviest and slowest
         * (10, 96). Their lifetimes disagreed with all of it - gas faded
         * in about 120 steps against steam's 160 and smoke's 240, so the
         * heavy gas that ought to settle in a hollow was the first of the
         * three to go.
         *
         * Six is roughly 640 steps, about twenty seconds, two and a half
         * times smoke's. Steam condenses, smoke disperses, and a heavy
         * flammable gas pools and waits - which is what makes a pocket of
         * it something to build a trap out of rather than a puff of
         * colour.
         *
         * Measured, and it costs less than it looks. Half a screen of gas
         * runs about 450 us a step while it is alive, at either figure:
         * at equal population the per-step cost is the same. What a
         * longer life changes is how long the population stays high - at
         * 32 that screen is down to three cells by step 240, at 6 it is
         * still 20,589. Every budget here is per-step, so nothing on the
         * scoreboard moves; a room full of gas simply stays expensive for
         * twenty seconds instead of five, which is the player's doing. */
            .decay = 6, /* 15 ticks needed to clear a fresh grain,
                               * 256/32 = 8 steps average between ticks -
                               * ~120 steps, around 2 seconds at this app's
                               * ~60fps step rate. Gas is whole-grain, not
                               * mass-based, so it cannot thin out a
                               * saturated pocket the way water levels one
                               * (see sand_gas.c); decaying away is what
                               * keeps a held-down pour from just piling
                               * solid forever, on top of making it look
                               * like a gas instead of an immortal block.
                               * Starting point, not final - tune on device
                               * like every other constant here. */

            .mobility = 96, /* ~2.7 steps average between rises - was 32
                               * (~8 steps average), measured on device as
                               * too sluggish to read as rising at all.
                               * Visibly slower than sand's instant
                               * one-cell-per-step, without reading as
                               * stuck. Life ticks every step regardless of
                               * whether this roll succeeds (see
                               * tick_decay() in sand_priv.h), so
                               * slowing the rise does not also stretch the
                               * lifetime. Starting point, not final - tune
                               * on device like every other constant
                               * here. */

            .sight = 16, /* was the global SAND_GAS_SIGHT constant,
                               * now this material's own figure - see
                               * material.h's own comment on `sight` for
                               * why it moved. Same value, same
                               * reasoning as before: deliberately wider
                               * than water's SAND_LIQUID_SIGHT(8), the
                               * one parameter that encodes gas
                               * dispersing faster/further than water
                               * levels, given both use the same
                               * equalise_*() mechanism. */
        },

    [MAT_FIRE] =
        {
            .name = "Fire",
            .kind = KIND_GAS, /* rises and disperses through the exact
                                 * same pass gas does (sand_step_gas()
                                 * dispatches on kind, not material ID) -
                                 * tighter and shorter-lived than gas via
                                 * `sight`/`decay` below, not a different
                                 * mechanism. This replaces an earlier
                                 * KIND_STATIC version (immobile, never
                                 * buriable) - see
                                 * docs/Sand/Adding-a-Material.md and the
                                 * plan this redesign was built from for
                                 * why that changed. */

            .density = 15, /* strictly between gas's 10 and sand's
                                 * 60 - three things depend on this at
                                 * once: sand must still sink through
                                 * fire (needs fire < sand); fire must
                                 * not smother itself via its own gas
                                 * neighbours (smothered(), below in
                                 * sand_reactions.c, requires a
                                 * neighbour's density STRICTLY greater
                                 * than fire's, so anything at gas's
                                 * density or lower never counts); and
                                 * fire needs real headroom above gas to
                                 * mix with/displace what it just
                                 * ignited next to it. can_enter()
                                 * requires strictly greater density to
                                 * displace, so equal density (e.g.
                                 * fire == gas) would make the two
                                 * simply block each other instead of
                                 * mixing - the same already-accepted
                                 * limitation gas has displacing more
                                 * gas. */
            .slip = 255,   /* no resistance, same reasoning as
                                 * gas's own row above */
            .repose = 0,
            .scatter = 120, /* matches gas's own figure - equally
                                 * turbulent rise, tune independently
                                 * later if it should read differently */

            .decay = 96,    /* much the shortest life in the air, and
                                * with gas now the longest the gap is
                                * wider than the 32 this used to cite:
                                * 15 ticks *
                                * 256/96 (~2.7 steps average between
                                * ticks) ~= 40 steps, under a second at
                                * ~60fps - fire burns out noticeably
                                * faster than gas fades. Starting point,
                                * not final - tune on device like every
                                * other constant here. */
            .mobility = 96, /* matches gas's own figure as a
                                * starting point - tune independently if
                                * fire should rise faster/slower than
                                * gas once seen in motion */
            .sight = 5,     /* noticeably tighter than gas's 16 -
                                * "tighter instead of sparse". Starting
                                * point, not final - tune on device */
        },

    [MAT_WOOD] =
        {
            .name = "Wood",
            .kind = KIND_STATIC, /* a log does not fall over or pile up
                                     * - it sits where it is drawn until
                                     * fire chars it into an ember (see
                                     * sand_reactions.c) */
            .density = 150,      /* above sand (60) and water (30), so
                                     * neither can displace a log - it
                                     * holds its shape under a pour, the
                                     * way a real log does not wash away.
                                     * Below stone (200), which stays the
                                     * one thing nothing else touches.
                                     * Starting point, not final - tune on
                                     * device like every other constant
                                     * here. */
            /* slip/repose/scatter/decay/mobility/sight all meaningless for a
         * KIND_STATIC material and left at zero, same as stone's own row. */
        },

    [MAT_STEAM] =
        {
            .name = "Steam",
            .kind = KIND_GAS, /* rises and disperses through the
                                     * same pass gas and fire do
                                     * (sand_step_gas() dispatches on
                                     * kind, not material ID) - lighter
                                     * and faster-dispersing than either
                                     * via the figures below, not a
                                     * different mechanism.
                                     *
                                     * Steam is specifically WATER THAT
                                     * GOT HOT: boiled through a
                                     * conductor, or flashed off a fire
                                     * a liquid put out. Fuel burning
                                     * out leaves MAT_SMOKE instead, a
                                     * separate row below. The two were
                                     * one material at first, on the
                                     * theory that both want the same
                                     * "pale, light, rises, fades"
                                     * behaviour - true of the physics,
                                     * false of the PICTURE: a fire
                                     * quietly puffing white
                                     * kettle-steam reads as a bug,
                                     * because a player can see there is
                                     * no water anywhere near it. Two
                                     * rows, two palettes. */

            .density = 5, /* below gas (10) and fire (15), so
                                     * both of those can rise through and
                                     * displace steam, mixing through it
                                     * - can_enter() requires strictly
                                     * greater density to displace, so
                                     * the reverse is not true: steam
                                     * cannot displace gas or fire. The
                                     * same already-accepted
                                     * one-directional limitation
                                     * can_enter() has everywhere else -
                                     * Steam being lighter than WATER
                                     * (30) is what makes it bubble up
                                     * through a pool rather than sit
                                     * under it - see try_bubble() in
                                     * sand_gas.c, which reads this
                                     * density against the liquid's and
                                     * is the one place the ordinary
                                     * displacement rule is deliberately
                                     * inverted. */
            .slip = 255,  /* no resistance, same reasoning as
                                     * gas's own row */
            .repose = 0,
            .scatter = 140, /* above both gas's 120 and fire's
                                     * 120 - a wispier, more turbulent
                                     * rise. Starting point, not final -
                                     * tune on device like every other
                                     * constant here. */

            .decay = 24,     /* matches ember's own figure as a
                                     * starting point, tune independently
                                     * later - roughly 160 steps, ~2.7s
                                     * at ~60fps, so a wisp of steam
                                     * visibly fades rather than either
                                     * lingering or vanishing at once. */
            .mobility = 160, /* noticeably faster than gas's 96 or
                                     * fire's 96 - steam should read as
                                     * rising eagerly off a boiling pot,
                                     * not drifting the way gas does.
                                     * Starting point, not final - tune
                                     * on device like every other
                                     * constant here. */
            .sight = 20,     /* wider than gas's 16 - a puff of
                                     * steam disperses generously rather
                                     * than staying a tight column the
                                     * way fire's own 5 does. Starting
                                     * point, not final - tune on device
                                     * like every other constant here. */
        },

    [MAT_SMOKE] =
        {
            .name = "Smoke",
            .kind = KIND_GAS, /* same pass as steam, gas and fire -
                                     * see steam's own row above. Smoke
                                     * is what FUEL leaves behind when it
                                     * burns out (reaction_t.smoke),
                                     * where steam is what WATER leaves
                                     * when it gets hot. Physically the
                                     * two behave almost identically,
                                     * which is why this row is mostly a
                                     * copy of steam's - the reason they
                                     * are separate materials at all is
                                     * that they must not LOOK alike, so
                                     * a puff rising off a fire reads as
                                     * soot and a puff rising off a basin
                                     * reads as a kettle. The palette
                                     * below is the real payload of this
                                     * row. */

            .density = 7, /* between steam's 5 and gas's 10 -
                                     * nothing here is load bearing, it
                                     * only keeps smoke and steam from
                                     * being mutually undisplaceable the
                                     * way two cells of EQUAL density are
                                     * (see fire's own density comment
                                     * for that limitation). Smoke being
                                     * the heavier of the two means steam
                                     * can rise through smoke, which is
                                     * the right way round for a basin
                                     * sitting over a fire. */
            .slip = 255,  /* no resistance, same reasoning as
                                     * gas's own row */
            .repose = 0,
            .scatter = 150, /* just above steam's 140 - smoke
                                     * curls a little more than steam
                                     * does. Starting point, not final -
                                     * tune on device like every other
                                     * constant here. */

            .decay = 16,     /* lower than steam's 24, so smoke
                                     * LASTS LONGER - decay is the chance
                                     * per step of losing a life tick, so
                                     * smaller is slower. Roughly 240
                                     * steps, ~4s at ~60fps: soot hangs
                                     * around after the fire is out,
                                     * where a wisp off a pot does not.
                                     * Starting point, not final - tune
                                     * on device like every other
                                     * constant here. */
            .mobility = 120, /* between gas's 96 and steam's 160 -
                                     * smoke climbs, but lazily, where
                                     * steam comes off a boil eagerly.
                                     * Starting point, not final - tune
                                     * on device like every other
                                     * constant here. */
            .sight = 24,     /* widest of any gas here (steam 20,
                                     * gas 16, fire 5) - smoke spreads
                                     * and thins into a haze rather than
                                     * holding a column. Starting point,
                                     * not final - tune on device like
                                     * every other constant here. */
        },

    [MAT_OIL] =
        {
            .name = "Oil",
            .kind = KIND_LIQUID,
            .density = 22, /* below water's 30, which is what makes oil
                              * float rather than sink when the two meet -
                              * see the density swap in
                              * move_liquid_grain() (sand_liquid.c).
                              * Above fire's 15 so it is not something a
                              * flame can shove around. Sand (60) still
                              * sinks straight through it. */

            .mobility = 140, /* VISCOSITY, inverted - see material.h. Oil
                              * moves on a bit over half its steps where
                              * water moves on all of them, so it lags
                              * behind a tilt and holds a slope for a
                              * moment instead of levelling instantly.
                              *
                              * Measured as steps for a column to spread
                              * to the far wall: water 8, oil 18. It was
                              * 90 (24 steps), which read as sludge
                              * rather than as oil - clearly slower than
                              * water is the point, not clearly slower
                              * than everything.
                              *
                              * This is also what stops oil and water
                              * tearing through each other. The two
                              * separate by swapping whole cells
                              * (sink_through_lighter_liquid()), and that
                              * swap runs at the pace of the SLOWER of the
                              * pair - so the interface settles over a
                              * second or so instead of thrashing every
                              * step, which is what it did when every
                              * liquid was equally runny. Starting point,
                              * not final - tune on device like every
                              * other constant here. */

            /* The same "no resistance" values water uses, and for the same
         * reason: a liquid does not slide, pile or scatter, it flows
         * between neighbours as an amount. */
            .slip = 255,
            .repose = 0,
            .scatter = 0,
        },

    [MAT_LAVA] =
        {
            .name = "Lava",
            .kind = KIND_LIQUID,
            .density = 45, /* above water (30) so lava sinks and water
                              * floats when they meet, below sand (60) so
                              * sand still sinks through lava. Both fall
                              * out of the existing rules; neither needs
                              * lava-specific code. */
            .slip = 255,
            .repose = 0,
            .scatter = 0,

            .mobility = 70, /* VISCOSITY, inverted - see material.h, and
                              * the slowest thing on the board by a wide
                              * margin: 44 steps to spread as far as water
                              * goes in 8 and oil in 18. Molten rock
                              * creeping,
                              * which is most of what makes lava read as
                              * dangerous rather than as orange water.
                              *
                              * This row had NO mobility at all until it
                              * was noticed on device. The field arrived
                              * for gases and only grew a liquid reader
                              * later, and an unset byte is zero. That did
                              * not freeze lava outright - the wall-
                              * rebound splash moved liquid without
                              * consulting the gate, back when that
                              * mechanism still existed (removed
                              * 2026-08-30, see git history) - but it
                              * made it twelve times slower than intended,
                              * measured at 249 steps to cross what takes
                              * 20 here. Slow enough to look deliberate,
                              * which is why it lasted. Zero now reads as
                              * free-flowing so the next liquid to forget
                              * this errs towards water, where the mistake
                              * is obvious - see liquid_may_move() in
                              * sand_liquid.c.
                              * Starting point, not final - tune on device
                              * like every other constant here. */

            .decay = 0, /* MUST stay 0, and this is not a style
                              * choice. decay != 0 is what switches the
                              * variant nibble from meaning "how much of
                              * this cell is full" to meaning "life
                              * remaining" (see material.h's top comment),
                              * and tick_decay() would then eat a lava
                              * cell's MASS as though it were a lifespan.
                              * Lava is the first material that is both a
                              * liquid and a heat source, so it is the
                              * first place those two uses of the nibble
                              * could collide. Immortal is also simply
                              * what lava should be: it cools by touching
                              * water, not by waiting. */
        },

    [MAT_ACID] =
        {
            .name = "Acid",
            .kind = KIND_LIQUID,
            .density = 38, /* between water's 30 and lava's 45: acid
                              * sinks through water and floats on lava,
                              * both of which fall out of
                              * sink_through_lighter_liquid() with no
                              * acid-specific code. Sand (60) still sinks
                              * through it, which matters - a grain has to
                              * get INTO the acid to be eaten by it. */
            .slip = 255,   /* the usual "no resistance" values a liquid
                              * leaves these at - see water's own row */
            .repose = 0,
            .scatter = 0,

            .mobility = 220, /* VISCOSITY, inverted - see material.h.
                              * Just short of water's 255: acid is runny,
                              * and being fractionally slower is enough to
                              * read as heavier without behaving like oil.
                              * Starting point, not final - tune on device
                              * like every other constant here. */
        },

    [MAT_GLASS] =
        {
            .name = "Glass",
            .kind = KIND_STATIC,
            .density = 200, /* stone's own figure, and for the same
                              * reason: nothing displaces it, and it
                              * smothers a buried flame the way stone
                              * does. Glass differs from stone in what
                              * ACID does to it, not in how it sits. */
            .slip = 0,
            .repose = 0,
            .scatter = 0,
        },

    [MAT_DIRT] =
        {
            .name = "Dirt",
            .kind = KIND_POWDER,
            .density = 62, /* just above sand's 60. Soil is sand with
                              * water and organic matter packed into the
                              * gaps, so it should sink through a loose
                              * pile rather than float on it - and being
                              * only just heavier keeps that slow */
            .slip = 64,    /* stickier than sand's 96: damp soil clumps
                              * where dry sand runs */
            .repose = 11,  /* ~48 degrees against sand's ~35. A bank of
                              * earth holds a much steeper face than a
                              * dune does, which is most of what makes it
                              * read as soil rather than as brown sand */
            .scatter = 12, /* well under sand's 40 - it lands where it
                              * falls instead of skittering */
        },

    [MAT_SNOW] =
        {
            .name = "Snow",
            .kind = KIND_POWDER,
            .density = 15, /* Under oil's 22 and well under water's 30,
                              * so snow FLOATS on both - can_enter() lets
                              * the denser one displace it and that is the
                              * whole mechanism. Snow sitting on top of a
                              * pool is right, and it also puts the snow
                              * where it is useful: on the surface, in
                              * reach of whatever is above it. */
            .slip = 64,    /* Stickier than sand's 96. Snow clumps, and a
                              * bank that holds its shape is what makes it
                              * possible to pack snow ONTO a glass pane and
                              * have it stay there long enough to matter. */
            .repose = 9,   /* ~42 degrees, steeper than dry sand's ~35 -
                              * again so a bank holds. */
            .scatter = 90, /* High, and the one purely cosmetic number
                              * here: falling snow drifts instead of
                              * dropping straight, which is most of what
                              * makes it read as snow rather than as pale
                              * sand. */
        },

    /* ONE row for all sixteen extended materials - see MAT_EXTENDED in
     * material.h. The sweep reads this per cell per step and must not care
     * which extended material a cell is, so they all move identically:
     * they do not move at all, and nothing displaces them.
     *
     * That sharing is the whole trick, and also the whole limit. Anything
     * that needs its own density, kind, slip, repose or scatter cannot
     * live here and needs one of the ordinary slots. */
    [MAT_EXTENDED] =
        {
            .name = "Extended", .kind = KIND_STATIC, .density = 200, /* stone's figure: undisplaceable, and it
                              * smothers a buried flame the way stone
                              * does */
        },
};

/*=============================================================================
 * The reaction table - see material.h's own comment on reaction_t for why
 * this is a second table rather than more fields on materials[] above.
 *
 * Rows not given here default to all-zero, which reads correctly for every
 * field: never catches, never a heat source, never conducts, never smokes,
 * vanishes on quench, never flares. Adding a material that does not react
 * at all - most of them - costs nothing here.
 *===========================================================================*/

const reaction_t reactions[MATERIAL_MAX] = {
    /* Water's only reaction row, and it exists for one field.
     *
     * Everything water does to other materials - quenching a fire,
     * boiling into steam, melting snow - is driven from the OTHER side,
     * which is why it had no row here at all. Wetting is driven from the
     * other side too, and that is exactly the problem: `soaks` belongs to
     * sand and soil, and they cannot tell water from oil. */
    [MAT_WATER] =
        {
            .wets = 1,

            /* Lets acid's ordinary dissolve roll (step_one_dissolver_cell(),
             * sand_reactions.c) land on water at all - the same field sand
             * and wood already use, not a new mechanic. Nothing is EATEN
             * here: the outcome for a water neighbour is a material swap
             * (dilution), not a vanish - see
             * SAND_ACID_DILUTE_TO_WATER_CHANCE's own comment in sand.h for
             * that half. 220, the same tier as ash: water gives way to the
             * interaction about as readily as the softest solids do,
             * appropriate for something that is not being destroyed by it.
             * Starting point, not final - tune on device like every other
             * constant here. */
            .dissolvable = 220,

            /* Low, deliberately: a poured stream should be able to
             * occasionally outpace evaporation over a hot stone crust and
             * pool up a little, rather than every drop flashing to steam
             * the instant conducted heat reaches it (see conduct_heat(),
             * sand_reactions.c). Acid, below, sits at the opposite end of
             * this same field. Started at 24, raised about 20% (~9.4% ->
             * ~11.3% per step) once reported as resisting more than
             * wanted. Starting point, not final - tune on device like
             * every other constant here. */
            .boils = 29,
        },

    [MAT_ACID] =
        {
            /* The only thing that dissolves anything. 60 in 256 is roughly
         * one bite every four steps per acid cell, which eats a pile of
         * sand at a pace you can watch rather than one that removes it
         * between frames.
         *
         * Every bite costs the acid a unit of its own mass, exactly as
         * quenching costs water a unit (they go through the same
         * pay_quench_cost()). That is not decoration: without it a single
         * cell of acid would eat an unbounded amount of anything and
         * still be a single cell, which is the same mistake oil-soaked
         * ash made before soaking became a real transfer. A puddle of
         * acid has a budget, and when it is spent the puddle is gone. */
            .dissolves = 60,

            .fizz = 40, /* "about one bite in six" - roughly one visible
                         * puff every four steps once combined with
                         * .dissolves. DROPPED SHARPLY to 6 on 2026-09-01,
                         * same session, on a report of producing far too
                         * much gas - but that drop landed at the same time
                         * .evaporates (a SEPARATE, unrelated field -
                         * ambient acid-turns-to-gas, nothing to do with
                         * dissolving) was ALSO driven down hard, and with
                         * both knobs low at once "acid was the cause"
                         * stopped reading as rare and started reading as
                         * gone. Walked back up in steps (16, then 32) and
                         * settled back at the original 40 once .evaporates
                         * alone was judged enough to cover the "ambient"
                         * gas side on its own - .fizz only needs to answer
                         * "acid was the cause" for actual
                         * dissolving again. Starting point, not final - tune
                         * on device like every other constant here. */

            .evaporates = 1, /* the rarest a single byte-wide roll can
                             * express - 1 in 256 per cell per step. Still
                             * read as too frequent on device once a real
                             * puddle (many cells, all rolling every step)
                             * was watched rather than a single cell - see
                             * step_one_dissolver_cell()'s own comment for
                             * the extra gate (now 1-in-60, effective 1 in
                             * 15360) that takes the natural, per-material
                             * rate the rest of the way down.
                             * sand_set_evaporates()'s override path
                             * (tests, debug) bypasses that extra gate and
                             * uses whatever chance it is given exactly.
                             * Starting point, not final - tune on device
                             * like every other constant here. */

            /* Acid boiled via conducted heat unconditionally before this
             * field existed. 255 is what keeps that behaviour effectively
             * unchanged now that boiling is gated by a roll at all, rather
             * than acid quietly going immune to it by omission - the
             * opposite end of this same field from water's own low
             * figure above. */
            .boils = 255,

            /* Not water's MAT_STEAM default - acid evaporating through a
             * hot wall should leave the same MAT_GAS it leaves everywhere
             * else it evaporates (see .evaporates and .fizz above), not
             * kettle-steam. */
            .boils_to = MAT_GAS,
        },

    [MAT_OIL] =
        {
            /* Catches readily, but only where it meets air - see
         * material.h's own comment on `needs_air`. Without that flag a
         * spark landing on a pool would light every cell of it in a
         * single pass (this file's scan order propagates ignition
         * through a connected pocket within one step - see the top
         * comment), which is a detonation, not a slick burning.
         *
         * 50 in 256 is roughly one catch every five steps per adjacent
         * flame: brisk enough that oil reads as obviously more eager
         * than wood's 6, slow enough to watch a surface layer light up
         * rather than blink. Starting point, not final - tune on device
         * like every other constant here. */
            .flammability = 50,
            .needs_air = 1,
            .ignites_to = MAT_FIRE, /* burns straight to flame, unlike
                                     * wood: there is no log left to
                                     * smoulder, the fuel simply goes.
                                     * The flame rises off, exposing the
                                     * layer beneath, which is what eats
                                     * the pool downward */

            /* Lets acid's ordinary dissolve roll land on oil - the same
             * field water uses for its own acid interaction (see its own
             * comment above), but the OUTCOME is different: oil always
             * becomes acid on a successful bite (step_one_dissolver_cell(),
             * sand_reactions.c), not a biased coin flip between the two
             * materials the way water's is. Started at 40 ("slowly
             * dilutes", explicitly asked for), dropped to 1 (the rarest a
             * single byte-wide roll can express) once still reported too
             * fast, then brought back up to 16 once 1 read as too slow -
             * about one bite in sixteen, between the two extremes already
             * tried. Starting point, not final - tune on device like
             * every other constant here. */
            .dissolvable = 16,
        },

    [MAT_LAVA] =
        {
            /* A heat source that happens to be a liquid, and the clearest
         * proof the movement and reaction axes really are independent:
         * KIND_LIQUID in materials[] above, `burns` here, and not one
         * line of code anywhere knows about the combination. */
            .burns = 1,

            .quench_to = MAT_STONE, /* water puts lava out by turning it to
                                   * rock, rather than by making it
                                   * vanish. The water pays a unit of its
                                   * own mass for it, exactly as it does
                                   * quenching a fire (pay_quench_cost()),
                                   * so a small puddle cannot pave an
                                   * ocean of lava for free. */

            .flare = 16, /* well below ember's 48: lava licks the
                                   * occasional flame rather than burning
                                   * with one. Mostly so a pool reads as
                                   * dangerous rather than decorative.
                                   * Starting point, not final. */

            /* SEALED IN IS NOT SAFE, BUT A PULSE SHOULD BE RARE - 1 in 256
             * (~0.4%), per step, once a pool is COVERED FROM ABOVE - see
             * reaction_t.vent_chance's own comment for the design and
             * try_vent()/covered_from_above() (sand_reactions.c) for the
             * mechanism. The rarest a single byte-wide roll can express
             * (same floor sand_set_evaporates()'s own per-material figure
             * starts from, material.c's MAT_ACID row), deliberate: this
             * rolls every step a cell STAYS covered, not once per pool, so
             * even this floor still fires roughly once every 256 steps per
             * covered cell - frequent enough that a sealed pool is not
             * permanently inert, rare enough that a vent reads as an
             * occasional, dramatic pulse rather than a constant leak.
             * Paired with SAND_VENT_REACH set high (sand.h) so that rare
             * pulse throws a lot of material when it does fire, instead of
             * a small, frequent trickle - a held-in eruption rather than a
             * hiss. sand_set_vent_chance() (sand.h) overrides this for
             * tests that need a fast, deterministic pulse instead of
             * waiting on this real figure. Starting point, not measured on
             * device. */
            .vent_chance = 1,

            /* No residue: lava never burns out (decay 0 above), so nothing
         * here would ever fire. No conducts either - lava IS the heat,
         * it does not pass someone else's along. */
        },

    [MAT_SAND] =
        {
            /* Acid eats sand readily - it is the obvious thing to point acid
         * at, and the one that shows what it does. */
            .dissolvable = 200,

            /* Wet sand slowly becomes soil. Far slower than dirt drinks -
         * 8 against 60 - because this is sand CHANGING rather than dirt
         * filling up, and it should read as a shoreline turning to mud
         * over time rather than as a puddle instantly making earth. */
            .soaks = 8,
            .soaks_to = MAT_DIRT,

            .heats_to = MAT_GLASS,
            .heat_chance = 16, /* 16 in 256 per adjacent heat source per
                              * step. Deliberately slow - glass should be
                              * something you set up and wait for, not
                              * something that happens whenever a spark
                              * lands on a dune - but 8 was slower than
                              * that, not just patient.
                              *
                              * Measured on a bed of eleven sand cells
                              * under a held flame, steps to half the bed
                              * and then all of it:
                              *
                              *     8 -> 52, 184     24 -> 17,  51
                              *    16 -> 18, 137     32 -> 17,  27
                              *
                              * 16 brings the first visible progress in
                              * about a third of the time while leaving
                              * full conversion a couple of seconds' work.
                              * 24 and up collapse that second number to
                              * well under a second, which turns glass
                              * into something a passing spark makes.
                              * Starting point, not final - tune on device
                              * like every other constant here. */
        },

    [MAT_GLASS] =
        {
            /* Conducts exactly as well as stone, and the sameness is the
         * point: glass and stone should differ in ONE thing - what acid
         * does to them - so choosing between them is a decision about
         * acid and nothing else. A second axis of difference would make
         * the choice a guess.
         *
         * It had no row at all until this was noticed, which meant
         * conducts defaulted to 0 and heat simply stopped at glass. A
         * stone vessel over a flame boiled its contents and a glass one
         * did not - backwards, since glass is the vessel you have to make
         * and the only one that survives acid. Nothing announced it: an
         * absent row reads as "no reactions", which is right for most
         * materials and was wrong for this one. */
            .conducts = 220,

            /* Glass BANKS heat in its own variant nibble rather than
         * transforming on contact, and at the top of that ramp it melts.
         * See reaction_t.heat_ramp in material.h for why accumulation
         * rather than a per-step roll: a roll has no memory, so it cannot
         * tell a brief fierce flame from a long slow one, and "long
         * exposure" is the entire point of this reaction.
         *
         * Sources ADD, so a pane walled in by lava climbs several times
         * faster than one with a single flame under it - that range is
         * deliberate. A source that goes out lets the pane drain back to
         * room temperature, and the drain is what makes the ramp mean
         * duration rather than lifetime total.
         *
         * `cools` at half the ramp is deliberately not much lower. It has
         * to be large enough that heat visibly DRAINS once the fire is
         * out, because that draining is the only thing that makes the
         * ramp mean duration rather than merely total exposure. */
            .heats_to = MAT_LAVA,
            .heat_ramp = 64,

            /* `cools` is the drain ONE level above ambient; it scales with how
         * far above ambient the cell already is (step_one_tempered_cell()).
         * So this pair is not a tug of war at a single fixed rate - 64 up
         * against 10, then 20, then 30 as it climbs.
         *
         * Measured on a pane with a held source, which is what these
         * numbers are for:
         *
         *   lava     shatterable in 12-26 steps, molten in 102-678
         *   fire     shatterable in ~65 steps, NEVER molten (peaks at 13)
         *
         * That split is the reason for the scaling. Fire should be able to
         * make glass fragile and should not be able to melt it; lava should
         * do both. A flat drain cannot express that - it was 12 against 6,
         * which took 152 steps just to reach shatterable and made the whole
         * mechanic feel like it was not working.
         *
         * One BRUSH of fire still does nothing much, and no ramp fixes it:
         * fire is a rising gas, so a single dab has drifted off the pane
         * within a couple of steps. Measured, it peaks around 6 whether the
         * ramp is 64 or 160. Heat has to be HELD against glass, which is
         * the right lesson for the player to learn from it. */
            .cools = 5,

            /* Shocked glass goes back to being sand, which closes the loop it
         * opened: sand fuses to glass under heat, glass returns to sand
         * when the heat is pulled out of it too fast. The player can
         * un-make the material without a second material and without
         * spending one of the two remaining slots. */
            .shatters_to = MAT_SAND,
        },

    [MAT_DIRT] =
        {
            /* Dirt's variant is MOISTURE, 0 dry to 15 saturated. It soaks up
         * any liquid it touches - `soaks_to` is left at zero, so what it
         * absorbs raises its own variant rather than turning it into
         * something else - and dries back out slowly.
         *
         * Drying at a twelfth of the soaking rate means a watered patch
         * stays useful for a while and does not stay useful forever, which
         * is what makes watering a thing you do rather than a thing you
         * did once. */
            .soaks = 60,
            .dries = 5,

            .dissolvable = 200, /* the same as sand: it is mostly sand */

            /* SMELTING. Dirt's variant is fully spent - a tone bit plus
         * SOIL_MOISTURE_BITS of moisture - so there is nowhere to bank a
         * `heat_ramp` the way glass and stone do; this has to be a
         * memoryless roll, like sand into glass. See
         * docs/Sand/Metal-Smelting-Plan.md for the design this row
         * follows.
         *
         * `heats_to` names an EXTENDED cell (MATX() sets the top nibble to
         * MAT_EXTENDED), which place_reacted() already routes to the
         * extended path - nothing needed to change there for a reaction to
         * produce an extended material.
         *
         * 10, slower than sand's 16 into glass: smelting is meant to be a
         * project, not something a passing flame does by accident. The
         * wet stage below spends this SAME roll driving off moisture
         * first (try_heat_transform() in sand_reactions.c), so saturated
         * dirt takes roughly eight successes - one per level of
         * SOIL_MOISTURE_MAX - to reach metal instead of one. */
        .heats_to    = MATX(MATX_METAL),
        .heat_chance = 10,

        /* IMPURE ORE. Metal-Smelting-Plan.md originally rejected a mixed
         * yield as "a new field serving exactly one material" and shipped
         * all-metal instead. First revision (2026-08-31) picked 40 in 256
         * (~16%) and read as mostly metal on device; a same-day rebalance
         * moved it to 90 (~35%), and that still read as too much metal.
         * Second rebalance, same day: 220 in 256 (~86%) of successful dry
         * smelts come out stone instead, leaving metal the genuinely rare
         * outcome (~14%) rather than the default one. See reaction_t.
         * flaw_to's own comment (material.h) for the clumping that keeps
         * this from reading as salt-and-pepper noise even at this share. */
        .flaw_to     = MAT_STONE,
        .flaw_chance = 220,

        /* RUINED BY HASTE. First revision (2026-08-31) picked 24 in 256
         * (~9%) and read as too rare to matter on device; a same-day
         * rebalance moved it to 128 (exactly half), and that still wasn't
         * strong enough. Second rebalance, same day: 235 in 256 (~92%) per
         * eligible roll, and no longer gated to spare a cell's very first
         * roll - see try_heat_transform()'s own comment on why that gate
         * could never actually deliver the guarantee it promised. Saturated
         * dirt gets SOIL_MOISTURE_MAX = 7 such rolls on its way to bone dry
         * - moisture 7 down through 1, all seven now equally at risk - so
         * the chance of surviving every one of them uncracked is
         * (1 - 235/256)^7, on the order of 0.000003%. Watering ore before
         * it fires is now, for all practical purposes, a guarantee of
         * ruining it, and can happen on literal first contact with no
         * warning - wet dirt reaching metal or stone at all should be a
         * rare surprise, not a normal outcome. */
        .spoils_to     = MAT_SAND,
        .spoils_chance = 235,
    },

    [MAT_SNOW] =
        {
            /* The only cold thing on the board, and the reason thermal shock
         * is legible at all. `chills` pulls a heat level out of a hot
         * neighbour and marks snow as cold for the shock rule - 40 in 256
         * so a bank cools a pane briskly without a single flake being an
         * instant crack.
         *
         * It melts in the exchange: heat has to go somewhere, and snow
         * that chilled a glowing pane for free would make an unlimited
         * heat sink out of a material that falls in a light drift. 120 is
         * high - snow near ANY heat source is short-lived, which is the
         * behaviour you want when you have to pack a bank onto a pane
         * that is already hot. */
            .chills = 40,
            .heats_to = MAT_WATER,
            .heat_chance = 120,

            /* And it melts in liquid, at its own far slower rate - see
         * reaction_t.thaws. 120 beside a flame is two steps; 4 in water is
         * nearer a second per touching face, which is long enough to watch
         * a drift land on a pond and ride on it before it goes. Snow is
         * lighter than water precisely so that it does. */
            .thaws = 4,
        },

    /* Steam and smoke have rows here for convection, and steam now for
     * one more thing - see .condenses below. Otherwise they are
     * byproducts that react with nothing else, which is the usual reason
     * a material skips this table entirely. */
    [MAT_STEAM] =
        {
            .warms = 48, /* the hotter carrier: water that has just boiled */

            /* The inverse of water's own evaporation: a small,
             * deliberately rare per-step chance that a 2x2 patch of
             * steam quietly turns back into a droplet of water - fake
             * condensation, a cosmetic touch, not a real thermal model
             * (no cold surface checked, no heat reading involved - see
             * reaction_t.condenses's own comment in material.h). Started
             * at 3 (roughly 1 in 85), halved to roughly 1 in 128 once
             * reported as happening more than a "small chance" should.
             * Starting point, not final - tune on device like every
             * other constant here. */
            .condenses = 2,
            .condenses_to = MAT_WATER,
        },

    [MAT_SMOKE] =
        {
            .warms = 28, /* cooler than steam and far longer lived, so a
                        * lower rate spread over more steps */
        },

    [MAT_STONE] =
        {
            /* Stone carries a temperature exactly as glass does - its variant
         * is heat, it frosts, it glows, and a cold shock cracks it into
         * sand. Same fields, same scale, same colours meaning the same
         * things, because a player who has learned to read one wall should
         * not have to learn the other.
         *
         * It reads the temperature and does NOTHING ELSE with it. No
         * `heats_to`, so it never melts however hot it gets; no
         * `shatters_to`, so chilling it does not break it. Both absences
         * are decisions:
         *
         *     stone   shows heat, survives it, and acid eats it
         *     glass   immune to acid, melts under sustained heat, and
         *             cracks when chilled while hot
         *
         * If stone melted there would be no vessel that holds lava
         * indefinitely and the choice would collapse into "glass, but it
         * dies". And rock does not thermally shock into anything - a
         * quenched slab spalls and cracks, it does not turn to sand, and
         * there is no honest byproduct to name here. Thermal shock is
         * glass's, which is also what makes glass worth making.
         *
         * Ramps at half glass's rate. Rock is the heavier thing and should
         * take longer to come up to temperature, and it is the more common
         * building material - a wall that glowed the instant a flame came
         * near would have the whole board lit up. */
            .heat_ramp = 32,
            .cools = 5,

            .dissolvable = 60, /* Stone gives way to acid now, just slowly -
                              * well under sand's 200, so a wall holds for
                              * a while and then does not. It used to be
                              * immune, and being immune made it the only
                              * thing acid could be kept in.
                              *
                              * MAT_GLASS took that job, which is the whole
                              * point of the change: a container you have
                              * to MAKE (sand plus sustained heat) rather
                              * than one you already had, and acid that is
                              * dangerous to everything you built the level
                              * out of. Glass is immune by omission - it
                              * simply has no `dissolvable` - which is the
                              * same route every other material takes. */

            /* 220 in 256 (~0.86) is the chance heat crosses ONE cell of
         * stone - see conduct_heat()'s own comment in sand_reactions.c
         * for the walk this actually drives. It attenuates with depth,
         * not a fixed reach: crossing d cells succeeds with probability
         * 0.86^d, so a thin wall conducts briskly and a thick one more
         * slowly, without a second tuning constant.
         *
         * Was 176 (~0.69), which measured far too timid once the scene
         * was one a player could actually build. 0.69^d falls off a
         * cliff: a thirteen-cell floor got through on ~0.8% of steps and
         * a sixteen-cell one on ~0.3%, so a hand-drawn basin either took
         * the best part of a minute to show its first wisp of steam or
         * looked completely inert. At 0.86 those same depths are ~14%
         * and ~9%, and a basin drawn at any thickness the brush can
         * produce starts boiling within a step or two of the fire
         * reaching it - measured by sweeping slab thickness 1..20
         * against a pour-brush-sized blob of fire, not estimated.
         *
         * Thickness still matters, just over a usable range rather than
         * an unusable one. Starting point, not final - tune on device
         * like every other constant here. */
            .conducts = 220,
        },

    [MAT_GAS] =
        {
            /* 255: gas catches the instant fire touches it, and - because
         * try_ignite() checks for 255 before ever drawing a random number -
         * costs no RNG draw doing it, exactly as a plain boolean flammable
         * flag used to. That is what keeps every existing gas/fire test,
         * and the device frame-budget captures that depend on their exact
         * random sequence, bit-identical after this table split. */
            .flammability = 255,
            .ignites_to = MAT_FIRE, /* the only fuel today; written out
                                     * explicitly rather than relying on
                                     * the "0 reads as MAT_FIRE" default,
                                     * since MAT_FIRE is what should be
                                     * here regardless of which enum value
                                     * happens to be 0 */
        },

    [MAT_FIRE] =
        {
            .burns = 1, /* the one heat source that exists today - see
                         * sand_reactions.c's dispatch, which now keys off
                         * this instead of CELL_MATERIAL(c) == MAT_FIRE */

            .residue = 40, /* chance in 256 that a burnt-out fire cell leaves
                         * MAT_STEAM behind - smoke, physically the same
                         * material a kettle's steam is (see MAT_STEAM's
                         * own row above). Lower than ember's 90: a flame
                         * guttering out on its own is a smaller, briefer
                         * event than a whole ember finishing a slow
                         * burn. Starting point, not final - tune on
                         * device like every other constant here. */

            .quench_to = MAT_STEAM, /* touching water no longer just
                                  * vanishes - it boils off, at the cost
                                  * of a unit of the water's own mass
                                  * (see step_one_burning_cell() in
                                  * sand_reactions.c). Steam is a
                                  * byproduct, not a free lunch: a pot
                                  * boiled dry should eventually run dry. */
        },

    [MAT_WOOD] =
        {
            /* Wood standing in wet ground buds FOLIAGE. Slow - 6 in 256 is
         * one bud every forty steps or so per cell of trunk touching wet
         * soil, and only while somebody keeps the ground watered.
         *
         * It used to bud a PLANT, and that was the source of the thin
         * green threads running up beside a trunk. Budding can only ever
         * happen where wood meets wet soil, which is the foot of the
         * trunk - so every bud was a sucker at ground level, and a sucker
         * is a grower: it climbed the outside of the trunk as a one-cell
         * column, wandering with the dithered gravity into a zigzag that
         * reads as a line of loose dots rather than as part of a tree, and
         * it was too thin to ever harden and stop.
         *
         * Confirmed by deleting it - the same seed with `sprouts` at zero
         * grows the same trunk with no threads anywhere on it.
         *
         * Foliage is the right thing to put there. It cannot grow, so it
         * cannot climb; it cannot fall; and it is what a bare trunk
         * wanting to come back to life should be producing anyway. It
         * takes a grower back out of the growth loop rather than adding
         * one, which this feature has needed twice already. */
            .sprouts = 6,
            .sprouts_to = MATX(MATX_LEAF),

            /* And a crowned trunk puts out new GROWTH, which is where a tree
         * gets taller now that hardening leaves no green tip behind.
         * Rarer than leafing: a bud is a whole new limb rather than a
         * frond, and it is the only thing that compounds, so it is the
         * number to turn down first if a forest gets away. */
            .buds = 32,
            .buds_to = MATX(MATX_PLANT),

            /* A trunk standing in water waters its own roots, at a third of
         * green growth's rate - bark is not a leaf. */
            .drinks = 12,

            /* 6 in 256 is roughly 43 steps of contact with a single flame
         * before it catches - a fire that has to work at it, which is
         * the whole point of "slowly consumed" (see sand_reactions.c's
         * top comment for why wood does not just ignite straight to
         * MAT_FIRE the way gas does). Starting point, not final - tune
         * on device like every other constant here. */
            .flammability = 6,

            /* Ignites into ITSELF. Wood's variant is how much of it is left to
         * burn, so catching fire means going to a full one - which is
         * exactly what place_reacted() writes - rather than becoming some
         * other material.
         *
         * This is where ember used to be, and where it went. It was a
         * whole material for the state of wood that is on fire, and it
         * differed from wood in seven fields of which only decay was in
         * the movement table. A state that needs its own row costs a slot;
         * a state the variant can hold does not. */
            .ignites_to = MAT_WOOD,

            /* 24 in 256 per step across 15 levels is ~160 steps, about 2.7s at
         * this app's step rate - a log that visibly smoulders rather than
         * one that either lingers forever or guts out at once. Ember's own
         * figure, kept: the burn did not change, only where it lives. */
            .burn_decay = 24,

            .residue = 90, /* well above fire's 40: a whole log
                                   * finishing its burn is a bigger, more
                                   * definite event than a flame guttering
                                   * out, and should leave smoke far more
                                   * often */

            .quench_to = 0, /* water on a burning log puts it OUT
                                   * rather than replacing it - the log is
                                   * still there, just no longer alight,
                                   * which is what step_one_burning_cell()
                                   * does for a burn_decay material. Ember
                                   * named MAT_STEAM here because there was
                                   * nothing left to put out: the ember WAS
                                   * the fire, so quenching it had to
                                   * replace it with something. */

            .flare = 48, /* chance in 256 per step that a burning
                                   * log emits a MAT_FIRE cell into an
                                   * empty cardinal neighbour. Wood is
                                   * KIND_STATIC and would otherwise be a
                                   * glowing brick with no flame licking
                                   * off it; the emitted fire rises on its
                                   * own through sand_step_gas(), so "wood
                                   * burning below, flame above" falls out
                                   * of this one field */

            .dissolvable = 160, /* slower than sand's 200: a plank holds
                                   * out a moment longer than a loose pile
                                   * does */
        },

};

/*=============================================================================
 * The palette.
 *
 * Built at compile time, so it is 512 bytes of flash and no RAM at all.
 *
 * Writing it out by hand would be 256 unreadable and unmaintainable literals,
 * so the material colours are interpolated by macro instead. Ugly to read once;
 * the alternative is either a table nobody can safely edit, or building it at
 * startup and paying for it in the resource there is least of.
 *===========================================================================*/

/* Channel `sh` of the way from `lo` to `hi`, out of 15. */
#define LERP_CH(lo, hi, shift, sh)                                                                                     \
    ((((((lo) >> (shift)) & 0xFF) * (15 - (sh)) + (((hi) >> (shift)) & 0xFF) * (sh)) / 15) & 0xFF)

#define LERP(lo, hi, sh)  ((LERP_CH(lo, hi, 16, sh) << 16) | (LERP_CH(lo, hi, 8, sh) << 8) | LERP_CH(lo, hi, 0, sh))

/* A ramp for `n` steps between two colours, for a material that needs its
 * sixteen entries built in more than one piece. */
#define SEG(lo, hi, i, n) GFX_RGB(LERP(lo, hi, ((i) * 15) / ((n) - 1)))

/* Glass, whose variant is a TEMPERATURE rather than a shade, so this is a
 * temperature scale - with room temperature in the MIDDLE of it and two
 * different things happening on either side.
 *
 * Below SAND_AMBIENT_HEAT a pane has been chilled and is FROSTED: pale,
 * near white, the way cold glass actually goes. Those levels exist so that
 * "snow is making this colder" is something the player can see. With
 * ambient at the bottom of the range there was nothing below it, so
 * chilling a resting pane changed no number and therefore no colour.
 *
 * At SAND_SHOCK_HEAT the ramp BREAKS into a glow and climbs to lava's own
 * brightest, which is what a pane at 15 is about to become. The break is
 * the largest colour step in the ramp on purpose: at that level the
 * material stops merely cooling when something cold touches it and starts
 * shattering instead, and two panes that behave completely differently
 * must not look nearly identical.
 *
 * COMPUTED from the two constants rather than written out by hand. The
 * hand-written version needed a _Static_assert to catch the ramp and the
 * rule drifting apart, which worked but made the threshold expensive to
 * TUNE - every trial move meant re-cutting sixteen entries by hand. It is
 * a number that wants trying at several values against a real board, so
 * the palette follows it instead of guarding it. */
/* Wood, whose variant is HOW MUCH IS LEFT TO BURN rather than a shade.
 *
 * Zero is unlit timber and is the only value a drawn or unburnt log ever
 * has; one to fifteen is a log on fire, counting down. So this is not a
 * ramp at all - it is one colour followed by a ramp, and the jump between
 * them is the point: a log is either alight or it is not, and that should
 * not be a judgement call.
 *
 * The burn ramp is ember's own, kept exactly: dying char through glowing
 * orange, deliberately redder and darker than fire's yellow-white so a
 * smouldering log reads differently from the flame licking off it.
 *
 * Wood loses its grain to this, the same trade stone made for temperature,
 * and gets it back the same way - see material_colours(), which speckles
 * unlit wood from the cell's position. */
/* Dirt's colour at a given MOISTURE. Wet soil really is darker than dry,
 * so the direction is not a choice - and it means a watered patch shows as
 * a dark stain rather than as a number only the plants can see. */
#define DIRT_DRY          0x9A7B52
#define DIRT_WET          0x3A2A18
#define DIRT_RGB(v)       LERP(DIRT_DRY, DIRT_WET, v)

/* One tone of soil: the dry and wet ends shifted together, so a bank shows
 * its strata whether it is parched or sodden. Tone 0 is the darker.
 *
 * Pushed apart from 3/2, on the report that the difference between two
 * poured banks was barely visible. It was 42 points of luminance; it is
 * now 60. Soil has one bit of tone against sand's twelve shades, so this
 * pair is the entire visual difference between one pour and the next, and
 * it has to carry alone what a whole band carries for sand.
 *
 * 4/3 is the LIMIT, and the limit is not taste. Wet soil has to read
 * darker than dry soil whichever tones are involved, or watering stops
 * being legible - a watered patch is meant to show as a dark stain. Push
 * to 5/4 and saturated light-tone soil comes out at luminance 100 against
 * bone-dry dark-tone soil at 85, so a well-watered bank looks drier than
 * a parched one. At 4/3 the two just clear each other, 86 against 93.
 * Seven points is not much headroom; anything that moves DIRT_DRY or
 * DIRT_WET needs to re-check it. */
#define SOIL_TONE_LO(rgb) LERP((rgb), 0x000000, 4)
#define SOIL_TONE_HI(rgb) LERP((rgb), 0xFFFFFF, 3)

#define SOIL_END(t, rgb)  ((t) ? SOIL_TONE_HI(rgb) : SOIL_TONE_LO(rgb))

/* Eight steps of wetness at one tone, which is half of dirt's palette
 * block; the other half is the same thing at the other tone. */
#define SOIL_RAMP(t)                                                                                                   \
    GFX_RGB(LERP(SOIL_END(t, DIRT_DRY), SOIL_END(t, DIRT_WET), 0)),                                                    \
        GFX_RGB(LERP(SOIL_END(t, DIRT_DRY), SOIL_END(t, DIRT_WET), 2)),                                                \
        GFX_RGB(LERP(SOIL_END(t, DIRT_DRY), SOIL_END(t, DIRT_WET), 4)),                                                \
        GFX_RGB(LERP(SOIL_END(t, DIRT_DRY), SOIL_END(t, DIRT_WET), 6)),                                                \
        GFX_RGB(LERP(SOIL_END(t, DIRT_DRY), SOIL_END(t, DIRT_WET), 9)),                                                \
        GFX_RGB(LERP(SOIL_END(t, DIRT_DRY), SOIL_END(t, DIRT_WET), 11)),                                               \
        GFX_RGB(LERP(SOIL_END(t, DIRT_DRY), SOIL_END(t, DIRT_WET), 13)),                                               \
        GFX_RGB(LERP(SOIL_END(t, DIRT_DRY), SOIL_END(t, DIRT_WET), 15))

#define WOOD_UNLIT   0x5A3D24
#define WOOD_CHAR    0x2A0A00
#define WOOD_GLOW    0xFF7A28

#define WOOD_BURN(i) GFX_RGB(LERP(WOOD_CHAR, WOOD_GLOW, ((i) - 1) * 15 / 14))

#define WOOD_SHADES                                                                                                    \
    GFX_RGB(WOOD_UNLIT), WOOD_BURN(1), WOOD_BURN(2), WOOD_BURN(3), WOOD_BURN(4), WOOD_BURN(5), WOOD_BURN(6),           \
        WOOD_BURN(7), WOOD_BURN(8), WOOD_BURN(9), WOOD_BURN(10), WOOD_BURN(11), WOOD_BURN(12), WOOD_BURN(13),          \
        WOOD_BURN(14), WOOD_BURN(15)

/* Stone's ramp, built the same way glass's is and meaning the same things
 * at the same levels - see the glass block below for why the splits are
 * computed from the constants rather than written out.
 *
 * Stone LOSES its random shade to this. That shade was a per-cell texture
 * that made a wall look like rock rather than a flat block, and it is a
 * real thing to give up; the dither in material_dither() puts a texture
 * back at the pixel level, which is not the same speckle but is not
 * nothing either. What is gained is that a hot wall is visibly hot, which
 * a random grey could never show. */
#define STONE_FROST   0xCEDCE8
#define STONE_AMBIENT 0x5F6673
#define STONE_NEUTRAL 0x8A7466
#define STONE_GLOW    0x9E3A18
#define STONE_MOLTEN  0xE8752A

#define STONE_COOL(v) LERP(STONE_FROST, STONE_AMBIENT, ((v) * 15) / (SAND_AMBIENT_HEAT > 0 ? SAND_AMBIENT_HEAT : 1))
#define STONE_WARM(v)                                                                                                  \
    LERP(STONE_AMBIENT, STONE_NEUTRAL,                                                                                 \
         (((v) - SAND_AMBIENT_HEAT) * 15)                                                                              \
             / (SAND_SHOCK_HEAT > SAND_AMBIENT_HEAT ? SAND_SHOCK_HEAT - SAND_AMBIENT_HEAT : 1))
#define STONE_HOT(v)                                                                                                   \
    LERP(STONE_GLOW, STONE_MOLTEN,                                                                                     \
         (((v) - SAND_SHOCK_HEAT) * 15)                                                                                \
             / (SAND_SHOCK_HEAT < MATERIAL_VARIANTS - 1 ? MATERIAL_VARIANTS - 1 - SAND_SHOCK_HEAT : 1))

#define STONE_RGB(v) ((v) <= SAND_AMBIENT_HEAT ? STONE_COOL(v) : (v) < SAND_SHOCK_HEAT ? STONE_WARM(v) : STONE_HOT(v))

#define STONE_AT(v)  GFX_RGB(STONE_RGB(v))

#define STONE_SHADES                                                                                                   \
    STONE_AT(0), STONE_AT(1), STONE_AT(2), STONE_AT(3), STONE_AT(4), STONE_AT(5), STONE_AT(6), STONE_AT(7),            \
        STONE_AT(8), STONE_AT(9), STONE_AT(10), STONE_AT(11), STONE_AT(12), STONE_AT(13), STONE_AT(14), STONE_AT(15)

#define GLASS_FROST   0xD6EEF8
#define GLASS_AMBIENT 0x2E6B85
#define GLASS_NEUTRAL 0x7E8E86
#define GLASS_GLOW    0xC8701E
#define GLASS_MOLTEN  0xFFD873

/* The three segments, each mapped onto 0..15 for LERP. Every branch has to
 * compute without dividing by zero even where it is not selected, hence the
 * guards on the denominators. */
#define GLASS_COOL(v) LERP(GLASS_FROST, GLASS_AMBIENT, ((v) * 15) / (SAND_AMBIENT_HEAT > 0 ? SAND_AMBIENT_HEAT : 1))
#define GLASS_WARM(v)                                                                                                  \
    LERP(GLASS_AMBIENT, GLASS_NEUTRAL,                                                                                 \
         (((v) - SAND_AMBIENT_HEAT) * 15)                                                                              \
             / (SAND_SHOCK_HEAT > SAND_AMBIENT_HEAT ? SAND_SHOCK_HEAT - SAND_AMBIENT_HEAT : 1))
#define GLASS_HOT(v)                                                                                                   \
    LERP(GLASS_GLOW, GLASS_MOLTEN,                                                                                     \
         (((v) - SAND_SHOCK_HEAT) * 15)                                                                                \
             / (SAND_SHOCK_HEAT < MATERIAL_VARIANTS - 1 ? MATERIAL_VARIANTS - 1 - SAND_SHOCK_HEAT : 1))

#define GLASS_AT(v)                                                                                                    \
    GFX_RGB((v) <= SAND_AMBIENT_HEAT ? GLASS_COOL(v) : (v) < SAND_SHOCK_HEAT ? GLASS_WARM(v) : GLASS_HOT(v))

#define GLASS_SHADES                                                                                                   \
    GLASS_AT(0), GLASS_AT(1), GLASS_AT(2), GLASS_AT(3), GLASS_AT(4), GLASS_AT(5), GLASS_AT(6), GLASS_AT(7),            \
        GLASS_AT(8), GLASS_AT(9), GLASS_AT(10), GLASS_AT(11), GLASS_AT(12), GLASS_AT(13), GLASS_AT(14), GLASS_AT(15)

/* The ramp is computed, so the two levels only need to be sane: room
 * temperature strictly inside the range with the shock point above it and
 * below the top. */
_Static_assert(SAND_AMBIENT_HEAT > 0 && SAND_AMBIENT_HEAT < SAND_SHOCK_HEAT && SAND_SHOCK_HEAT < MATERIAL_VARIANTS - 1,
               "glass needs room below ambient for frost, room above the "
               "shock point to keep climbing, and ambient strictly between");
/* Sand's two bands. Twelve steps then four, so both are spread across the
 * full 0-15 interpolation regardless of how many entries they have. */
#define SAND_DUNE 0xB07430
#define SAND_PALE 0xF2CE90
#define CULLET_LO 0xB9D2CC
#define CULLET_HI 0xF0FAF6

#define SAND_DUNE_RAMP                                                                                                 \
    GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 0)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 1)),                                    \
        GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 2)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 4)),                                \
        GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 5)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 6)),                                \
        GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 8)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 9)),                                \
        GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 10)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 12)),                              \
        GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 13)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 15))

#define SAND_CULLET_RAMP                                                                                               \
    GFX_RGB(LERP(CULLET_LO, CULLET_HI, 0)), GFX_RGB(LERP(CULLET_LO, CULLET_HI, 5)),                                    \
        GFX_RGB(LERP(CULLET_LO, CULLET_HI, 10)), GFX_RGB(LERP(CULLET_LO, CULLET_HI, 15))

#define SHADES(lo, hi)                                                                                                 \
    GFX_RGB(LERP(lo, hi, 0)), GFX_RGB(LERP(lo, hi, 1)), GFX_RGB(LERP(lo, hi, 2)), GFX_RGB(LERP(lo, hi, 3)),            \
        GFX_RGB(LERP(lo, hi, 4)), GFX_RGB(LERP(lo, hi, 5)), GFX_RGB(LERP(lo, hi, 6)), GFX_RGB(LERP(lo, hi, 7)),        \
        GFX_RGB(LERP(lo, hi, 8)), GFX_RGB(LERP(lo, hi, 9)), GFX_RGB(LERP(lo, hi, 10)), GFX_RGB(LERP(lo, hi, 11)),      \
        GFX_RGB(LERP(lo, hi, 12)), GFX_RGB(LERP(lo, hi, 13)), GFX_RGB(LERP(lo, hi, 14)), GFX_RGB(LERP(lo, hi, 15))

/* Sixteen entries for an unused material id, so the table is a full 256 and a
 * corrupt cell byte can only ever index a colour, never run off the end. */
#define UNUSED SHADES(0xFF00FF, 0xFF00FF)

/* THE source of colour. The material table deliberately carries none, so there
 * is one place to change and none to forget. Rows are in material_id_t order. */
static const gfx_color_t palette[256] = {
    [MAT_EMPTY * MATERIAL_VARIANTS] = SHADES(0x0A0C14, 0x0A0C14), /* empty - the background */
    [MAT_SAND * MATERIAL_VARIANTS] =
        /* sand - twelve DUNE shades and then four of CULLET, sand that used to
     * be glass (see SAND_CULLET_BASE). The cullet band deliberately leaves
     * the ramp rather than extending it: a paler warm tan is still tan,
     * and what says "this was a window" is the cool desaturated cast, not
     * the brightness. It is pulled towards frosted glass's own colour, so
     * a pane and its wreckage are recognisably the same substance. */
    SAND_DUNE_RAMP,
    SAND_CULLET_RAMP,
    [MAT_WATER * MATERIAL_VARIANTS] = SHADES(0x77C4E8, 0x14406F), /* water - shallow is pale, deep is dark */
    [MAT_STONE * MATERIAL_VARIANTS] = STONE_SHADES,               /* stone - a TEMPERATURE scale now, not a
                                    * shade ramp: same levels and the same
                                    * meanings as glass, so one wall reads
                                    * like the other */
    [MAT_GAS * MATERIAL_VARIANTS] = SHADES(0x445544, 0xC8E8B8),   /* gas   */
    [MAT_FIRE * MATERIAL_VARIANTS] = SHADES(0x400A00, 0xFFE060),  /* fire  - dying ember is dark, freshly
                                    * lit is bright yellow-white; variant
                                    * is life remaining, same trick gas
                                    * already uses */
    [MAT_WOOD * MATERIAL_VARIANTS] = WOOD_SHADES,                 /* wood - variant 0 is UNLIT and every
                                    * other value is how much is left to
                                    * burn, so this ramp is one colour of
                                    * timber followed by a burn ramp. See
                                    * WOOD_SHADES above */
    [MAT_STEAM * MATERIAL_VARIANTS] = SHADES(0x6E8496, 0xF2FAFF), /* steam - variant is life remaining, so
                                    * a dying wisp is a cool blue-grey and
                                    * a fresh one is almost white; same
                                    * trick fire and gas already use.
                                    * Deliberately COOL and BRIGHT, against
                                    * smoke's warm and dim just below: the
                                    * pair has to be told apart at a
                                    * glance, in motion, at two screen
                                    * pixels per cell, which is the entire
                                    * reason they are two materials rather
                                    * than one (see MAT_SMOKE's own row in
                                    * the material table above). */
    [MAT_SMOKE * MATERIAL_VARIANTS] = SHADES(0x2A2622, 0x857A6E), /* smoke - dying wisp is near-black soot,
                                    * fresh is a warm mid grey-brown. Warm
                                    * rather than neutral so it reads as
                                    * soot off a fire, not fog.
                                    *
                                    * The bright end is deliberately held
                                    * DOWN, and the exact figures are
                                    * measured rather than eyeballed
                                    * (suite_sand.c pins both):
                                    *
                                    *   - at equal life, steam is at least
                                    *     89 luminance brighter than smoke,
                                    *     across all sixteen variants;
                                    *   - the FRESHEST smoke (122) is still
                                    *     dimmer than the most nearly-dead
                                    *     steam (132), so the two ranges do
                                    *     not overlap at all - a puff can
                                    *     never be ambiguous, whatever
                                    *     stage of its life it is caught at.
                                    *
                                    * That second property is the fiddly
                                    * one and the first draft of this
                                    * palette did not have it: 0x9A8F84 at
                                    * the bright end put fresh smoke at 144
                                    * against dying steam's 132, which
                                    * overlapped. Being able to tell these
                                    * two apart at a glance is the entire
                                    * reason they are separate materials,
                                    * so it is worth a test rather than a
                                    * good intention. */
    [MAT_OIL * MATERIAL_VARIANTS] = SHADES(0x6E5A22, 0x14100A),   /* oil   - a liquid's variant is FILL
                                    * LEVEL, not life, so this runs the
                                    * same way water's does: a thin film
                                    * is a murky olive and a deep pool is
                                    * nearly black. Dark and warm against
                                    * water's pale blue, so a slick
                                    * floating on water is unmistakable -
                                    * which is the whole point of giving
                                    * oil a density below water's */
    [MAT_LAVA * MATERIAL_VARIANTS] = SHADES(0xFFC24A, 0x8A1400),  /* lava  - fill level again, and
                                    * deliberately INVERTED against
                                    * fire's own ramp: a thin skim is
                                    * bright yellow and a deep pool is
                                    * dark red, so depth reads as
                                    * cooling crust rather than as more
                                    * heat. Keeps a lava pool visually
                                    * distinct from the flames it
                                    * flares */
    [MAT_ACID * MATERIAL_VARIANTS] = SHADES(0xEAFF3C, 0x2E6B0A),  /* acid  - a liquid's variant is FILL
                                    * LEVEL, so this runs the way water's
                                    * does: a thin film is a vivid lime and
                                    * a deep pool is dark olive. Saturated
                                    * and yellow-leaning on purpose, to
                                    * keep it clear of gas's pale, washed
                                    * green - the two are never adjacent in
                                    * the density ladder but they are
                                    * adjacent on screen the moment
                                    * something fizzes */
    [MAT_GLASS * MATERIAL_VARIANTS] = GLASS_SHADES,               /* glass - NOT a shade ramp. Glass is the
                                    * one material whose variant is HEAT
                                    * (material.h's top comment), so this
                                    * ramp is a temperature scale and a
                                    * heating pane visibly glows along it.
                                    * That is most of the argument for
                                    * spending the nibble this way: the
                                    * palette already indexes it, so heat
                                    * became visible for no rendering work
                                    * at all.
                                    *
                                    * Cool teal at rest through to
                                    * 0xFFC24A, which is not a chosen
                                    * colour but lava's own brightest one
                                    * - so a pane about to melt is already
                                    * exactly the colour of what it turns
                                    * into, and the transformation lands
                                    * without a visible seam */
    [MAT_DIRT * MATERIAL_VARIANTS] =
        /* dirt - TWO wetness ramps, one per carried tone, because the variant
     * is a tone in the top bit and moisture in the low three (see
     * SOIL_MOISTURE_BITS). Dry dusty tan at 0 through to dark damp earth
     * at 7, twice. Wet soil really is darker than dry, so the direction is
     * not a choice - and it means a watered patch reads as a dark stain
     * rather than being a number only the plants can see.
     *
     * The two tones are what the grain used to be, moved from the renderer
     * into the cell. They are a little further apart than stone's speckle:
     * soil is the least uniform thing on the board, and the variation is
     * most of what says so. */
    SOIL_RAMP(0),
    SOIL_RAMP(1),
    [MAT_SNOW * MATERIAL_VARIANTS] = SHADES(0xC6D8E4, 0xFFFFFF), /* snow  - a powder, so a shade ramp
                                    * again, and a narrow one: cold blue
                                    * white to plain white. Deliberately
                                    * the palest thing on the board, since
                                    * it has to read as COLD at a glance
                                    * for thermal shock to explain itself */
    /* THE EXTENDED RANGE, one entry each rather than a shade ramp: an
     * extended material has no variant to ramp over, because the low
     * nibble is its identity. Sixteen materials, sixteen colours, and the
     * palette needed no change to allow it - it was already indexed by the
     * whole cell byte. */
    /* The extended range. NAMED rather than counted, unlike every other
     * block here: this one is a single entry per material instead of a run
     * of sixteen, so a miscount does not shift a whole block somewhere
     * obvious - it silently swaps two materials' colours. Spelling the
     * index out means a duplicate is a build error (-Werror=override-init)
     * rather than a surprise on the panel.
     *
     * The magenta tail is the padding for slots nobody has claimed, and it
     * is load-bearing: test_every_material_has_a_palette_block() asserts
     * all sixteen are non-zero, because zero renders BLACK and black looks
     * like a styling choice rather than a bug. It has caught exactly that
     * twice. */
    [MAT_EXTENDED * MATERIAL_VARIANTS + MATX_ICE] = GFX_RGB(0xB6E4F2),   /* ice - paler and bluer than snow's
                                    * white, and flat rather than speckled:
                                    * a block of it should read as solid
                                    * and cold, where snow reads as loose */
    [MAT_EXTENDED * MATERIAL_VARIANTS + MATX_PLANT] = GFX_RGB(0x55672D), /* plant - OLIVE, pulled most of the
                                    * way to wood's brown: a stem is
                                    * timber that has not arrived yet,
                                    * and every one of these cells is on
                                    * its way to being some. It used to
                                    * be a vivid green, which read as new
                                    * growth all over the trunk and made
                                    * the whole tree glow. The leaves
                                    * keep the green - they are the part
                                    * meant to catch the eye */
    [MAT_EXTENDED * MATERIAL_VARIANTS + MATX_LEAF] = GFX_RGB(0x69B03A),  /* leaf - the one green left in a tree
                                    * now that the stem is olive, and the
                                    * only part meant to catch the eye.
                                    * only part meant to catch the eye */
    [MAT_EXTENDED * MATERIAL_VARIANTS + MATX_METAL] = GFX_RGB(0x7C8794), /* metal - a cool blue-grey, brighter and
                                    * cooler than ambient stone (STONE_AMBIENT
                                    * 0x5F6673) so a wall of it separates from
                                    * a stone one at two screen pixels per
                                    * cell, and greyer than ice's 0xB6E4F2 so
                                    * the two do not merge either. This is a
                                    * GUESS, not a measurement - it wants eyes
                                    * on the panel, same as every other
                                    * starting-point constant in this table
                                    * (see docs/Sand/Metal-Smelting-Plan.md). */
    [MAT_EXTENDED * MATERIAL_VARIANTS + MATX_METAL + 1] = GFX_RGB(0xFF00FF),
    GFX_RGB(0xFF00FF),
    GFX_RGB(0xFF00FF),
    GFX_RGB(0xFF00FF),
    GFX_RGB(0xFF00FF),
    GFX_RGB(0xFF00FF),
    GFX_RGB(0xFF00FF),
    GFX_RGB(0xFF00FF),
    GFX_RGB(0xFF00FF),
    GFX_RGB(0xFF00FF),
    GFX_RGB(0xFF00FF),
    GFX_RGB(0xFF00FF),
};

/* Glass's SECOND colour: the same temperature, mixed halfway to the
 * background.
 *
 * Painted on alternate pixels inside each cell's block it reads as a woven
 * or frosted pane rather than a solid slab - which is most of what tells
 * glass apart from stone at a glance, since the two are identical in the
 * density ladder and behave identically to everything except acid.
 *
 * Mixed toward the BACKGROUND specifically, not simply darkened, because
 * what glass wants to look like is see-through. Half strength is the whole
 * effect: at cell size 2 a block is four pixels, so a checker is two of
 * each and any subtler mix would round away.
 *
 * Only glass has one. Everything else dithers against itself, which is the
 * same as not dithering - see material_dither() and paint_row_n(). */
#define GLASS_DIM(v)      GFX_RGB(GLASS_LINE(GLASS_RGB(v)))

/* The same ramps pulled two thirds of the way back to their own ambient
 * colour, used wherever a cell touches empty space. Ten of fifteen, so an
 * outline still shifts with heat - just a third as far as the body does. */
#define GLASS_RGB(v)      ((v) <= SAND_AMBIENT_HEAT ? GLASS_COOL(v) : (v) < SAND_SHOCK_HEAT ? GLASS_WARM(v) : GLASS_HOT(v))

#define GLASS_EDGE_RGB(v) LERP(GLASS_RGB(v), GLASS_RGB(SAND_AMBIENT_HEAT), 10)
#define STONE_EDGE_RGB(v) LERP(STONE_RGB(v), STONE_RGB(SAND_AMBIENT_HEAT), 10)

/* The lines and their crossings are LIGHTER than the pane, not darker.
 * They were a mix toward the background, which is what you would do for
 * something see-through and which came out as very nearly no pattern at
 * all - a dark line on a dark pane is invisible. What glass actually shows
 * is light caught on it, so the lines lift toward white and the crossings
 * go most of the way there. That is the shine. */
#define GLASS_LINE(rgb)   LERP((rgb), 0xFFFFFF, 4)
#define GLASS_SHINE(rgb)  LERP((rgb), 0xFFFFFF, 11)

#define GLASS_EDGE_DIM(v) GFX_RGB(GLASS_LINE(GLASS_EDGE_RGB(v)))

static const gfx_color_t glass_edge_dither[MATERIAL_VARIANTS] = {
    GLASS_EDGE_DIM(0),  GLASS_EDGE_DIM(1),  GLASS_EDGE_DIM(2),  GLASS_EDGE_DIM(3),
    GLASS_EDGE_DIM(4),  GLASS_EDGE_DIM(5),  GLASS_EDGE_DIM(6),  GLASS_EDGE_DIM(7),
    GLASS_EDGE_DIM(8),  GLASS_EDGE_DIM(9),  GLASS_EDGE_DIM(10), GLASS_EDGE_DIM(11),
    GLASS_EDGE_DIM(12), GLASS_EDGE_DIM(13), GLASS_EDGE_DIM(14), GLASS_EDGE_DIM(15),
};

#define GLASS_EDGE_SHINE(v) GFX_RGB(GLASS_SHINE(GLASS_EDGE_RGB(v)))
#define GLASS_AT_SHINE(v)   GFX_RGB(GLASS_SHINE(GLASS_RGB(v)))

static const gfx_color_t glass_edge_shine[MATERIAL_VARIANTS] = {
    GLASS_EDGE_SHINE(0),  GLASS_EDGE_SHINE(1),  GLASS_EDGE_SHINE(2),  GLASS_EDGE_SHINE(3),
    GLASS_EDGE_SHINE(4),  GLASS_EDGE_SHINE(5),  GLASS_EDGE_SHINE(6),  GLASS_EDGE_SHINE(7),
    GLASS_EDGE_SHINE(8),  GLASS_EDGE_SHINE(9),  GLASS_EDGE_SHINE(10), GLASS_EDGE_SHINE(11),
    GLASS_EDGE_SHINE(12), GLASS_EDGE_SHINE(13), GLASS_EDGE_SHINE(14), GLASS_EDGE_SHINE(15),
};

static const gfx_color_t glass_shine[MATERIAL_VARIANTS] = {
    GLASS_AT_SHINE(0),  GLASS_AT_SHINE(1),  GLASS_AT_SHINE(2),  GLASS_AT_SHINE(3),
    GLASS_AT_SHINE(4),  GLASS_AT_SHINE(5),  GLASS_AT_SHINE(6),  GLASS_AT_SHINE(7),
    GLASS_AT_SHINE(8),  GLASS_AT_SHINE(9),  GLASS_AT_SHINE(10), GLASS_AT_SHINE(11),
    GLASS_AT_SHINE(12), GLASS_AT_SHINE(13), GLASS_AT_SHINE(14), GLASS_AT_SHINE(15),
};

/* A per-cell wobble in the PANE, the same trick stone's speckle uses and
 * deliberately much quieter: a twentieth either way against stone's fifth.
 * Stone is rock and wants visible grain; glass is smooth and wants only
 * enough variation that a wall of it stops looking like one flat fill.
 *
 * The lines and the shine are left uniform. They are light landing on the
 * surface rather than the surface itself, and letting them wobble per cell
 * makes a highlight look chewed rather than reflective. */
#define GLASS_GRAIN(rgb, k) GFX_RGB(LERP(LERP((rgb), 0x000000, 1), LERP((rgb), 0xFFFFFF, 1), (k) * 5))

#define GLASS_BODY_ROW(v)                                                                                              \
    {GLASS_GRAIN(GLASS_RGB(v), 0), GLASS_GRAIN(GLASS_RGB(v), 1), GLASS_GRAIN(GLASS_RGB(v), 2),                         \
     GLASS_GRAIN(GLASS_RGB(v), 3)}

#define GLASS_EDGE_BODY_ROW(v)                                                                                         \
    {GLASS_GRAIN(GLASS_EDGE_RGB(v), 0), GLASS_GRAIN(GLASS_EDGE_RGB(v), 1), GLASS_GRAIN(GLASS_EDGE_RGB(v), 2),          \
     GLASS_GRAIN(GLASS_EDGE_RGB(v), 3)}

static const gfx_color_t glass_body[MATERIAL_VARIANTS][4] = {
    GLASS_BODY_ROW(0),  GLASS_BODY_ROW(1),  GLASS_BODY_ROW(2),  GLASS_BODY_ROW(3),
    GLASS_BODY_ROW(4),  GLASS_BODY_ROW(5),  GLASS_BODY_ROW(6),  GLASS_BODY_ROW(7),
    GLASS_BODY_ROW(8),  GLASS_BODY_ROW(9),  GLASS_BODY_ROW(10), GLASS_BODY_ROW(11),
    GLASS_BODY_ROW(12), GLASS_BODY_ROW(13), GLASS_BODY_ROW(14), GLASS_BODY_ROW(15),
};

static const gfx_color_t glass_edge_body[MATERIAL_VARIANTS][4] = {
    GLASS_EDGE_BODY_ROW(0),  GLASS_EDGE_BODY_ROW(1),  GLASS_EDGE_BODY_ROW(2),  GLASS_EDGE_BODY_ROW(3),
    GLASS_EDGE_BODY_ROW(4),  GLASS_EDGE_BODY_ROW(5),  GLASS_EDGE_BODY_ROW(6),  GLASS_EDGE_BODY_ROW(7),
    GLASS_EDGE_BODY_ROW(8),  GLASS_EDGE_BODY_ROW(9),  GLASS_EDGE_BODY_ROW(10), GLASS_EDGE_BODY_ROW(11),
    GLASS_EDGE_BODY_ROW(12), GLASS_EDGE_BODY_ROW(13), GLASS_EDGE_BODY_ROW(14), GLASS_EDGE_BODY_ROW(15),
};

static const gfx_color_t glass_dither[MATERIAL_VARIANTS] = {
    GLASS_DIM(0),  GLASS_DIM(1),  GLASS_DIM(2),  GLASS_DIM(3),  GLASS_DIM(4),  GLASS_DIM(5),
    GLASS_DIM(6),  GLASS_DIM(7),  GLASS_DIM(8),  GLASS_DIM(9),  GLASS_DIM(10), GLASS_DIM(11),
    GLASS_DIM(12), GLASS_DIM(13), GLASS_DIM(14), GLASS_DIM(15),
};

/* Stone's SPECKLE: eight shades of each temperature, picked per cell from
 * the cell's own position rather than from its variant.
 *
 * Spread BOTH WAYS around the temperature colour, not just downward. The
 * first version only darkened, which made every wall sit below the grey it
 * used to average at and read as a different, murkier material. The old
 * shade ramp ran 0x4A4F5A to 0x767D8C with the resting colour near its
 * middle, so a fifth toward black and a fifth toward white from ambient
 * lands back on very nearly those two endpoints.
 *
 * Stone used to carry a random shade in its variant and a wall looked like
 * rock because of it. Spending the variant on temperature took that away
 * and left a flat grey slab. It does not have to: the shade never needed
 * to be stored, only to be STABLE - the same cell showing the same speckle
 * every frame - and a position gives that for free while the variant goes
 * on meaning heat.
 *
 * Four levels rather than the sixteen the old shade ramp had. The old one
 * spanned the whole grey range because grey was all it had to say; this
 * one has to leave the temperature legible underneath it, so it is a
 * texture on top of a colour rather than the colour itself. */
#define STONE_DARK(rgb)     LERP((rgb), 0x000000, 3)
#define STONE_LIGHT(rgb)    LERP((rgb), 0xFFFFFF, 3)

#define STONE_GRAIN(rgb, k) GFX_RGB(LERP(STONE_DARK(rgb), STONE_LIGHT(rgb), (k) * 15 / 7))

#define STONE_SPECKLE(v, k) STONE_GRAIN(STONE_RGB(v), k)

#define STONE_SPECKLE_ROW(v)                                                                                           \
    {STONE_SPECKLE(v, 0), STONE_SPECKLE(v, 1), STONE_SPECKLE(v, 2), STONE_SPECKLE(v, 3),                               \
     STONE_SPECKLE(v, 4), STONE_SPECKLE(v, 5), STONE_SPECKLE(v, 6), STONE_SPECKLE(v, 7)}

static const gfx_color_t stone_speckle[MATERIAL_VARIANTS][8] = {
    STONE_SPECKLE_ROW(0),  STONE_SPECKLE_ROW(1),  STONE_SPECKLE_ROW(2),  STONE_SPECKLE_ROW(3),
    STONE_SPECKLE_ROW(4),  STONE_SPECKLE_ROW(5),  STONE_SPECKLE_ROW(6),  STONE_SPECKLE_ROW(7),
    STONE_SPECKLE_ROW(8),  STONE_SPECKLE_ROW(9),  STONE_SPECKLE_ROW(10), STONE_SPECKLE_ROW(11),
    STONE_SPECKLE_ROW(12), STONE_SPECKLE_ROW(13), STONE_SPECKLE_ROW(14), STONE_SPECKLE_ROW(15),
};

#define STONE_EDGE_SPECKLE(v, k) STONE_GRAIN(STONE_EDGE_RGB(v), k)

#define STONE_EDGE_ROW(v)                                                                                              \
    {STONE_EDGE_SPECKLE(v, 0), STONE_EDGE_SPECKLE(v, 1), STONE_EDGE_SPECKLE(v, 2), STONE_EDGE_SPECKLE(v, 3),           \
     STONE_EDGE_SPECKLE(v, 4), STONE_EDGE_SPECKLE(v, 5), STONE_EDGE_SPECKLE(v, 6), STONE_EDGE_SPECKLE(v, 7)}

/* Wood's grain, on the same footing as stone's speckle and for the same
 * reason: wood spent its shade on burn progress, so an unlit log was one
 * flat brown fill.
 *
 * Only UNLIT wood is speckled. A burning log is glowing, and a glow that
 * varies cell to cell reads as dirty rather than as fire - the same
 * reasoning that keeps glass's shine uniform while its pane is not. */
#define WOOD_GRAIN(k) GFX_RGB(LERP(LERP(WOOD_UNLIT, 0x000000, 3), LERP(WOOD_UNLIT, 0xFFFFFF, 2), (k) * 15 / 7))

/* Dirt has no grain table. It used to, hashed from screen position like
 * stone's and wood's, and that is only right for a material that never
 * moves: dirt falls and piles, so the texture stayed pinned to the screen
 * while the dirt slid under it - a repeating pattern the board scrolled
 * through rather than anything belonging to the soil. Its tone lives in
 * the cell now, and the palette draws it. */

static const gfx_color_t wood_grain[8] = {
    WOOD_GRAIN(0), WOOD_GRAIN(1), WOOD_GRAIN(2), WOOD_GRAIN(3),
    WOOD_GRAIN(4), WOOD_GRAIN(5), WOOD_GRAIN(6), WOOD_GRAIN(7),
};

/* The two extended materials that are worth speckling, and the only
 * source of variation they can have.
 *
 * An extended material's variant IS which one it is, so there is no shade
 * to carry - the position hash is all there is, exactly as for stone and
 * wood. That is the right tool here for the same reason it was wrong for
 * dirt: a wall of ice does not move, and a tree, once it has grown, does
 * not either. A falling seed shimmers for the second it is in the air,
 * which is a fair price for foliage that is not one flat block of green.
 *
 * Leaves get the wider range of the two. Foliage in life is a mess of
 * light and shade and half-dead leaves; ice is one substance, and its
 * variation is facets catching the light rather than any real difference
 * in colour. */
/* The stem is OLIVE, not green: about 55% of the way from the green it
 * used to be towards wood's unlit 0x5A3D24, which is what a shoot part
 * way to being bark actually looks like.
 *
 * Reported as trees reading too bright. The stem is the right thing to
 * move rather than the foliage, because of what a stem IS here: every
 * one of these cells is on its way to being wood, either by finishing
 * its run or by drying out and lignifying. Colouring it as timber that
 * has not arrived yet says that, where a green as vivid as the leaves
 * said the opposite - that the trunk was covered in new growth.
 *
 * Luminance, since brightness was the complaint: the band was 85 to 159
 * of 255 and is now 75 to 123, against wood's own 67. A stem now sits
 * beside the trunk instead of on top of it. The leaves are untouched and
 * are still the bright thing in a tree, which is correct - they are the
 * only part that is supposed to catch the eye. */
#define PLANT_DARK        0x495422
#define PLANT_LIGHT       0x778746

/* Foliage: lighter and yellower than the stem, and a wider spread than
 * either of the others. A crown is sunlit on one side and shaded on the
 * other, and half of what makes it read as a canopy rather than as more
 * stem is that it is not one flat green.
 *
 * All of what a tree's colour is, now that foliage is one material
 * again. A leaf on a tree is sheltered_by wood and can drink through
 * the trunk, and withering asks both of those before it does anything,
 * so a crown stays exactly this green until something takes it. An
 * ageing chain that turned it gold and brown was built, measured and
 * removed: it cost two of the sixteen extended slots and, being
 * reachable only through senescence, was barely visible when it ran. */
#define LEAF_DARK         0x468F26
#define LEAF_LIGHT        0x8CD24E

#define ICE_DARK          0x93C9DE
#define ICE_LIGHT         0xDEF5FD

/* Metal: a wall of it does not move any more than a wall of ice does, so
 * it gets the same treatment - the position hash is the only variation a
 * material with no variant of its own can have. A narrower spread than
 * ice's, closer to ice's own range than leaf's wide one: metal is a cast,
 * uniform substance, and its speckle is meant to read as light catching a
 * surface rather than as real variation in the material. */
#define METAL_DARK        0x7C8794
#define METAL_LIGHT       0xB9C4D2

#define GRAIN8(lo, hi, k) GFX_RGB(LERP((lo), (hi), (k) * 15 / 7))

#define GRAIN8_ROW(lo, hi)                                                                                             \
    {GRAIN8(lo, hi, 0), GRAIN8(lo, hi, 1), GRAIN8(lo, hi, 2), GRAIN8(lo, hi, 3),                                       \
     GRAIN8(lo, hi, 4), GRAIN8(lo, hi, 5), GRAIN8(lo, hi, 6), GRAIN8(lo, hi, 7)}

static const gfx_color_t plant_grain[8] = GRAIN8_ROW(PLANT_DARK, PLANT_LIGHT);
static const gfx_color_t ice_grain[8] = GRAIN8_ROW(ICE_DARK, ICE_LIGHT);
static const gfx_color_t leaf_grain[8] = GRAIN8_ROW(LEAF_DARK, LEAF_LIGHT);
static const gfx_color_t metal_grain[8] = GRAIN8_ROW(METAL_DARK, METAL_LIGHT);

/* Metal's woven line and travelling shine - the same HATCHED mechanism
 * glass uses in paint_row_n(), which is generic to anything hatched and
 * not glass-specific (the diagonal grain, the crossings, the travelling
 * band all key off the pattern, never the material). What glass gets that
 * metal cannot is a per-variant ramp to shade these by: an extended
 * material's low nibble is spent naming WHICH one it is rather than
 * holding a variant (see the MAT_EXTENDED case below), so there is one
 * dither tone and one shine tone here, not sixteen.
 *
 * Lifted off METAL_LIGHT rather than off metal_grain's own per-cell
 * wobble, same reasoning as GLASS_LINE/GLASS_SHINE above: a highlight
 * that wobbled per cell would look chewed rather than reflective. Same
 * two weights as glass's, 4 and 11 of 15 - metal is meant to look
 * brushed and catching light exactly the way a pane does, just opaque. */
static const gfx_color_t metal_dither = GFX_RGB(LERP(METAL_LIGHT, 0xFFFFFF, 4));
static const gfx_color_t metal_shine = GFX_RGB(LERP(METAL_LIGHT, 0xFFFFFF, 11));

static const gfx_color_t stone_edge_speckle[MATERIAL_VARIANTS][8] = {
    STONE_EDGE_ROW(0),  STONE_EDGE_ROW(1),  STONE_EDGE_ROW(2),  STONE_EDGE_ROW(3),
    STONE_EDGE_ROW(4),  STONE_EDGE_ROW(5),  STONE_EDGE_ROW(6),  STONE_EDGE_ROW(7),
    STONE_EDGE_ROW(8),  STONE_EDGE_ROW(9),  STONE_EDGE_ROW(10), STONE_EDGE_ROW(11),
    STONE_EDGE_ROW(12), STONE_EDGE_ROW(13), STONE_EDGE_ROW(14), STONE_EDGE_ROW(15),
};

/*=============================================================================
 * A liquid rim's specular highlight.
 *
 * A liquid's own variant is a FILL LEVEL, and palette[] is indexed by the
 * whole cell byte, so fill level IS the shade - pale and thin, dark and
 * deep. That is right at a SURFACE, which is the only place fill level
 * means anything (see material_colours()'s own comment below for the
 * interior half of this story). But a flat fill ramp paints the top of a
 * pool and the underside of an overhang identically whenever they happen
 * to sit at the same fill level, and a real surface does not: the side
 * facing away from gravity catches the light, the side facing into it
 * does not.
 *
 * `liquid_spec[mask]` is that difference, as a shade SHIFT added to a rim
 * cell's own fill index before it is looked up in the palette - positive
 * darkens, negative brightens (liquid's ramp runs pale-to-dark as fill
 * rises, so brightening means moving DOWN the index - see
 * material_set_gravity() and material_colours() below, and mind the sign).
 *
 * Filled in ONCE PER FRAME by material_set_gravity(), not read per cell as
 * gravity - material_colours() runs per cell per painted row and is hot,
 * so all the trig below happens exactly once a frame and the per-cell cost
 * stays one array index, same as ever.
 *
 * Sized and indexed by MATERIAL_EDGE_MASK_COUNT - the number of values the
 * CARDINAL mask alone can take - and NOT by MATERIAL_VARIANTS, even though
 * both are 16 today. This table has never held a variant; it holds one
 * entry per CARDINAL edge mask, and gravity has no opinion about the
 * diagonal bits a water rim's foam reads, so there is nothing for this
 * table to say about them. The two constants agreeing was a coincidence
 * that used to double as the size, which is exactly the kind of thing that
 * silently stops being true the day either one changes for an unrelated
 * reason. */
static int8_t liquid_spec[MATERIAL_EDGE_MASK_COUNT];

/* How far liquid_spec's shift reaches, out of the sixteen levels a fill
 * ramp has. A look tuned by eye on the device, not derived from anything -
 * if the rim's highlight ever reads too strong or too faint, this is the
 * first number to move.
 *
 * 10, up from 6: reported as still too subtle even after the interior
 * gained its own depth gradient. Measured on water's ramp, this takes the
 * rim's up-face-to-down-face range from 49 luminance to 81. */
#define SPEC_STRENGTH 10

/* Rounds n/d to the nearest integer, for n of either sign and d > 0.
 *
 * Plain integer division truncates towards zero, which is fine for the
 * rest of this file's ramps (they only ever walk forward through a table)
 * but wrong here: a specular term that rounds -0.5 to 0 every time is a
 * highlight that is quietly weaker on one side than the other. */
static int
fx_round_div(int n, int d) {
    if (n >= 0) {
        return (n + d / 2) / d;
    }
    return -((-n + d / 2) / d);
}

/* Fills liquid_spec[] from this frame's gravity - see that table's own
 * comment for what it holds and why it exists at all.
 *
 * For each of the MATERIAL_EDGE_MASK_COUNT cardinal MATERIAL_EDGE_* masks,
 * the outward normal `n` is the sum of the unit vectors of whichever
 * cardinal sides are empty.
 * Because there are only two axes, `n`'s components collapse to the
 * three cases the caller was told to expect: no empty side at all, or an
 * opposite pair that cancels, gives (0, 0); exactly one empty side (or an
 * opposite pair plus one more) gives a single unit axis; two ADJACENT
 * empty sides give a diagonal of length sqrt(2). Nothing here needs a
 * general vector length for that reason - `norm_q8` below just picks
 * between "already unit length" and "divide by sqrt(2)" from how many of
 * n's two components are nonzero.
 *
 * The specular value is the normalised dot product of `n` with MINUS
 * gravity: +1 when the empty side faces straight against gravity (the top
 * of a pool), -1 when it faces straight along it (the underside of a
 * drip). Fixed point throughout, scaled by 256 the same way build_xflow()
 * in sand.c measures its own bias from gravity - see that function's own
 * comment for why im_len()'s ~4% approximation is fine for a quantity
 * nothing reads to better precision than "which way, roughly how much".
 *
 * NO LONGER also derives a depth/wave walk the way this function once did -
 * a liquid interior's `depth` and `wave` (material_colours()'s own comment
 * has the full account) are LOCAL now, walked fresh per cell against the
 * live grid by app_sand.c's paint_row_n(), not something gravity's
 * direction alone can work out ahead of time here. This function's only
 * remaining job is the specular table below, which is why it no longer
 * takes a grid size either - see material.h's own comment on this
 * function's declaration. */
void
material_set_gravity(int gx, int gy) {
    const int len = im_len(gx, gy);
    if (len == 0) {
        /* Free fall, or the board laid flat with nothing driving it: no
         * "up" to catch the light from, so no rim gets a highlight rather
         * than dividing by a length of zero. */
        for (unsigned m = 0; m < MATERIAL_EDGE_MASK_COUNT; m++) {
            liquid_spec[m] = 0;
        }
        return;
    }

    /* Unit vector of MINUS gravity, scaled by 256 - the direction a rim's
     * empty side has to face to catch the brightest highlight. */
    const int ux_q8 = (-gx * 256) / len;
    const int uy_q8 = (-gy * 256) / len;

    for (unsigned mask = 0; mask < MATERIAL_EDGE_MASK_COUNT; mask++) {
        const int nx = ((mask & MATERIAL_EDGE_RIGHT) ? 1 : 0) - ((mask & MATERIAL_EDGE_LEFT) ? 1 : 0);
        const int ny = ((mask & MATERIAL_EDGE_DOWN) ? 1 : 0) - ((mask & MATERIAL_EDGE_UP) ? 1 : 0);

        if (nx == 0 && ny == 0) {
            liquid_spec[mask] = 0;
            continue;
        }

        const int raw_q8 = nx * ux_q8 + ny * uy_q8;
        /* 181/256 is 1/sqrt(2), for the diagonal case; the axis-only case
         * is already unit length and needs no scaling at all. */
        const int norm_q8 = (nx != 0 && ny != 0) ? 181 : 256;
        const int spec_q8 = (raw_q8 * norm_q8) / 256; /* now in [-256,256] */

        /* NEGATED: the ramp runs bright-to-dark as fill rises, so a
         * positive specular term (facing away from gravity, wants to be
         * BRIGHTER) has to SUBTRACT from the index rather than add to it.
         * Getting this backwards inverts the whole effect - see
         * test_a_liquid_rim_catches_the_light_from_above in suite_sand.c,
         * which exists specifically to catch that mistake rather than
         * trust the arithmetic by eye. */
        liquid_spec[mask] = (int8_t)(-fx_round_div(spec_q8 * SPEC_STRENGTH, 256));
    }
}

/*=============================================================================
 * WATER'S FOAM: gathered at crevices, never on a flat run.
 *
 * A liquid rim already shades itself by which way its open side faces
 * against gravity - liquid_spec[] above. Foam is a second, independent
 * decoration on the same rim, and it answers a different question:
 * gravity says which side of a straight surface is bright, foam says
 * whether the surface is straight AT ALL.
 *
 * THE CURVATURE MEASURE. Count how many of a rim cell's EIGHT neighbours
 * are empty - all eight bits of `mask`, cardinal and diagonal alike, off-
 * grid counted as NOT empty exactly the way paint_row_n() already treats
 * the board edge as solid wall. A perfectly straight edge - the top of a
 * flat pool, say - has exactly 3 empty neighbours: the three cells on the
 * open side. Fewer than 3 is a cell tucked into a CONCAVE notch; more than
 * 3 is a cell sticking out of a CONVEX bump. Either way is curvature, so
 * `curvature = abs(empty_count - 3)` is 0 on a straight run and grows in
 * both directions away from it.
 *
 * NO MOTION FLAG, AND THIS IS THE WHOLE POINT. It would be easy to reach
 * for "is the water moving" as a second gate, and it would be redundant:
 * a calm pool's rim is smooth by construction, so curvature alone already
 * reads as almost entirely flat, and a sloshing one is jagged along its
 * whole length, so curvature alone already reads as almost entirely not.
 * Measured on real sloshing water, 48x32, water only - the share of rim
 * cells that are NOT flat (curvature > 0):
 *
 *     still, flat pool          4%
 *     settled at 40 degrees    34%
 *     8 steps into 40 degrees  66%
 *     2 steps into 40 degrees  76%
 *     2 steps into 75 degrees  94%
 *
 * 4% against 94% is curvature already doing the job a motion flag would
 * have been added to do. Adding one anyway would be a second mechanism
 * competing with the first to answer a question the first already
 * answers - do not add it back in; if foam ever needs tuning, the
 * threshold table below is where that tuning belongs.
 *
 * ANIMATING THE DITHER. Curvature decides WHERE foam gathers; the hash
 * decided WHICH of those cells show it, and until now that hash was the
 * stable per-cell grain hash every other speckled material already uses -
 * the same value, every frame, for as long as the shape stays put. That
 * read as a texture painted onto the water rather than as foam moving on
 * it, reported in exactly those words: it "reads as part of the rim".
 *
 * `foam_phase` - set once a frame by material_set_foam_phase(), see that
 * function's own comment in material.h for why it is a call separate from
 * material_set_gravity() - gives the dither a second input that changes
 * every frame even when the shape does not, so a cell's on/off answer keeps
 * changing while the curvature that gates it stays fixed. It is ADDED to
 * the hash (scaled by 0x9E37u first - see the mixing site below for why
 * that constant specifically), and the reason is that addition ROTATES the
 * low three bits' window of foaming values by one place every time the
 * phase advances, so two CONSECUTIVE phases can never produce the same
 * foam set: whatever was just inside the window has moved, and whatever
 * was just outside has taken its place.
 *
 * XOR WAS TRIED FIRST AND IS WRONG, and worth recording exactly why so
 * nobody "simplifies" this back. The reasoning at the time was that XOR
 * would decorrelate a cell's answer from its neighbours' better than a
 * plain shift would - which is backwards. water_foam_threshold's windows
 * are power-of-two aligned (0, 2, 4, 6 out of 8), and XORing the low three
 * bits by a fixed value maps an aligned window either ONTO ITSELF or onto
 * a DIFFERENT aligned window, depending only on the phase's own low bits -
 * so roughly half of all phase steps leave the foaming set completely
 * unchanged. Enumerated across all eight phases, mixed with XOR:
 *
 *     threshold 2 (curvature 1)   4 of 8 consecutive pairs identical
 *     threshold 4 (curvature 2)   6 of 8 consecutive pairs identical
 *     threshold 6 (curvature 3+)  4 of 8 consecutive pairs identical
 *
 * Threshold 4 - medium curvature, the commonest case on a real board - is
 * the worst of the three: the foam set changes on only 2 of every 8 phase
 * steps, which is most of a full second of FOAM_PHASE_MS ticks producing
 * nothing. Confirmed on a real sloshing scene, not just this table: phase
 * 1 to phase 2 changed exactly zero cells out of 635 foaming rim cells.
 * That is indistinguishable from the stable dither this change exists to
 * replace, most of the time - the opposite of the goal.
 *
 * Addition does not have this failure mode, and does not reintroduce the
 * unison the XOR attempt was trying (wrongly) to avoid either: at
 * threshold 2, for instance, only 2 of the 8 hash values foam at any given
 * phase, so the rim is never all-on or all-off regardless of which mixing
 * is used - that property was never XOR's to provide. See
 * test_foam_never_stalls_between_frames in suite_sand.c, which pins BOTH
 * halves: the foam set is never degenerate (all-on/all-off) at any single
 * phase, AND no two consecutive phases, across a full cycle of eight,
 * produce an identical set at any of the three thresholds.
 *
 * NOT FORCING DIRTY ROWS, AND THIS IS DELIBERATE. app_sand.c's
 * draw_dirty_rows() repaints only the rows something marked dirty, so an
 * animated foam cell sitting in an otherwise-settled row will not actually
 * be redrawn until that row changes for some other reason. Widening the
 * repaint to cover "any row that might have foam in it" would mean walking
 * the grid, or keeping a second row-tracking table the way row_has_shine[]
 * exists for glass's shine - real cost, paid every frame, for a cosmetic
 * dither. It is also unnecessary: foam only appears where curvature is
 * nonzero, and a rim's curvature is nonzero because the water is IN MOTION
 * (see the measurements above - a still pool is 4% non-flat, a sloshing one
 * is up to 94%), and motion is exactly what already marks a row dirty every
 * step. A settled pool's rim is flat, foams nowhere, and has nothing
 * animating to miss; a moving one is being repainted anyway. Do not go
 * looking for a way to force these rows dirty - there is nothing here that
 * needs it. */

/* FOAM's own colour - a side table, not a palette[] row, the same pattern
 * glass_shine and stone_speckle already use above: there is no spare slot
 * in palette[] for it, and a dither over an existing rim colour does not
 * need an indexed row of its own the way a fill level does.
 *
 * Brighter and whiter than water's own palest ramp entry (0x77C4E8, the
 * shallow end of the SHADES() run in palette[] above) - foam has to read
 * as something sitting ON the water, not merely as the palest water there
 * is. */
static const gfx_color_t water_foam = GFX_RGB(0xE8F6FF);

/* How far curvature is allowed to climb before it stops changing the
 * foam density any further - see water_foam_threshold below. Curvature
 * itself can reach 5 (an empty_count of 8, a cell exposed on every side),
 * but a corner poking into open air should look exactly as heavily foamed
 * as an ordinary jagged crevice one cell less exposed, not more so. */
#define WATER_FOAM_CURVATURE_MAX 3

/* Foam density at each curvature, expressed as a threshold against a mix
 * of `hash` and this frame's foam phase (see below): a cell foams when that
 * mix, masked to three bits, falls under the threshold for its curvature.
 * So this is a DITHER, not a fill - a "heavy" cell still shows bare rim on
 * 2 of its 8 possible values, and a "flat" one never foams at all, since
 * the masked mix can never be less than 0.
 *
 * A look tuned by eye, not measured - the first thing to move if foam
 * ever reads too sparse (raise these) or too busy (lower them). Kept as a
 * named table rather than a formula so tuning it is an edit to plain
 * numbers, not to arithmetic.
 *
 * RAISED from { 0, 2, 4, 6 }: reported as "the alternating is barely
 * visible", which had two causes. FOAM_BLOB_SHIFT (app_sand.c) already
 * covers one - each flip was a small area - by clumping foam into bigger
 * blocks; this covers the other, there simply was not much foam to begin
 * with. Curvature 0 stays 0 and MUST: a flat rim - the top of a still pool
 * - has to never foam at any hash or phase, whatever the other three
 * entries are, or foam stops meaning "the water is moving" and starts
 * meaning "the water exists" - see
 * test_a_flat_rim_still_never_foams in suite_sand.c, which pins exactly
 * that after this change. */
static const uint8_t water_foam_threshold[WATER_FOAM_CURVATURE_MAX + 1] = {
    0, /* curvature 0, flat   - no foam at all */
    3, /* curvature 1, light  - foams on 3 of 8 hash values */
    5, /* curvature 2, medium - foams on 5 of 8 */
    7, /* curvature 3+, heavy - foams on 7 of 8 */
};

/* THIS FRAME'S FOAM PHASE - see material_set_foam_phase() and its own
 * comment in material.h for what it means and why it is a call of its own
 * rather than folded into material_set_gravity(). Zero until the first
 * frame sets it, which paints exactly the stable dither foam had before
 * phase existed - a reasonable default for anything that reads
 * material_colours() before a frame ever runs (tests included). */
static unsigned foam_phase;

void
material_set_foam_phase(unsigned phase) {
    foam_phase = phase;
}

/* How many of `mask`'s bits are set - the same manual bit-count
 * suite_icons.c's popcount16() uses, kept here rather than shared because
 * the two operate on different widths for different reasons and a shared
 * helper would need to justify its own generality. No floating point, and
 * cheap enough for a path already gated to water rim cells only - see
 * paint_row_n() in app_sand.c for where that gate actually lives. */
static unsigned
material_popcount8(unsigned mask) {
    unsigned count = 0;
    for (unsigned bit = 0; bit < 8u; bit++) {
        count += (mask >> bit) & 1u;
    }
    return count;
}

/* How many of the sixteen fill-ramp steps a liquid interior's depth
 * gradient may brighten the shallowest cell by, relative to the body
 * colour at the deepest - see material_colours()'s own comment on the
 * liquid interior branch for why depth needs a cue here at all.
 *
 * A look knob, tuned against water's own ramp: index 15 (the body colour,
 * MASS_MAX) measures 54 luminance, 11 measures 85, 8 measures 111 - so 4
 * takes a full-height pool from about 54 at the bottom to about 85 at the
 * top, present without being garish. Move it if the gradient ever reads
 * too strong (lower it) or too subtle (raise it); it never touches the
 * rim, which keeps its own two terms (fill level and liquid_spec[]'s
 * specular) untouched by this constant entirely. */
#define DEPTH_RANGE          4

/* HOW MANY CELLS OF LOCAL DEPTH IT TAKES A LIQUID INTERIOR TO REACH FULL
 * DARKENING - the material's own body colour, with none of DEPTH_RANGE's
 * lightening left - clamped there for anything deeper, rather than needing
 * to approach local depth's own 255 clamp the way this divide used to.
 *
 * WATER USED TO GET SOMETHING DIFFERENT HERE: an animated sum-of-sines wave
 * table riding on top of this same local depth, and a haze BLEND toward a
 * pale fog colour in place of the plain shade-index shift every other
 * liquid used. Both are gone. The fog blend's own arithmetic (`depth_q`,
 * dividing by 255) was tuned for the OLD screen-position depth, which
 * legitimately spanned the full 0-255 range across the whole grid - local
 * depth resets to 0 at every puddle's own surface and rarely exceeds a few
 * dozen cells for any real pool in this app, so the fog blend sat pinned
 * near its palest end for essentially every visible pool, and water read as
 * a flat pale wash instead of depth-through-water: a genuine bug, not a
 * taste call. The wave bands had their own, separate problem: local depth
 * commits to a single dominant axis (see LOCAL DEPTH below) with a hard,
 * unsmoothed switch between vertical and horizontal, and the bands rode
 * straight over that seam with no blending between an axis-aligned reading
 * and a diagonal one, reading as rigid columns (or rows) of matching colour
 * rather than an organic gradient. Both mechanisms still exist, untouched,
 * on the water-wave-fog-depth-banked branch, for anyone who wants to
 * revisit that approach or reuse a piece of it - the wave-table technique,
 * the fog-blend arithmetic - for a different effect later.
 *
 * Water's interior now uses EXACTLY the mechanism oil, lava and acid always
 * did: a plain shade-index shift into the material's own ramp, darker with
 * depth, fed by local depth alone. The SAME `/255` divide that produced
 * water's fog bug also fed this plain shift for every liquid, just less
 * visibly - a shade-index nudge saturating late is a smaller, less
 * noticeable effect than an entire colour failing to darken - which is why
 * this constant fixes the scale for all four liquids at once, not water
 * alone.
 *
 * 24 was modelled against water's own ramp: it gives idx 11 (lum 85,
 * lightened) at the very surface, exactly as DEPTH_RANGE's own comment
 * above describes, and reaches idx 15 (lum 54, the full body colour) once
 * local depth hits this constant - comfortably inside the range any real
 * pool in this app actually reaches, rather than needing depth in the
 * hundreds the way the old /255 divide did.
 *
 * SHARED with sand_liquid.c as MATERIAL_LIQUID_DEPTH_BAND (material.h) -
 * that is the same 24, not a coincidence: sand_liquid.c uses it to bound how
 * far a newly-claimed surface cell can possibly move any cell's rendered
 * depth, and that has to be exactly this constant, or the two will silently
 * drift apart the day only one of them gets retuned. */
#define DEPTH_SATURATE_CELLS MATERIAL_LIQUID_DEPTH_BAND

material_pattern_t
material_colours(cell_t c, unsigned hash, unsigned mask, unsigned depth, gfx_color_t out[3]) {
    const uint8_t v = CELL_VARIANT(c);

    /* A LIQUID paints one of two ways depending on whether this cell is a
     * rim, and the split is not a cheat - it is what the variant actually
     * MEANS. A liquid cell is only ever partially filled at a surface or
     * in transit: mid-body, every cell wants to be full, and the only
     * reason one dips below MASS_MAX is the levelling rule redistributing
     * mass one step at a time (see material.h's own top comment on why the
     * fill level exists at all). That is a transient of the SOLVER, not a
     * claim that there is less water there - so painting an interior cell
     * at anything but the full body colour shows the player an artefact of
     * how the simulation works rather than anything true about the pool.
     *
     * It is also what fixes the comb. build_xflow()'s two-ray dither
     * (sand.c) settles neighbouring interior columns to different fills
     * while a pool is moving under tilt, one cell full and the next at
     * mid-ramp, over and over - measured two steps into a strong tilt as
     * alternating fills a whole 81 luminance apart, which reads on the
     * panel as hard lines through the water. The dither cannot be turned
     * off; it is what keeps a settled pool's surface reading the same
     * slope at every angle rather than one fixed shape (see build_xflow()'s
     * own comment for what THAT bug looked like). But the comb only ever
     * shows up mid-body, so an interior cell that always paints full water
     * makes it invisible without touching the simulation or the palette at
     * all.
     *
     * BUT A FLAT INTERIOR READS AS FLAT, which is exactly what was reported
     * once the comb stopped showing: a settled pool has NO fill variation
     * to shade with at all - measured 0 of 747 interior cells anything but
     * full at 40 degrees settled, 0 of 720 settled flat, and only 5% even 3
     * steps into a tilt - so undoing the flat interior was never on the
     * table; painting the FILL differently is painting almost nothing
     * differently. `depth` is a new cue rather than a rescue of the old
     * one: light attenuates with depth, so a deeper cell reads darker and a
     * shallower one reads lighter, shifting the index toward the BRIGHT end
     * of the ramp (see DEPTH_RANGE below) by up to DEPTH_RANGE steps as the
     * cell gets shallower, from the body colour at the very deepest point.
     *
     * THIS USED TO ALSO CARRY WAVE BANDS AND, FOR WATER SPECIFICALLY, A HAZE
     * BLEND in place of the plain shift below - both removed. See
     * DEPTH_SATURATE_CELLS's own comment just above this function for why:
     * the fog blend's range mismatch was a genuine bug (pinned near-pale for
     * any depth a real pool here reaches), and the wave bands rode straight
     * over local depth's own axis seam with no blending between an
     * axis-aligned reading and a diagonal one, reading as rigid columns
     * rather than an organic gradient. Water's interior now runs the exact
     * same shift every other liquid always has - no more "water is
     * special" fork in this branch.
     *
     * LOCAL DEPTH, NOT SCREEN POSITION. `depth` is 0 at this material's own
     * boundary - the neighbour one step toward the surface is not the same
     * material, whether that is a different liquid, empty space, or solid -
     * and climbs by one for each further cell into the body, clamped at
     * 255. That is a genuine per-puddle measurement, walked fresh from the
     * live grid by app_sand.c's paint_row_n() (see LOCAL DEPTH's own long
     * comment there for the full mechanism), not a screen-position gradient
     * the way an earlier version of this cue was: that version was reported
     * from the device as reading "almost like platinum" - a gradient swept
     * across the WHOLE SCREEN, blind to where the water actually was, reads
     * as a metallic sheen rather than depth through a medium - and as
     * wanting to "just follow the shape of the puddle" instead, which this
     * now does: an obstacle breaking a pool's surface shows up as a dip back
     * to a small depth right where it interrupts the water, rather than a
     * band painted straight through it.
     *
     * The one simplification this DOES still make is the SAME single-
     * dominant-axis approximation build_xflow() (sand.c) already accepts
     * for the simulation's own movement - `|gy| >= |gx|` picks a straight
     * vertical or horizontal "toward the surface" rather than bracketing a
     * genuinely diagonal gravity with two rays. Good enough for a cosmetic
     * cue built on the same idiom already trusted for movement. The other
     * accepted trade-off is staleness under the dirty-row optimisation -
     * only rows something else marked dirty ever get repainted, so a
     * column's stored depth for a row that has not repainted in a while can
     * lag the puddle's current shape - see col_local_depth[]'s own comment
     * in app_sand.c for why that costs nothing extra to accept and matches
     * the precedent already established for foam's own drift.
     *
     * INTERIOR ONLY, DELIBERATELY NOT THE RIM. A rim cell already carries
     * two terms - its own fill level, and liquid_spec[]'s specular shift -
     * and both already use most of the ramp's range between them; a third
     * term stacked on top would spend most of its own range clamped against
     * whichever end the other two had already reached, buying little for
     * the cost of a less legible rim. The interior had nothing until now,
     * so it is the only place adding a term is clearly a gain rather than a
     * diminishing one.
     *
     * The RIM is the opposite case: it is the one place a liquid's fill
     * level is actually true, and it is what lets the pale film at a
     * shallow edge, lava's bright skim, oil's murky olive and acid's vivid
     * lime survive on screen at all - flattening those the way an earlier
     * attempt at this fix did destroyed the very thing this pass exists to
     * keep. A rim cell keeps exactly the fill-indexed lookup every other
     * material already gets, shifted by liquid_spec[] indexed by the
     * CARDINAL bits alone - see that table's own comment above for what
     * the shift means and material_set_gravity() for where it comes from,
     * and MATERIAL_EDGE_CARDINAL's own comment in material.h for why the
     * index has to be masked down rather than used raw now that `mask`
     * carries diagonal bits too.
     *
     * WATER'S rim then gets one thing no other liquid does: foam, gated
     * purely by curvature - see this file's own comment on that above.
     * Oil, lava and acid fall through this same block unchanged; only the
     * id check below diverts water into the foam path.
     *
     * `hash` ITSELF ARRIVES ALREADY COARSENED FOR WATER. paint_row_n() in
     * app_sand.c hands this function material_grain_hash(cx, cy) for every
     * material except water, and material_grain_hash(cx >> FOAM_BLOB_SHIFT,
     * cy >> FOAM_BLOB_SHIFT) for water - an 8x8 block of cells sharing one
     * value instead of each rolling its own - so foam clusters in blobs
     * rather than speckling one cell at a time. This function does not
     * know that, and does not need to: it just uses whatever `hash` it was
     * handed, the same as it always has.
     *
     * That is safe ONLY because foam is the SOLE consumer of water's hash.
     * Water is KIND_LIQUID and returns from this function below, so it
     * never reaches the `switch (CELL_MATERIAL(c))` further down that reads
     * `hash` for glass, stone, wood and the extended materials' grain - the
     * coarsening never touches any of them. If water is ever given a
     * speckle of its own - the way stone and wood have one - it will want
     * the FINE per-cell hash like everything else, and the day that
     * happens this coarsening has to move from "every water cell,
     * unconditionally" to "only where foam actually reads it", or the new
     * speckle will silently stripe in 8x8 blocks with no test anywhere
     * catching why. Nothing enforces that today - it is a fact about the
     * rest of this file, not a type - which is exactly why it is written
     * down here rather than left to be rediscovered. */
    if (material_of(c)->kind == KIND_LIQUID) {
        const uint8_t id = CELL_MATERIAL(c);
        const unsigned cardinal = mask & MATERIAL_EDGE_CARDINAL;

        if (cardinal == 0) {
            /* THE SAME SHIFT FOR EVERY LIQUID, WATER INCLUDED - no more id
             * check here at all. `depth` is clamped to DEPTH_SATURATE_CELLS
             * before it ever reaches the divide, rather than trusting the
             * divide itself to fall to zero cleanly past that point: depth
             * is unsigned and can run all the way to 255 (paint_row_n()'s
             * own clamp in app_sand.c), and DEPTH_SATURATE_CELLS - depth
             * would wrap to a huge unsigned value the moment depth exceeds
             * it if this cap were not applied first. */
            const unsigned depth_capped = depth < DEPTH_SATURATE_CELLS ? depth : DEPTH_SATURATE_CELLS;

            /* Distance-to-saturation, out of DEPTH_RANGE whole shade steps -
             * see DEPTH_SATURATE_CELLS's own comment above for the constant
             * this divides by and why 255 was wrong. Plain integer
             * arithmetic throughout, no intermediate fixed-point scale to
             * promote to and shift back down again: that machinery existed
             * only to keep a wave residual's fractional resolution alive
             * through the merge, and there is no wave left to preserve. */
            const int bright =
                ((int)DEPTH_RANGE * (int)(DEPTH_SATURATE_CELLS - depth_capped)) / (int)DEPTH_SATURATE_CELLS;
            int idx = (int)MASS_MAX - bright;
            idx = idx < 0 ? 0 : (idx > MASS_MAX ? MASS_MAX : idx);
            out[0] = palette[CELL_MAKE(id, (uint8_t)idx)];
        } else {
            int idx = (int)v + liquid_spec[cardinal];
            idx = idx < 0 ? 0 : (idx > MASS_MAX ? MASS_MAX : idx);
            out[0] = palette[CELL_MAKE(id, (uint8_t)idx)];

            if (id == MAT_WATER) {
                const unsigned empty_count = material_popcount8(mask);
                unsigned curvature = (empty_count > 3) ? (empty_count - 3) : (3 - empty_count);
                if (curvature > WATER_FOAM_CURVATURE_MAX) {
                    curvature = WATER_FOAM_CURVATURE_MAX;
                }

                /* ADD, never `^` - see the long comment above this file's
                 * curvature block for why XOR fails: water_foam_threshold's
                 * windows are power-of-two aligned, and XORing the low
                 * three bits by a fixed value maps an aligned window onto
                 * itself or another aligned window, so roughly half of all
                 * phase steps leave the foaming set completely unchanged -
                 * measured as 4, 6 and 4 of 8 consecutive phase pairs
                 * identical at the three thresholds, worst at the commonest
                 * (medium) curvature. Addition has no such alignment to
                 * preserve: it ROTATES the window by one place every phase
                 * step, so consecutive phases can never match. 0x9E37u is
                 * the same constant either mixing would use - the point of
                 * it here is only that it is ODD (ends in 7, an odd hex
                 * digit), so multiplying `foam_phase` by it still steps
                 * through all eight low-bit values as phase climbs by 1,
                 * rather than by some larger stride that would revisit
                 * values and stall in a different way. */
                const unsigned dithered = hash + foam_phase * 0x9E37u;
                if ((dithered & 7u) < water_foam_threshold[curvature]) {
                    out[0] = water_foam;
                }
            }
        }
        out[1] = out[0];
        out[2] = out[0];
        return MATERIAL_FLAT;
    }

    switch (CELL_MATERIAL(c)) {
        case MAT_EXTENDED:
            /* Switched on the low nibble, which for these is their identity
         * rather than a variant - see MATX(). Anything without a grain of
         * its own falls through to the flat palette entry below.
         *
         * Metal gets its own leading equality check, ahead of the guard
         * below, because it returns a different PATTERN (HATCHED) rather
         * than just a different colour - it cannot live inside the
         * ternary, which only ever chooses a colour for one shared
         * MATERIAL_SPECKLED return. That guard-plus-ternary shape below is
         * otherwise untouched and back to the three materials it was
         * measured at: a respelling of it into a switch cost 14% through
         * the inlining cliff, and a single unhinted branch cost 26% of a
         * benchmark, simulation byte-identical either way - see
         * docs/Sand/Tuning-At-a-Glance.md. Adding metal's check ahead of it
         * is one more cheap equality test per extended cell, not a
         * restructure of the measured shape. */
            if (v == MATX_METAL) {
                out[0] = metal_grain[hash & 7u];
                out[1] = metal_dither;
                out[2] = metal_shine;
                return MATERIAL_HATCHED;
            }
            if (v == MATX_PLANT || v == MATX_LEAF || v == MATX_ICE) {
                out[0] = (v == MATX_PLANT) ? plant_grain[hash & 7u]
                         : (v == MATX_LEAF) ? leaf_grain[hash & 7u]
                                            : ice_grain[hash & 7u];
                out[1] = out[0];
                out[2] = out[0];
                return MATERIAL_SPECKLED;
            }
            break;
        case MAT_GLASS: {
            /* CARDINAL bits only - see MATERIAL_EDGE_CARDINAL's own comment in
         * material.h. `mask != 0` would be wrong now that the mask can
         * carry diagonal bits a water rim reads: a pane with every
         * cardinal neighbour occupied but one diagonal empty must stay
         * interior, not spring an edge. */
            const bool edge = (mask & MATERIAL_EDGE_CARDINAL) != 0;
            out[0] = edge ? glass_edge_body[v][hash & 3u] : glass_body[v][hash & 3u];
            out[1] = edge ? glass_edge_dither[v] : glass_dither[v];
            out[2] = edge ? glass_edge_shine[v] : glass_shine[v];
            return MATERIAL_HATCHED;
        }
        case MAT_STONE:
            /* Same CARDINAL-only test as glass above, and for the same reason -
         * see MATERIAL_EDGE_CARDINAL's own comment in material.h. */
            out[0] =
                ((mask & MATERIAL_EDGE_CARDINAL) != 0) ? stone_edge_speckle[v][hash & 7u] : stone_speckle[v][hash & 7u];
            out[1] = out[0];
            out[2] = out[0];
            return MATERIAL_SPECKLED;
        case MAT_WOOD:
            if (v != 0) {
                break; /* alight: one flat glow, not grain */
            }
            out[0] = wood_grain[hash & 7u];
            out[1] = out[0];
            out[2] = out[0];
            return MATERIAL_SPECKLED;
        default: break;
    }

    out[0] = palette[c];
    out[1] = out[0];
    out[2] = out[0];
    return MATERIAL_FLAT;
}

/* One reaction row per extended material, indexed by the low nibble.
 *
 * This is where an extended material gets to be itself. The physics row
 * above is shared, so everything that distinguishes one from another lives
 * either here or in the palette. */
static const char* const extended_names[MATERIAL_EXTENDED_COUNT] = {
    [MATX_ICE] = "Ice",
    [MATX_PLANT] = "Plant",
    [MATX_LEAF] = "Leaf",
    [MATX_METAL] = "Metal",
};

const char*
material_name(cell_t c) {
    if (cell_is_extended(c)) {
        const char* n = extended_names[CELL_VARIANT(c)];
        return (n != NULL) ? n : "?";
    }
    return materials[CELL_MATERIAL(c)].name;
}

const reaction_t extended_reactions[MATERIAL_EXTENDED_COUNT] = {

    [MATX_ICE] =
        {
            /* Snow that stays where it is put.
         *
         * Snow is a powder: it drifts as it falls (scatter 90), floats on
         * water, and melts in anything liquid, so aiming it at one face of
         * a hot vessel is most of the difficulty in using it at all. Ice
         * is the same cold in a form you can BUILD with - it holds still,
         * so a wall of it can be put against the glass you actually meant.
         *
         * Chills harder than snow, 60 against 40, because a solid block
         * pressed against a pane is more contact than a flake resting on
         * it. That also makes it the reliable way to shock glass, which is
         * the thing snow has never quite been.
         *
         * Melts to water near heat like snow does, and slowly in liquid -
         * `thaws` 2 against snow's 4, since a block does not dissolve at
         * the rate a flake does. */
            .chills = 60,
            .heats_to = MAT_WATER,
            .heat_chance = 90,
            .thaws = 2,
        },

    [MATX_PLANT] =
        {
            /* Grows on wet soil, against gravity, and turns to wood where it
         * gets tall. The whole reason it can be an extended material is
         * that none of that needs a variant: growth is SPATIAL - it
         * occupies more cells rather than filling up a counter - so the
         * low nibble stays free to say which extended material it is.
         *
         * 40 in 256 per step is one cell every six or seven steps while
         * there is moisture to spend, and there is only ever as much of
         * that as somebody watered in. Soil that dries out stops a tree
         * where it stands.
         *
         * Six in a line becomes wood. Low enough that a sapling turns into
         * a trunk while you are watching it, high enough that a plant
         * creeping over flat ground never does. */
            /* Drinks briskly. This is drainage as much as it is nutrition -
         * water sitting in a thicket with nowhere to go looks broken, and
         * the fix wants to be visible at the speed the player poured it. */
            .drinks = 40,

            .grows = 12,
            .hardens_to = MAT_WOOD,
            .clings_to = MAT_WOOD,

            /* What hardening leaves behind: a trunk two cells wider than a
         * stick at the foot, and foliage round the top of it.
         *
         * 110 in 256 per candidate space, over the five upward directions
         * of the top three cells of the run - so a crown of three or four
         * leaves, varying, rather than a fixed rosette. */
            .canopy = 110,
            .canopy_to = MATX(MATX_LEAF),
            .trunk_girth = 2,

            /* High, but not certain. At 255 a limb is a perfectly straight
         * ray; the occasional reversion to reckoning from gravity is what
         * bends it back towards upright, which is what a real bough does
         * and what keeps a tree from looking like a diagram. */
            .holds_line = 200, /* and it is part of one, which is the
                                    * same material here and will not be
                                    * once foliage exists */
            .harden_run = 6,
            .harden_chance = 64, /* one in four; measured */

            /* And it POURS, so a handful of seeds scattered over a bed
         * behaves like a handful of seeds rather than hanging wherever the
         * brush left them. It cannot be a KIND_POWDER to get that - see
         * reaction_t.falls - and would not want to be: this rule leaves a
         * grown stem standing, because what is under a stem is more stem.
         *
         * About one step in three, not every step. A grain of sand falls a
         * cell per step and a leaf should not: at full speed a loose scrap
         * of green crossed the board faster than the eye follows, which
         * reads as teleporting rather than as falling. */
            .falls = 85,

            /* The slowest rate this scale can express, and it wants to be.
         * Measured, forty scraps on bare stone: at 3 in 256 half of them
         * were gone by step 60 - two seconds - and a seed poured onto dry
         * soil died before its water arrived. At 1 the half-life is about
         * six seconds, which gives a player time to fetch the watering can
         * and still clears the litter from a broken tree inside twenty. */
            .withers = 1,

            /* And it burns, which is most of the point of growing a tree. Well
         * above wood's 6: green growth catches far more readily than
         * seasoned timber, and it flashes to flame rather than charring,
         * because there is not enough of it in one cell to smoulder. */
            .flammability = 40,

            .dissolvable = 220, /* softer than wood's 160 - acid goes
                                * through leaves faster than through a
                                * plank */
        },

    [MATX_LEAF] =
        {
            /* FOLIAGE. What a tree puts out when a run of it hardens - see the
         * canopy in sand_reactions.c - and deliberately a material of its
         * own rather than more plant.
         *
         * The reason is not that it needs different numbers. It is that
         * every cell of a PLANT is a grower: foliage touches wood, so it
         * never withers, and find_water() walks down through wood, so it
         * can always drink. A canopy made of plant would feed the growth
         * loop with every leaf it put out, and that loop has run away
         * once already. Charging moisture for each leaf would only make it
         * expensive; having no `grows` field at all makes it impossible,
         * which is the better kind of answer.
         *
         * So, pointedly, this row has no `grows`, no `falls` and no
         * `hardens_to`. Leaves do not spread, do not fall and never turn
         * into timber. What they do is hang there being green, catch fire
         * readily, and let water through. */
            .clings_to = MAT_WOOD,
            .sheltered_by = MAT_WOOD, /* a tree in drought keeps its leaves */

            /* It has to DRINK, which sounds like the opposite of everything
         * above and is not optional. Every extended material shares one
         * physics row - KIND_STATIC at stone's density - so water can
         * neither fall through a canopy nor soak into it, and a bowl of
         * leaves holds a pond indefinitely. That was a real bug once
         * already, on the plant, and a leaf is the surface rain actually
         * lands on. */
            .drinks = 40,

            /* And it must be able to GO. Nothing else would ever clear it:
         * with no `falls`, a crown whose trunk burns away would hang in
         * the air permanently. Withering handles it, and `clings_to`
         * above is what stops a living tree shedding - a leaf touching
         * wood is safe however dry the ground gets. At the same rate as
         * the plant, not twice it: a stem being stouter than a leaf was
         * the old reasoning, and a shed leaf lasting longer used up the
         * difference. */
            .withers = 1,

            /* Catches far more readily than green stem (40) or seasoned wood
         * (6). A fire that reaches a canopy should run through it, which
         * is both what happens and the best thing to look at. */
            .flammability = 90,

            .dissolvable = 240, /* the softest thing on the board */
        },

    /* METAL. Dirt smelted by sustained heat - see
     * docs/Sand/Metal-Smelting-Plan.md, which this row follows exactly.
     *
     * No variant to spend: an extended material's low nibble is its
     * identity, so metal cannot glow, cannot hold a temperature, and
     * cannot melt. The design leans into that rather than fighting it -
     * metal's whole job is the one thing nothing else on the board does
     * well, moving heat a LONG way, and everything below is in service of
     * that one axis. */
    [MATX_METAL] =
        {
            /* Rolled per cell crossed by conduct_heat()'s walk, so depth d
         * succeeds with probability (conducts/256)^d - see that
         * function's own comment in sand_reactions.c. 248 puts the mean
         * walk at roughly CONDUCT_REACH cells (32), against stone and
         * glass's 220:
         *
         *     depth   stone/glass (220)   metal (248)
         *        8           30%              78%
         *       16          8.5%              60%
         *       32          0.8%              36%
         *
         * That is the point of the material: at CONDUCT_REACH itself the
         * cap starts doing real work rather than being slack, so the
         * self-growing rod (below, and see sand_reactions.c's own
         * comment on the wet-dirt stage of try_heat_transform()) stops at
         * a length the player can see is a designed limit rather than an
         * arbitrary one. Starting point, not final - tune on device like
         * every other constant in this table. */
            .conducts = 248,

            /* Balance revision, 2026-08-30: metal now resists acid as hard as
         * this field allows without opting all the way out (0 is immune -
         * see its own comment in material.h - and would also drop metal
         * from the generated reaction docs, which 1 does not). Previously
         * 110, deliberately FASTER than stone's 60 so acid was metal's
         * documented counter; superseded, not layered on top of, by this
         * value - see docs/Sand/Metal-Smelting-Plan.md, updated to match.
         * A future balance pass may revisit whether metal should cost
         * something else instead, now that it is no longer acid's weak
         * point. */
            .dissolvable = 1,

            /* Deliberately no `heats_to`, `heat_ramp`, `heat_chance`, `chills`
         * or anything else thermal. With no variant metal cannot ramp,
         * and a memoryless heat_chance roll would mean a metal wall
         * beside lava randomly catching and turning to lava on its own -
         * which would make a metal container just as unsafe as the stone
         * it replaces. So metal SURVIVES heat outright: it is what makes
         * it worth building the rest of a vessel out of, the one axis
         * stone and glass do not both have (stone survives heat but
         * conducts it no better than glass; glass melts). Every field not
         * named above is 0, and that is a decision, not an oversight -
         * see reactions[]'s own top comment on what an absent field means
         * field by field. */
        },
};

const gfx_color_t*
material_palette(void) {
    return palette;
}
