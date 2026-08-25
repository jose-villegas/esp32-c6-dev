#include "material.h"

/*=============================================================================
 * The table.
 *
 * `const`, so it lives in flash rather than RAM. Adding a material is a row.
 *===========================================================================*/

const material_t materials[MATERIAL_MAX] = {
    [MAT_EMPTY] = {
        .name    = "empty",
        .kind    = KIND_NONE,
        .density = 0,
    },

    [MAT_SAND] = {
        .name    = "Sand",
        .kind    = KIND_POWDER,
        .density = 60,
        /* Buried sand locks up quickly - this is what stops a floor of it
         * skating sideways on the faintest tilt. */
        .slip    = 96,
        .repose  = 7,        /* about 35 degrees, dry sand */
        .scatter = 40,
    },

    [MAT_WATER] = {
        .name    = "Water",
        .kind    = KIND_LIQUID,
        .density = 30,       /* lighter than sand, so sand sinks through it */

        /* Unused by a liquid: it does not slide, pile or scatter, it flows
         * between neighbours as an amount. Left at the values that mean "no
         * resistance", so that anything reading them generically still gets a
         * sensible answer. */
        .slip    = 255,
        .repose  = 0,
        .scatter = 0,

        .mobility = 255,     /* VISCOSITY, inverted - see material.h's own
                              * comment on the field. Water is the runny
                              * one and moves on every step it can, which
                              * is exactly what every liquid did before
                              * this field had a second reader, so water's
                              * behaviour is unchanged by its arrival. */
    },

    [MAT_STONE] = {
        .name    = "Stone",
        .kind    = KIND_STATIC,
        .density = 200,      /* nothing displaces it */
        .slip    = 0,
        .repose  = 0,
        .scatter = 0,
    },

    [MAT_GAS] = {
        .name    = "Gas",
        .kind    = KIND_GAS,
        .density = 10,       /* between empty (0) and water (30), so sand and
                               * water sinking through it in the main sweep
                               * displace it automatically - not sensitive,
                               * anywhere from about 1 to 25 works the same */

        /* Same "no resistance" values as water, and for the same reason:
         * gas rises and slides via sand's own try_fall_or_scatter()/
         * try_slide() (see sand_gas.c), inverted, and a real angle of
         * repose or load resistance would stop it from spreading at all,
         * the opposite of what it is for. */
        .slip    = 255,
        .repose  = 0,
        .scatter = 120,      /* well above sand's 40 - a visibly turbulent,
                               * wispy rise rather than a rigid column */

        .decay   = 32,        /* 15 ticks needed to clear a fresh grain,
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

        .mobility = 96,        /* ~2.7 steps average between rises - was 32
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

        .sight = 16,           /* was the global SAND_GAS_SIGHT constant,
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

    [MAT_FIRE] = {
        .name    = "Fire",
        .kind    = KIND_GAS,    /* rises and disperses through the exact
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

        .density = 15,          /* strictly between gas's 10 and sand's
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
        .slip    = 255,         /* no resistance, same reasoning as
                                 * gas's own row above */
        .repose  = 0,
        .scatter = 120,         /* matches gas's own figure - equally
                                 * turbulent rise, tune independently
                                 * later if it should read differently */

        .decay    = 96,        /* shorter life than gas's 32: 15 ticks *
                                * 256/96 (~2.7 steps average between
                                * ticks) ~= 40 steps, under a second at
                                * ~60fps - fire burns out noticeably
                                * faster than gas fades. Starting point,
                                * not final - tune on device like every
                                * other constant here. */
        .mobility = 96,         /* matches gas's own figure as a
                                * starting point - tune independently if
                                * fire should rise faster/slower than
                                * gas once seen in motion */
        .sight    = 5,          /* noticeably tighter than gas's 16 -
                                * "tighter instead of sparse". Starting
                                * point, not final - tune on device */
    },

    [MAT_WOOD] = {
        .name    = "Wood",
        .kind    = KIND_STATIC,     /* a log does not fall over or pile up
                                     * - it sits where it is drawn until
                                     * fire chars it into an ember (see
                                     * sand_reactions.c) */
        .density = 150,             /* above sand (60) and water (30), so
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

    [MAT_STEAM] = {
        .name    = "Steam",
        .kind    = KIND_GAS,        /* rises and disperses through the
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

        .density = 5,               /* below gas (10) and fire (15), so
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
        .slip    = 255,             /* no resistance, same reasoning as
                                     * gas's own row */
        .repose  = 0,
        .scatter = 140,             /* above both gas's 120 and fire's
                                     * 120 - a wispier, more turbulent
                                     * rise. Starting point, not final -
                                     * tune on device like every other
                                     * constant here. */

        .decay    = 24,             /* matches ember's own figure as a
                                     * starting point, tune independently
                                     * later - roughly 160 steps, ~2.7s
                                     * at ~60fps, so a wisp of steam
                                     * visibly fades rather than either
                                     * lingering or vanishing at once. */
        .mobility = 160,            /* noticeably faster than gas's 96 or
                                     * fire's 96 - steam should read as
                                     * rising eagerly off a boiling pot,
                                     * not drifting the way gas does.
                                     * Starting point, not final - tune
                                     * on device like every other
                                     * constant here. */
        .sight    = 20,             /* wider than gas's 16 - a puff of
                                     * steam disperses generously rather
                                     * than staying a tight column the
                                     * way fire's own 5 does. Starting
                                     * point, not final - tune on device
                                     * like every other constant here. */
    },

    [MAT_SMOKE] = {
        .name    = "Smoke",
        .kind    = KIND_GAS,        /* same pass as steam, gas and fire -
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

        .density = 7,               /* between steam's 5 and gas's 10 -
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
        .slip    = 255,             /* no resistance, same reasoning as
                                     * gas's own row */
        .repose  = 0,
        .scatter = 150,             /* just above steam's 140 - smoke
                                     * curls a little more than steam
                                     * does. Starting point, not final -
                                     * tune on device like every other
                                     * constant here. */

        .decay    = 16,             /* lower than steam's 24, so smoke
                                     * LASTS LONGER - decay is the chance
                                     * per step of losing a life tick, so
                                     * smaller is slower. Roughly 240
                                     * steps, ~4s at ~60fps: soot hangs
                                     * around after the fire is out,
                                     * where a wisp off a pot does not.
                                     * Starting point, not final - tune
                                     * on device like every other
                                     * constant here. */
        .mobility = 120,            /* between gas's 96 and steam's 160 -
                                     * smoke climbs, but lazily, where
                                     * steam comes off a boil eagerly.
                                     * Starting point, not final - tune
                                     * on device like every other
                                     * constant here. */
        .sight    = 24,             /* widest of any gas here (steam 20,
                                     * gas 16, fire 5) - smoke spreads
                                     * and thins into a haze rather than
                                     * holding a column. Starting point,
                                     * not final - tune on device like
                                     * every other constant here. */
    },

    [MAT_EMBER] = {
        .name    = "Ember",
        .kind    = KIND_STATIC,     /* what wood chars into - stays put and
                                     * keeps burning in place rather than
                                     * floating off as a flame would. See
                                     * sand_reactions.c's top comment for
                                     * why this is a genuinely different
                                     * material from fire rather than wood
                                     * just igniting straight to MAT_FIRE. */
        .density = 150,             /* same as wood - it is charred wood,
                                     * still a solid log's worth of mass
                                     * sitting in the same cell */

        .decay = 24,                /* roughly 15 * (256/24) ~= 160 steps,
                                     * ~2.7s at this app's ~60fps step rate
                                     * - a log that visibly smoulders
                                     * rather than one that either lingers
                                     * forever or gutters out at once.
                                     * Starting point, not final - tune on
                                     * device like every other constant
                                     * here.
                                     *
                                     * Worth knowing rather than chasing as
                                     * a bug: at density 150, an ember is
                                     * essentially never smother()'d - that
                                     * predicate needs all four neighbours
                                     * STRICTLY denser, and only stone (200)
                                     * qualifies. Burying a log in sand will
                                     * not put it out; only decay, or water,
                                     * does. */
    },

    [MAT_OIL] = {
        .name    = "Oil",
        .kind    = KIND_LIQUID,
        .density = 22,       /* below water's 30, which is what makes oil
                              * float rather than sink when the two meet -
                              * see the density swap in
                              * move_liquid_grain() (sand_liquid.c).
                              * Above fire's 15 so it is not something a
                              * flame can shove around. Sand (60) still
                              * sinks straight through it. */

        .mobility = 140,     /* VISCOSITY, inverted - see material.h. Oil
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
        .slip    = 255,
        .repose  = 0,
        .scatter = 0,
    },

    [MAT_LAVA] = {
        .name    = "Lava",
        .kind    = KIND_LIQUID,
        .density = 45,       /* above water (30) so lava sinks and water
                              * floats when they meet, below sand (60) so
                              * sand still sinks through lava. Both fall
                              * out of the existing rules; neither needs
                              * lava-specific code. */
        .slip    = 255,
        .repose  = 0,
        .scatter = 0,

        .mobility = 70,      /* VISCOSITY, inverted - see material.h, and
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
                              * not freeze lava outright - the
                              * wall-rebound splash moves liquid without
                              * consulting the gate - but it made it
                              * twelve times slower than intended,
                              * measured at 249 steps to cross what takes
                              * 20 here. Slow enough to look deliberate,
                              * which is why it lasted. Zero now reads as
                              * free-flowing so the next liquid to forget
                              * this errs towards water, where the mistake
                              * is obvious - see liquid_may_move() in
                              * sand_liquid.c.
                              * Starting point, not final - tune on device
                              * like every other constant here. */

        .decay   = 0,        /* MUST stay 0, and this is not a style
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

    [MAT_ACID] = {
        .name    = "Acid",
        .kind    = KIND_LIQUID,
        .density = 38,       /* between water's 30 and lava's 45: acid
                              * sinks through water and floats on lava,
                              * both of which fall out of
                              * sink_through_lighter_liquid() with no
                              * acid-specific code. Sand (60) still sinks
                              * through it, which matters - a grain has to
                              * get INTO the acid to be eaten by it. */
        .slip    = 255,      /* the usual "no resistance" values a liquid
                              * leaves these at - see water's own row */
        .repose  = 0,
        .scatter = 0,

        .mobility = 220,     /* VISCOSITY, inverted - see material.h.
                              * Just short of water's 255: acid is runny,
                              * and being fractionally slower is enough to
                              * read as heavier without behaving like oil.
                              * Starting point, not final - tune on device
                              * like every other constant here. */
    },

    [MAT_GLASS] = {
        .name    = "Glass",
        .kind    = KIND_STATIC,
        .density = 200,      /* stone's own figure, and for the same
                              * reason: nothing displaces it, and it
                              * smothers a buried flame the way stone
                              * does. Glass differs from stone in what
                              * ACID does to it, not in how it sits. */
        .slip    = 0,
        .repose  = 0,
        .scatter = 0,
    },

    [MAT_SNOW] = {
        .name    = "Snow",
        .kind    = KIND_POWDER,
        .density = 15,       /* Under oil's 22 and well under water's 30,
                              * so snow FLOATS on both - can_enter() lets
                              * the denser one displace it and that is the
                              * whole mechanism. Snow sitting on top of a
                              * pool is right, and it also puts the snow
                              * where it is useful: on the surface, in
                              * reach of whatever is above it. */
        .slip    = 64,       /* Stickier than sand's 96. Snow clumps, and a
                              * bank that holds its shape is what makes it
                              * possible to pack snow ONTO a glass pane and
                              * have it stay there long enough to matter. */
        .repose  = 9,        /* ~42 degrees, steeper than dry sand's ~35 -
                              * again so a bank holds. */
        .scatter = 90,       /* High, and the one purely cosmetic number
                              * here: falling snow drifts instead of
                              * dropping straight, which is most of what
                              * makes it read as snow rather than as pale
                              * sand. */
    },

    /* The remaining slot is unused and left zeroed except for these, which make an
     * unknown material inert rather than undefined: it never moves and nothing
     * can displace it. Designated initialisers zero the rest. */
    [MAT_COUNT ... MATERIAL_MAX - 1] = {
        .name    = "?",
        .kind    = KIND_STATIC,
        .density = 255,
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
    [MAT_ACID] = {
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

        .fizz = 40,     /* about one bite in six leaves a wisp of smoke.
                         * Acid was silent before this: cells simply
                         * vanished, with nothing on screen to say the
                         * acid was the cause or that it was working.
                         *
                         * Modest on purpose - every bite fizzing would
                         * bury a dissolving pile under its own exhaust,
                         * and the smoke has to be readable against the
                         * thing being eaten rather than instead of it.
                         * Starting point, not final - tune on device like
                         * every other constant here. */
    },

    [MAT_OIL] = {
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
        .needs_air    = 1,
        .ignites_to   = MAT_FIRE,   /* burns straight to flame, unlike
                                     * wood: there is no log left to
                                     * smoulder, the fuel simply goes.
                                     * The flame rises off, exposing the
                                     * layer beneath, which is what eats
                                     * the pool downward */
    },

    [MAT_LAVA] = {
        /* A heat source that happens to be a liquid, and the clearest
         * proof the movement and reaction axes really are independent:
         * KIND_LIQUID in materials[] above, `burns` here, and not one
         * line of code anywhere knows about the combination. */
        .burns = 1,

        .quench_to = MAT_STONE,   /* water puts lava out by turning it to
                                   * rock, rather than by making it
                                   * vanish. The water pays a unit of its
                                   * own mass for it, exactly as it does
                                   * quenching a fire (pay_quench_cost()),
                                   * so a small puddle cannot pave an
                                   * ocean of lava for free. */

        .flare = 16,              /* well below ember's 48: lava licks the
                                   * occasional flame rather than burning
                                   * with one. Mostly so a pool reads as
                                   * dangerous rather than decorative.
                                   * Starting point, not final. */

        /* No residue: lava never burns out (decay 0 above), so nothing
         * here would ever fire. No conducts either - lava IS the heat,
         * it does not pass someone else's along. */
    },

    [MAT_SAND] = {
        /* Acid eats sand readily - it is the obvious thing to point acid
         * at, and the one that shows what it does. */
        .dissolvable = 200,

        .heats_to    = MAT_GLASS,
        .heat_chance = 16,   /* 16 in 256 per adjacent heat source per
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

    [MAT_GLASS] = {
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
        .heats_to    = MAT_LAVA,
        .heat_ramp   = 64,

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
        .cools       = 5,

        /* Shocked glass goes back to being sand, which closes the loop it
         * opened: sand fuses to glass under heat, glass returns to sand
         * when the heat is pulled out of it too fast. The player can
         * un-make the material without a second material and without
         * spending one of the two remaining slots. */
        .shatters_to = MAT_SAND,
    },

    [MAT_SNOW] = {
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
        .chills      = 40,
        .heats_to    = MAT_WATER,
        .heat_chance = 120,

        /* And it melts in liquid, at its own far slower rate - see
         * reaction_t.thaws. 120 beside a flame is two steps; 4 in water is
         * nearer a second per touching face, which is long enough to watch
         * a drift land on a pond and ride on it before it goes. Snow is
         * lighter than water precisely so that it does. */
        .thaws       = 4,
    },

    [MAT_STONE] = {
        .dissolvable = 60,   /* Stone gives way to acid now, just slowly -
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

    [MAT_GAS] = {
        /* 255: gas catches the instant fire touches it, and - because
         * try_ignite() checks for 255 before ever drawing a random number -
         * costs no RNG draw doing it, exactly as a plain boolean flammable
         * flag used to. That is what keeps every existing gas/fire test,
         * and the device frame-budget captures that depend on their exact
         * random sequence, bit-identical after this table split. */
        .flammability = 255,
        .ignites_to   = MAT_FIRE,   /* the only fuel today; written out
                                     * explicitly rather than relying on
                                     * the "0 reads as MAT_FIRE" default,
                                     * since MAT_FIRE is what should be
                                     * here regardless of which enum value
                                     * happens to be 0 */
    },

    [MAT_FIRE] = {
        .burns = 1,     /* the one heat source that exists today - see
                         * sand_reactions.c's dispatch, which now keys off
                         * this instead of CELL_MATERIAL(c) == MAT_FIRE */

        .residue = 40,    /* chance in 256 that a burnt-out fire cell leaves
                         * MAT_STEAM behind - smoke, physically the same
                         * material a kettle's steam is (see MAT_STEAM's
                         * own row above). Lower than ember's 90: a flame
                         * guttering out on its own is a smaller, briefer
                         * event than a whole ember finishing a slow
                         * burn. Starting point, not final - tune on
                         * device like every other constant here. */

        .quench_to = MAT_STEAM,   /* touching water no longer just
                                  * vanishes - it boils off, at the cost
                                  * of a unit of the water's own mass
                                  * (see step_one_burning_cell() in
                                  * sand_reactions.c). Steam is a
                                  * byproduct, not a free lunch: a pot
                                  * boiled dry should eventually run dry. */
    },

    [MAT_WOOD] = {
        /* 6 in 256 is roughly 43 steps of contact with a single flame
         * before it catches - a fire that has to work at it, which is
         * the whole point of "slowly consumed" (see sand_reactions.c's
         * top comment for why wood does not just ignite straight to
         * MAT_FIRE the way gas does). Starting point, not final - tune
         * on device like every other constant here. */
        .flammability = 6,
        .ignites_to   = MAT_EMBER,   /* chars, does not flash - see
                                      * sand_reactions.c's top comment */
        .dissolvable  = 160,         /* slower than sand's 200: a plank
                                      * holds out a moment longer than a
                                      * loose pile does */
    },

    [MAT_EMBER] = {
        .dissolvable = 160,  /* what is left of a burning log gives way
                              * the same as the log would */

        .burns = 1,     /* a second heat source, alongside fire - see
                         * sand_reactions.c's dispatch, which is why that
                         * now keys off reaction_t.burns rather than
                         * CELL_MATERIAL(c) == MAT_FIRE */

        .residue = 90,  /* well above fire's 40: a whole ember finishing
                         * its slow burn is a bigger, more definite event
                         * than a flame guttering out, and should leave
                         * smoke behind far more often. Starting point,
                         * not final - tune on device like every other
                         * constant here. */

        .quench_to = MAT_STEAM,   /* same reasoning as fire's own row -
                                  * see there. */

        .flare = 48,    /* chance in 256, per step, that this ember emits
                         * a MAT_FIRE cell into an empty cardinal neighbour
                         * - an ember is KIND_STATIC and would otherwise be
                         * a glowing brick with no flame licking up off it.
                         * The emitted fire is ordinary MAT_FIRE and rises
                         * on its own through sand_step_gas(), so "wood
                         * burning below, flame above" falls out of this
                         * one field rather than needing any upward-aware
                         * code in sand_reactions.c. Starting point, not
                         * final - tune on device like every other
                         * constant here. */
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
#define LERP_CH(lo, hi, shift, sh)                                        \
    ((((((lo) >> (shift)) & 0xFF) * (15 - (sh)) +                         \
       (((hi) >> (shift)) & 0xFF) * (sh)) / 15) & 0xFF)

#define LERP(lo, hi, sh)                                                  \
    ((LERP_CH(lo, hi, 16, sh) << 16) |                                    \
     (LERP_CH(lo, hi,  8, sh) <<  8) |                                    \
      LERP_CH(lo, hi,  0, sh))

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
#define GLASS_FROST   0xD6EEF8
#define GLASS_AMBIENT 0x2E6B85
#define GLASS_NEUTRAL 0x7E8E86
#define GLASS_GLOW    0xC8701E
#define GLASS_MOLTEN  0xFFD873

/* The three segments, each mapped onto 0..15 for LERP. Every branch has to
 * compute without dividing by zero even where it is not selected, hence the
 * guards on the denominators. */
#define GLASS_COOL(v)  LERP(GLASS_FROST, GLASS_AMBIENT,                    \
                            ((v) * 15) / (SAND_AMBIENT_HEAT > 0            \
                                          ? SAND_AMBIENT_HEAT : 1))
#define GLASS_WARM(v)  LERP(GLASS_AMBIENT, GLASS_NEUTRAL,                  \
                            (((v) - SAND_AMBIENT_HEAT) * 15) /             \
                            (SAND_SHOCK_HEAT > SAND_AMBIENT_HEAT           \
                             ? SAND_SHOCK_HEAT - SAND_AMBIENT_HEAT : 1))
#define GLASS_HOT(v)   LERP(GLASS_GLOW, GLASS_MOLTEN,                      \
                            (((v) - SAND_SHOCK_HEAT) * 15) /               \
                            (SAND_SHOCK_HEAT < MATERIAL_VARIANTS - 1       \
                             ? MATERIAL_VARIANTS - 1 - SAND_SHOCK_HEAT : 1))

#define GLASS_AT(v)                                                        \
    GFX_RGB((v) <= SAND_AMBIENT_HEAT ? GLASS_COOL(v)                       \
            : (v) < SAND_SHOCK_HEAT  ? GLASS_WARM(v)                       \
                                     : GLASS_HOT(v))

#define GLASS_SHADES                                                       \
    GLASS_AT(0),  GLASS_AT(1),  GLASS_AT(2),  GLASS_AT(3),                 \
    GLASS_AT(4),  GLASS_AT(5),  GLASS_AT(6),  GLASS_AT(7),                 \
    GLASS_AT(8),  GLASS_AT(9),  GLASS_AT(10), GLASS_AT(11),                \
    GLASS_AT(12), GLASS_AT(13), GLASS_AT(14), GLASS_AT(15)

/* The ramp is computed, so the two levels only need to be sane: room
 * temperature strictly inside the range with the shock point above it and
 * below the top. */
_Static_assert(SAND_AMBIENT_HEAT > 0 &&
               SAND_AMBIENT_HEAT < SAND_SHOCK_HEAT &&
               SAND_SHOCK_HEAT < MATERIAL_VARIANTS - 1,
               "glass needs room below ambient for frost, room above the "
               "shock point to keep climbing, and ambient strictly between");
#define SHADES(lo, hi)                                                    \
    GFX_RGB(LERP(lo, hi,  0)), GFX_RGB(LERP(lo, hi,  1)),                 \
    GFX_RGB(LERP(lo, hi,  2)), GFX_RGB(LERP(lo, hi,  3)),                 \
    GFX_RGB(LERP(lo, hi,  4)), GFX_RGB(LERP(lo, hi,  5)),                 \
    GFX_RGB(LERP(lo, hi,  6)), GFX_RGB(LERP(lo, hi,  7)),                 \
    GFX_RGB(LERP(lo, hi,  8)), GFX_RGB(LERP(lo, hi,  9)),                 \
    GFX_RGB(LERP(lo, hi, 10)), GFX_RGB(LERP(lo, hi, 11)),                 \
    GFX_RGB(LERP(lo, hi, 12)), GFX_RGB(LERP(lo, hi, 13)),                 \
    GFX_RGB(LERP(lo, hi, 14)), GFX_RGB(LERP(lo, hi, 15))

/* Sixteen entries for an unused material id, so the table is a full 256 and a
 * corrupt cell byte can only ever index a colour, never run off the end. */
#define UNUSED SHADES(0xFF00FF, 0xFF00FF)

/* THE source of colour. The material table deliberately carries none, so there
 * is one place to change and none to forget. Rows are in material_id_t order. */
static const gfx_color_t palette[256] = {
    SHADES(0x0A0C14, 0x0A0C14),   /* empty - the background */
    SHADES(0xB07430, 0xF2CE90),   /* sand  */
    SHADES(0x77C4E8, 0x14406F),   /* water - shallow is pale, deep is dark */
    SHADES(0x4A4F5A, 0x767D8C),   /* stone */
    SHADES(0x445544, 0xC8E8B8),   /* gas   */
    SHADES(0x400A00, 0xFFE060),   /* fire  - dying ember is dark, freshly
                                    * lit is bright yellow-white; variant
                                    * is life remaining, same trick gas
                                    * already uses */
    SHADES(0x3A2616, 0x7A5230),   /* wood  - dark grain to lit grain */
    SHADES(0x6E8496, 0xF2FAFF),   /* steam - variant is life remaining, so
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
    SHADES(0x2A2622, 0x857A6E),   /* smoke - dying wisp is near-black soot,
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
    SHADES(0x2A0A00, 0xFF7A28),   /* ember - dying char to glowing orange,
                                    * deliberately redder and darker than
                                    * fire's own yellow-white so a
                                    * smouldering log reads differently
                                    * from the flame above it */
    SHADES(0x6E5A22, 0x14100A),   /* oil   - a liquid's variant is FILL
                                    * LEVEL, not life, so this runs the
                                    * same way water's does: a thin film
                                    * is a murky olive and a deep pool is
                                    * nearly black. Dark and warm against
                                    * water's pale blue, so a slick
                                    * floating on water is unmistakable -
                                    * which is the whole point of giving
                                    * oil a density below water's */
    SHADES(0xFFC24A, 0x8A1400),   /* lava  - fill level again, and
                                    * deliberately INVERTED against
                                    * fire's own ramp: a thin skim is
                                    * bright yellow and a deep pool is
                                    * dark red, so depth reads as
                                    * cooling crust rather than as more
                                    * heat. Keeps a lava pool visually
                                    * distinct from the flames it
                                    * flares */
    SHADES(0xEAFF3C, 0x2E6B0A),   /* acid  - a liquid's variant is FILL
                                    * LEVEL, so this runs the way water's
                                    * does: a thin film is a vivid lime and
                                    * a deep pool is dark olive. Saturated
                                    * and yellow-leaning on purpose, to
                                    * keep it clear of gas's pale, washed
                                    * green - the two are never adjacent in
                                    * the density ladder but they are
                                    * adjacent on screen the moment
                                    * something fizzes */
    GLASS_SHADES,                 /* glass - NOT a shade ramp. Glass is the
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
    SHADES(0xC6D8E4, 0xFFFFFF),   /* snow  - a powder, so a shade ramp
                                    * again, and a narrow one: cold blue
                                    * white to plain white. Deliberately
                                    * the palest thing on the board, since
                                    * it has to read as COLD at a glance
                                    * for thermal shock to explain itself */
    UNUSED,
};

const gfx_color_t *material_palette(void)
{
    return palette;
}
