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

/* Each material is written once to both halves of its row pair via this
 * variadic macro. This allows `material_of()` to find the correct row using
 * `cell >> 3`, ignoring the top bit of the low nibble. The macro supports
 * commas, multiline arguments, and comments. Row splitting is detailed in
 * material.h's MATERIAL_ROWS comment. */
#define TWIN_ROW(id, ...)                                                \
    [MATERIAL_ROW(id)] = __VA_ARGS__, [MATERIAL_ROW(id) + 1] = __VA_ARGS__

const material_t materials[MATERIAL_ROWS] = {
    TWIN_ROW(MAT_EMPTY,
        {
            .name = "empty",
            .kind = KIND_NONE,
            .density = 0,
        }),

    TWIN_ROW(MAT_SAND,
        {
            .name = "Sand",
            .kind = KIND_POWDER,
            .density = 60,
            /* Buried sand locks up quickly - this is what stops a floor of it
         * skating sideways on the faintest tilt. */
            .slip = 96,
            .repose = 7, /* about 35 degrees, dry sand */
            .scatter = 40,
        }),

    TWIN_ROW(MAT_WATER,
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
        }),

    TWIN_ROW(MAT_STONE,
        {
            .name = "Stone",
            .kind = KIND_STATIC,
            .density = 200, /* nothing displaces it */
            .slip = 0,
            .repose = 0,
            .scatter = 0,
        }),

    TWIN_ROW(MAT_GAS,
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

            /* Despite its weight, gas is the longest-lived airborne material.
             * With a density of 10 and mobility of 96, it lasts 120 steps, or
             * about 40 seconds. Compare this to steam (5, 160; 160 steps, 53
             * seconds) and smoke (7, 120; 240 steps, 80 seconds). Gas is
             * costly (450 us/step) but outlasts the others. */
            .decay = 6, /* 15 ticks to clear grain, 8 steps avg between ticks
                         * (~120 steps, ~2 sec at 60fps). Gas is whole-grain,
                         * not mass-based, so it doesn't thin out saturated
                         * pockets. Decay prevents infinite solid buildup,
                         * simulating gas. Adjust on device. */

            .mobility = 96, /* ~2.7 steps avg between rises (was 32, ~8
                             * steps), too sluggish on device. Slower than
                             * sand's instant rise, but not stuck. Life ticks
                             * regardless (see tick_decay() in sand_priv.h).
                             * Tune on device. */

            .sight = 16, /* Material's own `sight` figure, wider than water's
                          * `SAND_LIQUID_SIGHT` (8), for faster/further gas
                          * dispersal using same `equalise_*()` mechanism. */
        }),

    TWIN_ROW(MAT_FIRE,
        {
            .name = "Fire",
            .kind = KIND_GAS, /* rises and disperses via sand_step_gas(),
                               * tighter and shorter-lived than gas via
                               * `sight`/`decay` - replaces KIND_STATIC
                               * version (immobile, never buriable) */

            .density = 15, /* Strictly between gas's 10 and sand's 60. Three
                            * dependencies: sand sinks through fire (fire <
                            * sand), fire avoids smothering via gas
                            * (smothered(), sand_reactions.c, requires
                            * neighbour's density > fire), and fire mixes with
                            * newly ignited elements (can_enter() needs
                            * strictly greater density). */
            .slip = 255,   /* no resistance, same reasoning as
                                 * gas's own row above */
            .repose = 0,
            .scatter = 120, /* matches gas's own figure - equally
                                 * turbulent rise, tune independently
                                 * later if it should read differently */

            .decay = 96,    /* Shortest life in air, gas longest. Gap wider
                             * than 32: 15 ticks * 256/96 (~2.7 steps) ~= 40
                             * steps, under a second at ~60fps. Fire burns out
                             * faster than gas fades. Starting point; tune on
                             * device. */
            .mobility = 96, /* matches gas's own figure as a
                                * starting point - tune independently if
                                * fire should rise faster/slower than
                                * gas once seen in motion */
            .sight = 5,     /* noticeably tighter than gas's 16 -
                                * "tighter instead of sparse". Starting
                                * point, not final - tune on device */
        }),

    TWIN_ROW(MAT_WOOD,
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
        }),

    TWIN_ROW(MAT_STEAM,
        {
            .name = "Steam",
            .kind = KIND_GAS, /* Rises and disperses like gas and fire;
                               * lighter and faster. Steam is boiled water or
                               * flashed off fire. Fuel burning leaves
                               * MAT_SMOKE. Initially one material, but
                               * separate rows for correct visuals. */

            .density = 5, /* Below gas (10) and fire (15), allowing them to
                           * rise and mix with steam. can_enter() requires
                           * higher density for displacement, preventing steam
                           * from displacing gas or fire. Steam's lower
                           * density than WATER (30) causes it to bubble up,
                           * as seen in try_bubble() in sand_gas.c. */
            .slip = 255,  /* no resistance, same reasoning as
                                     * gas's own row */
            .repose = 0,
            .scatter = 140, /* above both gas's 120 and fire's
                                     * 120 - a wispier, more turbulent
                                     * rise. Starting point, not final -
                                     * tune on device like every other
                                     * constant here. */

            .decay = 12,     /* RAISED TO ~320 STEPS (~5.3S AT ~60FPS), 33%
                              * LONGER THAN SMOKE'S 16 STEPS (~4S). STEAM
                              * DECAYS SLOWER THAN SMOKE BUT NO TEST CHANGE
                              * NEEDED. STARTING LIFE IS MATERIAL_VARIANTS - 1
                              * = 15. NOT MEASURED ON DEVICE YET. */
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
        }),

    TWIN_ROW(MAT_SMOKE,
        {
            .name = "Smoke",
            .kind = KIND_GAS, /* Similar to steam, gas, and fire; smoke is
                               * FUEL's residue, steam is WATER's. Both behave
                               * alike, thus this row mirrors steam's. They
                               * differ only in appearance to distinguish fire
                               * smoke from kettle steam. Palette defines this
                               * row. */

            .density = 7, /* Between Steam's 5 and Gas's 10 - not load
                           * bearing; prevents smoke and steam from being
                           * mutually undisplaceable. Smoke is heavier, so
                           * steam rises through it, correct for basin over
                           * fire. */
            .slip = 255,  /* no resistance, same reasoning as
                                     * gas's own row */
            .repose = 0,
            .scatter = 150, /* just above steam's 140 - smoke
                                     * curls a little more than steam
                                     * does. Starting point, not final -
                                     * tune on device like every other
                                     * constant here. */

            .decay = 16,     /* Smoke lasts longer than steam (240 steps, ~4s
                              * at 60fps). Decay is the chance per step of
                              * losing a life tick, smaller value means slower
                              * decay. Starting point, not final; tune on
                              * device like other constants. */
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
        }),

    TWIN_ROW(MAT_OIL,
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

            .mobility = 140, /* VISCOSITY inverted - see material.h. Oil lags
                              * behind tilts, holds slopes. Water spreads in 8
                              * steps, oil in 18. Slower than water, not
                              * sludge. Stops oil and water tearing. Swaps
                              * cells at slower liquid's pace, settling over a
                              * second. Tune on device. */

            /* The same "no resistance" values water uses, and for the same
         * reason: a liquid does not slide, pile or scatter, it flows
         * between neighbours as an amount. */
            .slip = 255,
            .repose = 0,
            .scatter = 0,
        }),

    TWIN_ROW(MAT_LAVA,
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

            .mobility = 70, /* VISCOSITY inverted, see material.h. Slowest
                             * process: 44 steps vs water's 8 and oil's 18.
                             * Lava's movement mimics molten rock. Initially
                             * immobile, corrected by setting unset byte to
                             * zero. Previously, zero meant no movement; now,
                             * it allows free flow. Tune on device. */

            .decay = 0, /* decay != 0 switches nibble meaning, affecting lava
                         * cell mass. Lava is first liquid heat source,
                         * causing potential conflict. Lava should be
                         * immortal, cooling by water, not time. */
        }),

    TWIN_ROW(MAT_ACID,
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
        }),

    TWIN_ROW(MAT_GLASS,
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
        }),

    TWIN_ROW(MAT_DIRT,
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
        }),

    TWIN_ROW(MAT_SNOW,
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
        }),

    /* NIBBLE 15's TWO ROWS - not a TWIN_ROW; different materials. Lower ROW
     * shared by all extended STATICs. Sweep reads per cell, per step,
     * ignoring STATIC. Sharing means all move identically, not at all,
     * nothing displaces them. Limitation: cannot have unique density, kind,
     * slip, repose, or scatter. */
    [MATERIAL_ROW(MAT_EXTENDED)] =
        {
            .name = "Extended", .kind = KIND_STATIC, .density = 200, /* stone's figure: undisplaceable, and it
                              * smothers a buried flame the way stone
                              * does */
        },

    /* GUNPOWDER's physics: Accessed via bit 3 in low nibble (GUNPOWDER_BASE,
     * material.h). KIND_POWDER for split. Density 50: below sand (60), dirt
     * (62), above liquids (30/38/45). Sinks in liquids, sand/dirt rest.
     * Powders don't sink through each other. Gaps visible under impulse,
     * blast sorts grit. Black powder lighter than quartz sand. Adjust
     * slip/repose/scatter as needed. See docs/Sand/Adding-a-Material.md for
     * density. */
    [MATERIAL_ROW(MAT_EXTENDED) + 1] =
        {
            .name = "Gunpowder",
            .kind = KIND_POWDER,
            .density = 50,
            .slip = 80,
            .repose = 8,
            .scatter = 30,
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
    /* Water's only reaction row, for one field. Water's effects on other
     * materials are driven from the other side, as are wetting actions
     * (`soaks`), which belong to sand and soil, leading to ambiguity. */
    [MAT_WATER] =
        {
            .wets = 1,

            /* Lets acid dissolve roll land on water using the same mechanism
             * as sand and wood. No material is eaten; water turns to acid
             * with a chance defined in sand.h. Interaction with water is as
             * easy as with ash, fitting for non-destructive behavior. Initial
             * setting; adjust as needed. */
            .dissolvable = 220,

            /* Low, deliberately: poured stream can occasionally outpace
             * evaporation over hot stone crust. Acid, below, is at the
             * opposite end. Started at 24, raised to ~11.3% per step due to
             * excessive resistance. Starting point, not final - tune on
             * device. */
            .boils = 29,
        },

    [MAT_ACID] =
        {
            /* The only dissolver. In 256, one bite every four steps, visibly
             * consuming sand. Each bite costs; unlimited consumption
             * impossible. A puddle exhausts a cell's budget. Sand/wood/stone
             * pay one-unit chip on miss, risking death
             * (SAND_ACID_EAT_DEATH_CHANCE, SAND_ACID_OIL_DEATH_CHANCE).
             * Water/acid dilution spends the entire cell
             * (SAND_ACID_DILUTE_TO_WATER_CHANCE). */
            .dissolves = 60,

            .fizz = 40, /* Roughly one visible puff every four steps with
                         * .dissolves. Reduced to 6 on 2026-09-01 due to
                         * excessive gas production. Raised to 40 after
                         * adjusting .evaporates for ambient gas. Adjust as
                         * needed. */

            .evaporates = 1, /* Rarest byte-wide roll: 1 in 256 per cell per
                              * step. Still too frequent in real puddles. See
                              * `step_one_dissolver_cell()` for extra gate
                              * (now 1-in-60, effective 1 in 15360).
                              * `sand_set_evaporates()` override bypasses
                              * gate. Tune on device. */

            /* Acid boiled via conducted heat unconditionally before this
             * field existed. 255 maintains that behaviour now boiling is
             * gated by a roll. */
            .boils = 255,

            /* Not water's MAT_STEAM default - acid evaporating through a
             * hot wall should leave the same MAT_GAS it leaves everywhere
             * else it evaporates (see .evaporates and .fizz above), not
             * kettle-steam. */
            .boils_to = MAT_GAS,
        },

    [MAT_OIL] =
        {
            /* Sparks ignite instantly without `needs_air`. Scan order spreads
             * ignition quickly. 50 in 256 means one catch every five steps
             * for adjacent flames. Adjust starting point as needed. */
            .flammability = 50,
            .needs_air = 1,
            .ignites_to = MAT_FIRE, /* burns straight to flame, unlike
                                     * wood: there is no log left to
                                     * smoulder, the fuel simply goes.
                                     * The flame rises off, exposing the
                                     * layer beneath, which is what eats
                                     * the pool downward */

            /* Acid's dissolve roll lands on oil, mostly boiling off as gas
             * (SAND_ACID_OIL_TO_GAS_CHANCE) and acid cell dying outright
             * (SAND_ACID_OIL_DEATH_CHANCE) - see comments in
             * step_one_dissolver_cell() (sand_reactions.c). Chance set at 16,
             * between previously tried 1 and 40 - tune as needed. */
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
            .heat_chance = 16, /* 16 in 256 per heat source per step.
                                * Deliberately slow for patient setup. 8 was
                                * too slow (52, 184 steps), 24 and up (17, 17
                                * steps) are too fast, 16 balances time (18,
                                * 137 steps). Tune on device. */
        },

    [MAT_GLASS] =
        {
            /* Glass and stone differ by acid resistance. Stone conducts heat
             * fully, glass does not. Initially, glass had no conduct value,
             * defaulting to 0, stopping heat transfer. Stone boiled contents
             * over flame, glass did not. Missing data implied no reactions,
             * incorrect for glass. */
            .conducts = 220,

            /* Glass banks heat via nibble variant, melting at ramp's top. See
             * `reaction_t.heat_ramp` in `material.h`. Sources add, speeding
             * heating in enclosed areas. Heat drains when sources extinguish,
             * making ramp mean duration, not total exposure. `cools` at half
             * ramp ensures visible draining. */
            .heats_to = MAT_LAVA,
            .heat_ramp = 64,

            /* In `step_one_tempered_cell()`, `cools` scales with temperature
             * to avoid a fixed drain rate. On a pane with a held source, lava
             * shatters in 12-26 steps and melts in 102-678, while fire
             * shatters in ~65 steps and never melts (peaks at 13). Fire
             * should fragilize but not melt glass; lava should do both. Flat
             * drain (12 vs 6) took 152 steps, felt ineffective. Fire's rise
             * means a single dab peaks around 6. Heat must be held for
             * effect. */
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
            /* Dirt's variant is MOISTURE when wet (1 to SOIL_MOISTURE_MAX),
             * TONE when dry. It soaks up liquid, raising MOISTURE, and dries
             * slowly at a thirtieth of the soaking rate, affecting SHADING.
             * Moisture is per-cell, percolating down. Damp banks shade via
             * wetness; dry ones use SOIL_DRY_TONES. A saturated bank takes
             * about 900 steps to dry at a rate of 2, showing a visible
             * gradient. */
            .tones = SOIL_DRY_TONES,       /* the table-driven codec's own
                              * copy of the fixed split above - see
                              * reaction_t.tones's own comment
                              * (material.h). MUST agree with
                              * SOIL_DRY_TONES/SOIL_MOISTURE_MAX, which is
                              * what test_dirt_moisture_macros_and_codec_
                              * helpers_agree_on_every_byte pins. */
            .moist_max = SOIL_MOISTURE_MAX,
            .soaks = 60,

            /* THE ONLY MATERIAL A PLANT TREATS AS GROUND - see
             * reaction_t.soil's own comment (material.h) for why this is a
             * separate field from `dries` rather than reusing it: gunpowder
             * also dries, but a fuse is not soil. */
            .soil = 1,
            .dries = 2,

            .dissolvable = 200, /* the same as sand: it is mostly sand */

            /* Dirt's variant is fully spent, no heat_ramp bank; memoryless
             * roll, 10, slower than sand (16). Wet stage spends roll driving
             * off moisture, 8 successes needed. */
        .heats_to    = MATX(MATX_METAL),
        .heat_chance = 10,

        .flaw_to     = MAT_STONE,
        .flaw_chance = 220,

        /* RUINED BY HASTE. 235 in 256 (~92%) per eligible roll, no longer
         * gated. Saturated dirt (SOIL_MOISTURE_MAX = 7) survives uncracked
         * with ~0.000003% chance. Watering ore before firing is practically
         * guaranteed to ruin it. Wet dirt reaching metal or stone should be
         * rare. */
        .spoils_to     = MAT_SAND,
        .spoils_chance = 235,
    },

    [MAT_SNOW] =
        {
            /* `chills` transfers heat from a hot neighbour, marking snow as
             * cold for thermal shock - 40 in 256 cools a pane without
             * cracking. Snow melts in the exchange; 120 ensures snow near
             * heat sources is short-lived. */
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

            /* Inverse of evaporation: rare 2x2 steam to droplet chance (1 in
             * 256), cosmetic, no real thermal model. Adjusted due to steam
             * decay reduction, now lingers ~4s. Tune as needed. */
            .condenses = 1,
            .condenses_to = MAT_WATER,
        },

    [MAT_SMOKE] =
        {
            .warms = 28, /* cooler than steam and far longer lived, so a
                        * lower rate spread over more steps */
        },

    [MAT_STONE] =
        {
            /* Stone mirrors glass's temperature properties: heat, frosting,
             * glowing, cold shock. Identical fields, scale, colors, for
             * consistency. Reads temperature only. Stone withstands heat,
             * vulnerable to acid. Glass acid-resistant, melts under heat,
             * cracks cooling hot. Stone melts, eliminating lava containers.
             * Rock spalls, not thermally shocked. Ramps at half glass rate,
             * more common, slower heat-up. */
            .heat_ramp = 32,
            .cools = 5,

            .dissolvable = 60, /* Stone now succumbs to acid slowly, under
                                * sand's 200 limit. MAT_GLASS, requiring
                                * creation, takes over as acid container.
                                * Glass immunity arises from lack of
                                * `dissolvable`, aligning with other
                                * materials. */

            /* 220 in 256 (~0.86) chance heat crosses stone cell. See
             * `conduct_heat()` in `sand_reactions.c`. Probability drops with
             * depth as 0.86^d, making thin walls faster, thick ones slower.
             * Previously, at 176 (~0.69), basins took over a minute for
             * steam. Now, at 0.86, any basin boils quickly. Thickness still
             * matters. Adjust as needed. */
            .conducts = 220,
        },

    [MAT_GAS] =
        {
            /* gas catches fire on contact, costing no RNG draw; keeps tests
             * and frame-budget captures identical */
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

            .residue = 40, /* 1 in 256 chance a burnt-out fire cell leaves
                            * MAT_STEAM behind. Lower than ember's 90%.
                            * Starting point; tune as needed. */

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
            /* Wood standing in wet ground buds FOLIAGE, one bud every forty
             * steps per cell of trunk touching wet soil, only while watered.
             * Used to bud a PLANT, causing thin green threads. Foliage,
             * unable to grow or fall, is correct. It removes a grower from
             * the loop. */
            .sprouts = 6,
            .sprouts_to = MATX(MATX_LEAF),

            /* And a crowned trunk puts out new GROWTH, which is where a tree
         * gets taller now that hardening leaves no green tip behind.
         * Rarer than leafing: a bud is a whole new limb rather than a
         * frond, and it is the only thing that compounds, so it is the
         * number to turn down first if a forest gets away. */
            .buds = 32,
            .buds_to = MATX(MATX_PLANT),

            /* Budding and sprouting use soil moisture; see reaction_t.roots
             * in material.h. PART 1 plants the first root (root_depth == 0).
             * PART 2, step_one_rooting_cell() in sand_reactions.c, continues
             * growth. This triggers once per tree, measuring initial root
             * formation and retaining 40 from the original design. */
            .roots = 40,
            .roots_to = MATX(MATX_ROOT),

            /* A trunk standing in water waters its own roots, at a third of
         * green growth's rate - bark is not a leaf. */
            .drinks = 12,

            /* 6 in 256 is roughly 43 steps of contact with a single flame
             * before it catches, reflecting "slowly consumed". Wood does not
             * ignite directly to MAT_FIRE like gas (see sand_reactions.c).
             * Starting point; tune on device. */
            .flammability = 6,

            /* Ignites into ITSELF. Wood's variant tracks remaining burnable.
             * Ember was a separate material with 7 fields, now decay handled
             * by variant. State with own row costs a slot. */
            .ignites_to = MAT_WOOD,

            /* 24 in 256 per step across 15 levels is ~160 steps, about 2.7s at
         * this app's step rate - a log that visibly smoulders rather than
         * one that either lingers forever or guts out at once. Ember's own
         * figure, kept: the burn did not change, only where it lives. */
            .burn_decay = 24,

            /* Variant 0 is unlit, 1..15 lit - explicitly stated for
             * cell_is_burning()/tick_decay_at() in material.h/sand_priv.h.
             * Similar to gunpowder's row stating 7. */
            .lit_from = 1,

            .residue = 90, /* well above fire's 40: a whole log
                                   * finishing its burn is a bigger, more
                                   * definite event than a flame guttering
                                   * out, and should leave smoke far more
                                   * often */

            .quench_to = 0, /* Water extinguishes fire, replacing it with
                             * steam; step_one_burning_cell() does this for
                             * burn_decay material. Ember named MAT_STEAM as
                             * the fire itself was extinguished. */

            .flare = 48, /* 1/256 chance per step for a burning log to emit a
                          * MAT_FIRE cell into an empty cardinal neighbour.
                          * Wood is KIND_STATIC, otherwise it would be a
                          * glowing brick without flame. Flame rises via
                          * sand_step_gas(), creating "wood burning below,
                          * flame above" */

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

/* The same blend, out of 255 rather than 15 - glass's live gravity
 * gradient (MAT_GLASS case below) needs a small tilt to move the shade a
 * small amount, finer than the sixteen steps the rest of the palette is
 * built from once, at compile time, and never touches again. */
#define LERP8_CH(lo, hi, shift, fr)                                                                                    \
    ((((((lo) >> (shift)) & 0xFF) * (255 - (fr)) + (((hi) >> (shift)) & 0xFF) * (fr)) / 255) & 0xFF)

#define LERP8(lo, hi, fr) ((LERP8_CH(lo, hi, 16, fr) << 16) | (LERP8_CH(lo, hi, 8, fr) << 8) | LERP8_CH(lo, hi, 0, fr))

/* A ramp for `n` steps between two colours, for a material that needs its
 * sixteen entries built in more than one piece. */
#define SEG(lo, hi, i, n) GFX_RGB(LERP(lo, hi, ((i) * 15) / ((n) - 1)))

/* Temperature scale with room temperature in the middle, chilled panes
 * frosted below SAND_AMBIENT_HEAT, glowing above SAND_SHOCK_HEAT. Wood: unlit
 * timber at zero, burning from 1 to 15. Dirt: luminance ramp from 0 to
 * SOIL_DRY_TONES - 1 + SOIL_MOISTURE_MAX (14), wet soil darker than dry. */
#define DIRT_DRY 0x9A7B52
#define DIRT_WET 0x3A2A18

/* 15 steps, variants 0-14 (SOIL_MOISTURE_MAX), DIRT_DRY to DIRT_WET range;
 * variant 15 unused, repeats variant 14's colour. `i * 15 / 14` truncates to
 * `i` for i < 14, creating an identity ramp with a final jump. */
#define SOIL_SHADES                                                                                                   \
    GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 0)), GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 1)),                                       \
        GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 2)), GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 3)),                                   \
        GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 4)), GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 5)),                                   \
        GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 6)), GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 7)),                                   \
        GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 8)), GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 9)),                                   \
        GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 10)), GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 11)),                                 \
        GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 12)), GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 13)),                                 \
        GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 15)), /* variant 14: saturated */                                            \
        GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 15))  /* variant 15: unused - same as 14 */

#define WOOD_UNLIT   0x5A3D24
#define WOOD_CHAR    0x2A0A00
#define WOOD_GLOW    0xFF7A28

#define WOOD_BURN(i) GFX_RGB(LERP(WOOD_CHAR, WOOD_GLOW, ((i) - 1) * 15 / 14))

#define WOOD_SHADES                                                                                                    \
    GFX_RGB(WOOD_UNLIT), WOOD_BURN(1), WOOD_BURN(2), WOOD_BURN(3), WOOD_BURN(4), WOOD_BURN(5), WOOD_BURN(6),           \
        WOOD_BURN(7), WOOD_BURN(8), WOOD_BURN(9), WOOD_BURN(10), WOOD_BURN(11), WOOD_BURN(12), WOOD_BURN(13),          \
        WOOD_BURN(14), WOOD_BURN(15)

/* Stone's ramp mirrors glass's, using constants for splits. Stone loses
 * per-cell random shade, which dither in material_dither() partially
 * replaces. Hot walls appear visibly hot now. */
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
#define GLASS_NEUTRAL 0x8C7E70
#define GLASS_GLOW    0xC8701E
#define GLASS_MOLTEN  0xFFD873

/* Was 0x7E8E86 - G the highest channel, a sage green between AMBIENT's
 * blue and GLOW's orange. STONE_NEUTRAL crosses that gap R-dominant
 * instead (see MAT_GLASS's own reaction row above - "differ in ONE
 * thing" - for why the two heat ramps were meant to track each other). */

/* Reported as glass turning green under heat once the live gravity blend
 * made this endpoint far more visible than the old per-cell wobble ever
 * did. R-dominant now, the same shape stone's own transition uses. */

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

#define SAND_DUNE_RAMP                                                                                                 \
    GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 0)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 1)),                                    \
        GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 2)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 4)),                                \
        GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 5)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 6)),                                \
        GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 8)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 9)),                                \
        GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 10)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 12)),                              \
        GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 13)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 15))

/* CULLET uses four anchors in a 16-step cycle, spaced quarter-turns apart,
 * matching SAND_CULLET_BASE's band. These anchors create a pale, cool effect
 * simulating ground glass. A saturated cycle was rejected. The GLINT is a
 * rare pure white flash. */
#define CULLET_CYCLE_A 0xCFEAF2 /* pale cyan */
#define CULLET_CYCLE_B 0xD8D0F0 /* lilac */
#define CULLET_CYCLE_C 0xF0D6DC /* rose */
#define CULLET_CYCLE_D 0xE2F0D2 /* mint */

/* What a glinting grain shows: full white, the panel's highest radiance. */
#define CULLET_GLINT GFX_RGB(0xFFFFFF)

/* Odds of glinting roll (MAT_SAND) per cell per phase step - about one in
 * 192. Started at 64, reduced to a third. Modulo used for flexibility, not
 * limited to powers of two. One division per CULLET cell, ordinary sand never
 * reaches this. Lower value = more glints. */
#define CULLET_GLINT_ONE_IN 192

#define SAND_CULLET_RAMP                                                                                               \
    GFX_RGB(CULLET_CYCLE_A), GFX_RGB(CULLET_CYCLE_B), GFX_RGB(CULLET_CYCLE_C), GFX_RGB(CULLET_CYCLE_D)

/* One loop A -> B -> C -> D -> A with four LERP steps per segment (t = 0, 4,
 * 8, 12 of 15). Stepping by CULLET_CYCLE_LEN / SAND_CULLET_SHADES (4) lands
 * on the next anchor. Define CULLET_CYCLE_LEN in material.h for host tests.
 * Array safety depends on these properties. */
_Static_assert((CULLET_CYCLE_LEN & (CULLET_CYCLE_LEN - 1)) == 0,
               "the cycle wraps with a mask below - it has to stay a power of two");
_Static_assert(CULLET_CYCLE_LEN % SAND_CULLET_SHADES == 0,
               "each cullet shade needs to land on an exact quarter-turn of the cycle");

#define CULLET_PALE(lo, hi, t) GFX_RGB(LERP(lo, hi, t))

static const gfx_color_t cullet_cycle[CULLET_CYCLE_LEN] = {
    CULLET_PALE(CULLET_CYCLE_A, CULLET_CYCLE_B, 0), CULLET_PALE(CULLET_CYCLE_A, CULLET_CYCLE_B, 4),
    CULLET_PALE(CULLET_CYCLE_A, CULLET_CYCLE_B, 8), CULLET_PALE(CULLET_CYCLE_A, CULLET_CYCLE_B, 12),
    CULLET_PALE(CULLET_CYCLE_B, CULLET_CYCLE_C, 0), CULLET_PALE(CULLET_CYCLE_B, CULLET_CYCLE_C, 4),
    CULLET_PALE(CULLET_CYCLE_B, CULLET_CYCLE_C, 8), CULLET_PALE(CULLET_CYCLE_B, CULLET_CYCLE_C, 12),
    CULLET_PALE(CULLET_CYCLE_C, CULLET_CYCLE_D, 0), CULLET_PALE(CULLET_CYCLE_C, CULLET_CYCLE_D, 4),
    CULLET_PALE(CULLET_CYCLE_C, CULLET_CYCLE_D, 8), CULLET_PALE(CULLET_CYCLE_C, CULLET_CYCLE_D, 12),
    CULLET_PALE(CULLET_CYCLE_D, CULLET_CYCLE_A, 0), CULLET_PALE(CULLET_CYCLE_D, CULLET_CYCLE_A, 4),
    CULLET_PALE(CULLET_CYCLE_D, CULLET_CYCLE_A, 8), CULLET_PALE(CULLET_CYCLE_D, CULLET_CYCLE_A, 12),
};

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
        /* Twelve DUNE shades and four CULLET shades. Cullet leaves ramp for
         * cool desaturated cast. Phase-0 rest look; live frames use
         * material_colours()'s MAT_SAND case. Both use same anchors. */
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
    [MAT_STEAM * MATERIAL_VARIANTS] = SHADES(0x6E8496, 0xF2FAFF), /* steam
                                                                   * variant:
                                                                   * life
                                                                   * remaining,
                                                                   * cool
                                                                   * blue-grey
                                                                   * (dying),
                                                                   * almost
                                                                   * white
                                                                   * (fresh);
                                                                   * COOL and
                                                                   * BRIGHT,
                                                                   * contrasting
                                                                   * with
                                                                   * smoke's
                                                                   * warm and
                                                                   * dim; two
                                                                   * materials
                                                                   * needed
                                                                   * for quick
                                                                   * visual
                                                                   * distinction
                                                                   * at two
                                                                   * pixels
                                                                   * per cell. */
    [MAT_SMOKE * MATERIAL_VARIANTS] = SHADES(0x2A2622, 0x857A6E), /* smoke -
                                                                   * dying
                                                                   * wisp
                                                                   * near-black
                                                                   * soot,
                                                                   * fresh
                                                                   * warm mid
                                                                   * grey-brown.
                                                                   * Bright
                                                                   * end held
                                                                   * down.
                                                                   * Measured:
                                                                   * steam 89
                                                                   * lum
                                                                   * brighter
                                                                   * than
                                                                   * smoke at
                                                                   * equal
                                                                   * life.
                                                                   * FRESHEST
                                                                   * smoke
                                                                   * (122)
                                                                   * dimmer
                                                                   * than most
                                                                   * nearly-dead
                                                                   * steam
                                                                   * (132). No
                                                                   * overlap
                                                                   * ensures
                                                                   * clear
                                                                   * distinction. */
    [MAT_OIL * MATERIAL_VARIANTS] = SHADES(0x6E5A22, 0x14100A),   /* oil -
                                                                   * variant
                                                                   * is FILL
                                                                   * LEVEL,
                                                                   * like
                                                                   * water:
                                                                   * thin film
                                                                   * is murky
                                                                   * olive,
                                                                   * deep pool
                                                                   * nearly
                                                                   * black.
                                                                   * Dark,
                                                                   * warm,
                                                                   * contrasts
                                                                   * water's
                                                                   * pale
                                                                   * blue,
                                                                   * making
                                                                   * floating
                                                                   * slick
                                                                   * unmistakable. */
    [MAT_LAVA * MATERIAL_VARIANTS] = SHADES(0xFFC24A, 0x8A1400),  /* lava  - fill level again, and
                                    * deliberately INVERTED against
                                    * fire's own ramp: a thin skim is
                                    * bright yellow and a deep pool is
                                    * dark red, so depth reads as
                                    * cooling crust rather than as more
                                    * heat. Keeps a lava pool visually
                                    * distinct from the flames it
                                    * flares */
    [MAT_ACID * MATERIAL_VARIANTS] = SHADES(0xEAFF3C, 0x2E6B0A),  /* acid -
                                                                   * variant
                                                                   * FILL
                                                                   * LEVEL,
                                                                   * similar
                                                                   * to water:
                                                                   * thin film
                                                                   * is vivid
                                                                   * lime,
                                                                   * deep pool
                                                                   * is dark
                                                                   * olive.
                                                                   * Saturated
                                                                   * and
                                                                   * yellow-leaning
                                                                   * to avoid
                                                                   * gas's
                                                                   * pale
                                                                   * green.
                                                                   * Adjacent
                                                                   * densities
                                                                   * never
                                                                   * overlap
                                                                   * on
                                                                   * screen. */
    [MAT_GLASS * MATERIAL_VARIANTS] = GLASS_SHADES,               /* glass -
                                                                   * not a
                                                                   * shade
                                                                   * ramp.
                                                                   * Heat
                                                                   * variant
                                                                   * in
                                                                   * material.h.
                                                                   * This ramp
                                                                   * is a
                                                                   * temperature
                                                                   * scale;
                                                                   * heating
                                                                   * pane
                                                                   * glows
                                                                   * along it.
                                                                   * Cool teal
                                                                   * at rest
                                                                   * to
                                                                   * 0xFFC24A
                                                                   * (lava's
                                                                   * brightest),
                                                                   * seamless
                                                                   * transition. */
    [MAT_DIRT * MATERIAL_VARIANTS] =
        /* dirt - ONE ramp for the nibble, dry tones to moisture, using STATE
         * for variant. Dusty tan to dark damp earth, strictly darkening,
         * aligning number and colour. Soil's shading varies with state
         * (moisture or tone) instead of one bit. */
    SOIL_SHADES,
    [MAT_SNOW * MATERIAL_VARIANTS] = SHADES(0xC6D8E4, 0xFFFFFF), /* snow  - a powder, so a shade ramp
                                    * again, and a narrow one: cold blue
                                    * white to plain white. Deliberately
                                    * the palest thing on the board, since
                                    * it has to read as COLD at a glance
                                    * for thermal shock to explain itself */
    /* EXTENDED RANGE: 16 codes, 16 colours, palette unchanged. STATIC has no
     * variants, gunpowder uses MATERIAL_FLAT. Named entries avoid miscount
     * shifts, duplicates cause errors. Magenta tail (3 entries) is
     * load-bearing padding, ensuring non-zero codes to prevent BLACK
     * rendering. */
    [MAT_EXTENDED * MATERIAL_VARIANTS + MATX_ICE] = GFX_RGB(0xB6E4F2),      /* ice - paler and bluer than snow's
                                    * white, and flat rather than speckled:
                                    * a block of it should read as solid
                                    * and cold, where snow reads as loose */
    [MAT_EXTENDED * MATERIAL_VARIANTS + MATX_PLANT] = GFX_RGB(0x55672D),    /* plant
                                                                             * -
                                                                             * OLIVE,
                                                                             * stem
                                                                             * timber
                                                                             * in
                                                                             * progress:
                                                                             * used
                                                                             * to
                                                                             * be
                                                                             * vivid
                                                                             * green,
                                                                             * now
                                                                             * wood's
                                                                             * brown;
                                                                             * leaves
                                                                             * remain
                                                                             * green,
                                                                             * catching
                                                                             * eye */
    [MAT_EXTENDED * MATERIAL_VARIANTS + MATX_LEAF] = GFX_RGB(0x69B03A),     /* leaf - the one green left in a tree
                                    * now that the stem is olive, and the
                                    * only part meant to catch the eye.
                                    * only part meant to catch the eye */
    [MAT_EXTENDED * MATERIAL_VARIANTS + MATX_METAL] = GFX_RGB(0x7C8794),    /* metal
                                                                             * -
                                                                             * cool
                                                                             * blue-grey,
                                                                             * brighter
                                                                             * than
                                                                             * ambient
                                                                             * stone
                                                                             * (0x5F6673)
                                                                             * and
                                                                             * greyer
                                                                             * than
                                                                             * ice's
                                                                             * 0xB6E4F2.
                                                                             * Guess,
                                                                             * not
                                                                             * measured,
                                                                             * for
                                                                             * visual
                                                                             * separation.
                                                                             * See
                                                                             * docs/Sand/Metal-Smelting-Plan.md. */
    [MAT_EXTENDED * MATERIAL_VARIANTS + MATX_ROOT] = GFX_RGB(0xBFA58A),     /* root
                                                                             * -
                                                                             * PALE,
                                                                             * WARM
                                                                             * TAN,
                                                                             * ~170
                                                                             * lum,
                                                                             * avoids
                                                                             * dirt's
                                                                             * ~45-127
                                                                             * range,
                                                                             * matches
                                                                             * trunk,
                                                                             * warmer
                                                                             * than
                                                                             * bone
                                                                             * but
                                                                             * cooler
                                                                             * than
                                                                             * sand. */
    [MAT_EXTENDED * MATERIAL_VARIANTS + MATX_ROOT + 1] = GFX_RGB(0xFF00FF), /* spare static, unclaimed */
    GFX_RGB(0xFF00FF),                                                      /* spare static, unclaimed */
    GFX_RGB(0xFF00FF),                                                      /* spare static, unclaimed */

    /* GUNPOWDER, flat colour using MATERIAL_FLAT. Three dry tones: unlit
     * charcoal to dull brick red. Four moisture levels: darkens from
     * black-red 0x2B1410 to wet blue, avoiding overlap. */
    [GUNPOWDER_CELL(0)] = GFX_RGB(0x141014), /* dry, tone 0 - near-black */
    [GUNPOWDER_CELL(1)] = GFX_RGB(0x2B1410), /* dry, tone 1 - black-red */
    [GUNPOWDER_CELL(2)] = GFX_RGB(0x46160F), /* dry, tone 2 - dark red;
                              * the brush paints this one (brush_color(),
                              * app_sand.c) because it is the only one of
                              * the three that reads on the panel at all -
                              * the other two are close enough to the
                              * background to disappear */
    [GUNPOWDER_CELL(3)] = GFX_RGB(0x251210), /* moisture 1 */
    [GUNPOWDER_CELL(4)] = GFX_RGB(0x1F1011), /* moisture 2 */
    [GUNPOWDER_CELL(5)] = GFX_RGB(0x180E11), /* moisture 3 */
    [GUNPOWDER_CELL(6)] = GFX_RGB(0x120C12), /* moisture 4 */
    [GUNPOWDER_CELL(7)] = GFX_RGB(0xFF8C2A), /* LIT (GUNPOWDER_LIT) - the
                              * fuse itself, a hot ember orange near fire's
                              * bright end, so a burning trail reads as
                              * burning. Was a fifth moisture level before
                              * REVISION 2 spent this code on the lit state */
};

/* The same ramps pulled two thirds of the way back to their own ambient
 * colour, used wherever a cell touches empty space. Ten of fifteen, so an
 * outline still shifts with heat - just a third as far as the body does. */
#define GLASS_RGB(v)      ((v) <= SAND_AMBIENT_HEAT ? GLASS_COOL(v) : (v) < SAND_SHOCK_HEAT ? GLASS_WARM(v) : GLASS_HOT(v))

/* Blends v ITSELF toward ambient before choosing a colour, not the two
 * ENDPOINT colours after the fact the way STONE_EDGE_RGB still does. */

/* Averaging a HOT cell's saturated orange with ambient's saturated blue
 * in raw RGB space lands on green - a hue with nothing to do with
 * either heat or ambient. */

/* Blending the TEMPERATURE first keeps every edge colour a real point
 * on glass's own COOL/WARM/HOT ramp instead. Stone's own endpoints stay
 * close enough in hue that mixing its raw colours never hits this. */
#define GLASS_EDGE_V_RAW(v) ((v) + (((int)(SAND_AMBIENT_HEAT) - (int)(v)) * 10) / 15)

/* Still crosses ONE boundary on its own: a cell only just past
 * SAND_SHOCK_HEAT dampens down to 4, back into WARM's own blue-grey -
 * read as the pane cooling off rather than merely dimming its glow. */

/* Clamped to SAND_SHOCK_HEAT whenever the real v is already HOT, so a
 * cell that has genuinely started glowing never shows a cooler band's
 * colour at all. */
#define GLASS_EDGE_V(v)                                                                                               \
    (((v) >= SAND_SHOCK_HEAT && GLASS_EDGE_V_RAW(v) < SAND_SHOCK_HEAT) ? SAND_SHOCK_HEAT : GLASS_EDGE_V_RAW(v))
#define GLASS_EDGE_RGB(v) GLASS_RGB(GLASS_EDGE_V(v))
#define STONE_EDGE_RGB(v) LERP(STONE_RGB(v), STONE_RGB(SAND_AMBIENT_HEAT), 10)

/* The far end of the live gravity gradient - GLASS_FROST only through
 * COOL. Blending a WARM or HOT orange all the way to that icy blue passed
 * through a muddy yellow-green, reading as the glass turning green rather
 * than heat catching the light; white keeps the hue and adds brightness. */
#define GLASS_GRADIENT_HI(v) ((v) <= SAND_AMBIENT_HEAT ? GLASS_FROST : 0xFFFFFF)

/* Stone's SPECKLE: eight shades per cell based on position, spreading both
 * ways. Old version only darkened, causing walls to look murkier. Old shade
 * ramp 0x4A4F5A to 0x767D8C, now uses position for stability, four levels
 * instead of sixteen, overlaying texture on temperature. */
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

/* Wood's grain like stone's speckle: UNLIT wood is speckled; burning logs
 * glow uniformly to avoid cell-to-cell variation appearing dirty. */
#define WOOD_GRAIN(k) GFX_RGB(LERP(LERP(WOOD_UNLIT, 0x000000, 3), LERP(WOOD_UNLIT, 0xFFFFFF, 2), (k) * 15 / 7))

/* Dirt lacks a grain table. Screen-position hashing like stone and wood
 * caused issues: dirt movement made texture repeat. Now, tone is in the cell,
 * with palette drawing it. */

static const gfx_color_t wood_grain[8] = {
    WOOD_GRAIN(0), WOOD_GRAIN(1), WOOD_GRAIN(2), WOOD_GRAIN(3),
    WOOD_GRAIN(4), WOOD_GRAIN(5), WOOD_GRAIN(6), WOOD_GRAIN(7),
};

/* Extended materials speckled, no shade. Ice, tree facets. Stem OLIVE, 55% to
 * wood 0x5A3D24. Brightness issue fixed: stem 75-123, wood 67. Stem beside
 * trunk, leaves bright. */
#define PLANT_DARK        0x495422
#define PLANT_LIGHT       0x778746

/* Foliage: lighter, yellower, wider than stem. Sunlit on one side, shaded on
 * the other. Non-uniform green creates half the canopy. Now one material.
 * Leaves sheltered, drink through trunk, wither after both. Gold-brown chain
 * removed for cost, activates only at senescence. */
#define LEAF_DARK         0x468F26
#define LEAF_LIGHT        0x8CD24E

#define ICE_DARK          0x93C9DE
#define ICE_LIGHT         0xDEF5FD

/* Metal: Like ice, it has minimal movement; position hash is its only
 * variation. Narrower spread than ice, simulating light on a uniform surface. */
#define METAL_DARK        0x7C8794
#define METAL_LIGHT       0xB9C4D2

/* Roots use narrow speckle like ice and metal for fixed texture. ROOT_DARK is
 * pale, not dark; ROOT_LIGHT matches METAL_LIGHT for visual consistency. */
#define ROOT_DARK         0xBFA58A
#define ROOT_LIGHT        0xDCC5A8

/* Roots grow deep, not old. Neighbor count shades material_colours(). Tips
 * stay fresh, darken with child growth. Losing a child lightens the parent.
 * ROOT_OLD shifts to WOOD_UNLIT. RGB in ROOT_SHADES transitions smoothly.
 * Both ends revert to old hues. ROOT_OLD_LIGHT remains lighter than ROOT_OLD. */
#define ROOT_OLD          0x7A5535
#define ROOT_OLD_LIGHT    0x976D48
#define ROOT_SHADES       4

#define ROOT_STEP(k)       LERP(ROOT_DARK, ROOT_OLD, (k) * 15 / (ROOT_SHADES - 1))
#define ROOT_STEP_LIGHT(k) LERP(ROOT_LIGHT, ROOT_OLD_LIGHT, (k) * 15 / (ROOT_SHADES - 1))

/* Which of the ROOT_SHADES steps a root with `n` root neighbours wears.
 * 0 or 1 is a tip (or a lone seed), 2 has one child, 3 is a junction, and
 * anything more is the collar or the middle of a thicket. */
static inline unsigned root_shade(unsigned n)
{
    return n <= 1u ? 0u : n == 2u ? 1u : n == 3u ? 2u : (ROOT_SHADES - 1u);
}

#define GRAIN8(lo, hi, k) GFX_RGB(LERP((lo), (hi), (k) * 15 / 7))

#define GRAIN8_ROW(lo, hi)                                                                                             \
    {GRAIN8(lo, hi, 0), GRAIN8(lo, hi, 1), GRAIN8(lo, hi, 2), GRAIN8(lo, hi, 3),                                       \
     GRAIN8(lo, hi, 4), GRAIN8(lo, hi, 5), GRAIN8(lo, hi, 6), GRAIN8(lo, hi, 7)}

static const gfx_color_t plant_grain[8] = GRAIN8_ROW(PLANT_DARK, PLANT_LIGHT);
static const gfx_color_t ice_grain[8] = GRAIN8_ROW(ICE_DARK, ICE_LIGHT);
static const gfx_color_t leaf_grain[8] = GRAIN8_ROW(LEAF_DARK, LEAF_LIGHT);
static const gfx_color_t metal_grain[8] = GRAIN8_ROW(METAL_DARK, METAL_LIGHT);

/* Metal's own travelling shine, HATCHED's one surviving effect now that
 * the woven diagonal line under it is gone (app_sand.c's paint_row_n()) -
 * lifted off METAL_LIGHT rather than metal_grain's per-cell wobble, so
 * the highlight reads as one reflective surface, not chewed. */
static const gfx_color_t metal_shine = GFX_RGB(LERP(METAL_LIGHT, 0xFFFFFF, 11));
/* One grain row per shade step, fresh first - see ROOT_OLD above. */
static const gfx_color_t root_grain[ROOT_SHADES][8] = {
    GRAIN8_ROW(ROOT_STEP(0), ROOT_STEP_LIGHT(0)),
    GRAIN8_ROW(ROOT_STEP(1), ROOT_STEP_LIGHT(1)),
    GRAIN8_ROW(ROOT_STEP(2), ROOT_STEP_LIGHT(2)),
    GRAIN8_ROW(ROOT_STEP(3), ROOT_STEP_LIGHT(3)),
};
_Static_assert(ROOT_SHADES == 4, "root_grain[] above spells out one row per shade - add a row here too");

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

/* How far liquid_spec's shift reaches in the 16-level fill ramp. Tuned by eye
 * on device. If rim's highlight is too strong or faint, adjust this value.
 * 10, up from 6: still too subtle after interior depth gradient. On water's
 * ramp, changes up-face-to-down-face range from 49 to 81 luminance. */
#define SPEC_STRENGTH 10

/* Rounds n/d to nearest integer for n of either sign and d > 0. Plain integer
 * division truncates towards zero, which is fine for ramps but incorrect
 * here: specular term that rounds -0.5 to 0 is weaker on one side. */
static int
fx_round_div(int n, int d) {
    if (n >= 0) {
        return (n + d / 2) / d;
    }
    return -((-n + d / 2) / d);
}

/* Fills `liquid_spec[]` using frame gravity. For `MATERIAL_EDGE_*`,
 * calculates outward normal `n` from empty sides. If no or opposite sides are
 * empty, `n` is (0, 0); if one side is empty, `n` is a unit axis; if two
 * adjacent sides are empty, `n` is a sqrt(2) diagonal. `norm_q8` normalises
 * `n` based on nonzero components. Specular value is the normalised dot
 * product of `n` with MINUS gravity, scaled by 256. Removes depth/wave walk
 * derivation. */
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

        /* The ramp runs bright-to-dark as fill rises, so a positive specular
         * term (facing away from gravity, wants to be BRIGHTER) subtracts
         * from the index. Getting this wrong inverts the effect; see
         * test_a_liquid_rim_catches_the_light_from_above in
         * suite_sand_liquid_depth.c. */
        liquid_spec[mask] = (int8_t)(-fx_round_div(spec_q8 * SPEC_STRENGTH, 256));
    }
}

/* See material.h for function purpose. Starts from MINUS-gravity vector from
 * material_set_gravity(), turns LEFT by an eighth. 45-degree slant wanted.
 * Screen y grows downward, so LEFT turn is (x, y) -> (x + y, y - x) over sqrt
 * 2, using 181/256 for 1/sqrt 2. */
void
material_shine_direction(int gx, int gy, int *ux_q8, int *uy_q8) {
    const int len = im_len(gx, gy);
    if (len == 0) {
        *ux_q8 = 181;
        *uy_q8 = 181;
        return;
    }
    *ux_q8 = (-(gx + gy) * 181) / len;
    *uy_q8 = ((gx - gy) * 181) / len;
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
 * test_foam_never_stalls_between_frames in suite_sand_foam.c, which pins BOTH
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

/* FOAM's own colour - a side table, not a palette[] row, using
 * stone_speckle's pattern. No spare palette[] slot; dither over rim colour
 * suffices. Brighter and whiter than water's palest ramp entry (0x77C4E8) to
 * appear on water, not as palest water. */
static const gfx_color_t water_foam = GFX_RGB(0xE8F6FF);

/* Curvature threshold for foam density, max 5 (empty_count 8) to prevent
 * corners from appearing more foamed than jagged crevices. */
#define WATER_FOAM_CURVATURE_MAX 3

/* Foam density by curvature as threshold against `hash` and frame's foam
 * phase: cell foams if masked mix < threshold. DITHER, not fill. Tuned by
 * eye. Adjust if foam is sparse (raise) or busy (lower). Named table for easy
 * tuning. Curvature 0 is 0 (never foams) for flat rim logic. */
static const uint8_t water_foam_threshold[WATER_FOAM_CURVATURE_MAX + 1] = {
    0, /* curvature 0, flat   - no foam at all */
    3, /* curvature 1, light  - foams on 3 of 8 hash values */
    5, /* curvature 2, medium - foams on 5 of 8 */
    7, /* curvature 3+, heavy - foams on 7 of 8 */
};

/* THIS FRAME'S FOAM PHASE - see material_set_foam_phase() in material.h. Zero
 * until first frame sets it, painting the stable dither foam had before phase
 * existed. */
static unsigned foam_phase;

void
material_set_foam_phase(unsigned phase) {
    foam_phase = phase;
}

/* THIS FRAME'S CULLET PHASE - see material_set_cullet_phase() in material.h.
 * Cullet runs on its own clock (CULLET_PHASE_MS, app_sand.c). Starts at zero,
 * setting cullet shades at anchor colors (cullet_cycle[0], [4], [8], [12] -
 * phase 0). Other phases use static SAND_CULLET_RAMP entries. */
static unsigned cullet_phase;

void
material_set_cullet_phase(unsigned phase) {
    cullet_phase = phase;
}

/* THIS FRAME'S GLASS PHASE - see material_set_glass_phase()'s own comment
 * in material.h for what drives it (a live gravity snapshot, not a
 * clock). Zero until the first frame sets it - a reasonable default for
 * anything reading material_colours() before a frame ever runs. */
static int glass_phase;

void
material_set_glass_phase(int phase) {
    glass_phase = phase;
}

/* Counts `mask` bits using suite_icons.c's popcount16() method, kept locally
 * due to differing widths. No floating point, suitable for water rim cells
 * only. See paint_row_n() in app_sand.c for gating. */
static unsigned
material_popcount8(unsigned mask) {
    unsigned count = 0;
    for (unsigned bit = 0; bit < 8u; bit++) {
        count += (mask >> bit) & 1u;
    }
    return count;
}

/* Depth gradient may brighten the shallowest cell by up to 16 steps, relative
 * to the body colour at the deepest. Index 4 adjusts gradient strength, from
 * 54 to 85 luminance, avoiding the rim. Adjust if gradient is too strong
 * (lower) or too subtle (raise). */
#define DEPTH_RANGE          4

/* Depth for full darkening clamped at 24, matching oil, lava, acid. Water
 * used sum-of-sines wave table and fog blend, removed due to flat appearance
 * and rigid columns. /255 divide caused fog bug, now fixed for all liquids.
 * Shared with sand_liquid.c as MATERIAL_LIQUID_DEPTH_BAND. */
#define DEPTH_SATURATE_CELLS MATERIAL_LIQUID_DEPTH_BAND

material_pattern_t
material_colours(cell_t c, unsigned hash, unsigned mask, unsigned depth, gfx_color_t out[3]) {
    const uint8_t v = CELL_VARIANT(c);

    /* A liquid cell's appearance differs by location: rim or body. Interior
     * cells fill fully to mask artefacts like the comb effect, where mid-body
     * cells alternate fills. `depth` measures local depth, not screen
     * position, affecting color brightness. This fixes previous fog blending
     * and wave band issues. Interior cells use `depth` for shading, while
     * rims maintain fill levels for precise rendering. Water rims add foam
     * based on curvature, and `hash` is coarsened to cluster foam in 8x8
     * blocks. */
    if (material_of(c)->kind == KIND_LIQUID) {
        const uint8_t id = CELL_MATERIAL(c);
        const unsigned cardinal = mask & MATERIAL_EDGE_CARDINAL;

        if (cardinal == 0) {
            /* SAME SHIFT FOR EVERY LIQUID, WATER INCLUDED. `depth` clamped to
             * DEPTH_SATURATE_CELLS to prevent unsigned wrap-around when depth
             * exceeds DEPTH_SATURATE_CELLS. */
            const unsigned depth_capped = depth < DEPTH_SATURATE_CELLS ? depth : DEPTH_SATURATE_CELLS;

            /* Distance-to-saturation, in DEPTH_RANGE shade steps - see
             * DEPTH_SATURATE_CELLS comment. Plain integer arithmetic, no
             * fixed-point scaling: no wave residual to preserve. */
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

                /* ADD, not `^` - XOR fails as water_foam_threshold's windows
                 * are power-of-two aligned, causing roughly half of phase
                 * steps to leave the foaming set unchanged. Addition rotates
                 * the window by one, preventing matches. 0x9E37u ensures all
                 * low-bit values are stepped through as phase increases. */
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
        case MAT_SAND: {
            /* ONE compare added for MAT_EXTENDED (14%/26% inlining cliff for
             * RESTRUCTURE). Sand is common, so this compare is first,
             * possibly doing nothing else. Below SAND_CULLET_BASE, breaks to
             * flat palette. At or above, `v` uses cullet_cycle[] in
             * material.c for cullet shade. GLINT handled by `i` down,
             * unreachable by ordinary sand. */
            if (v < SAND_CULLET_BASE) {
                break;
            }
            const unsigned i = ((v - SAND_CULLET_BASE) * (CULLET_CYCLE_LEN / SAND_CULLET_SHADES) + cullet_phase) &
                                (CULLET_CYCLE_LEN - 1);

            /* GLINT: Occasionally flash pure white instead of cycle colour
             * mimicking ground glass. Mix uses existing hash and phase, not
             * RNG, so glinting set changes with cullet_phase.
             * CULLET_GLINT_ONE_IN controls rarity; see its comment for modulo
             * explanation. */
            const bool glint = ((hash + cullet_phase * 0x9E37u) % CULLET_GLINT_ONE_IN) == 0;
            out[0] = glint ? CULLET_GLINT : cullet_cycle[i];
            out[1] = out[0];
            out[2] = out[0];
            return MATERIAL_FLAT;
        }
        case MAT_EXTENDED:
            /* Switched on the low nibble, which for these is their identity
         * rather than a variant - see MATX(). Anything without a grain of
         * its own falls through to the flat palette entry below. */

            /* Metal gets its own leading check, ahead of the guard below,
         * because it still returns a different PATTERN (HATCHED) for its
         * travelling shine - it cannot live inside the ternary, which
         * only ever picks a colour for one shared MATERIAL_SPECKLED
         * return. */

            /* The woven diagonal line HATCHED used to draw alongside that
         * shine is gone (paint_row_n(), app_sand.c) - read as a printed
         * grid rather than metal - but the shine stays, so metal still
         * needs the pattern, just not the line colour. */
            if (v == MATX_METAL) {
                out[0] = metal_grain[hash & 7u];
                out[1] = out[0];
                out[2] = metal_shine;
                return MATERIAL_HATCHED;
            }

            /* Guard-plus-ternary, measured at plant/leaf/ice/root: a
         * switch cost 14% through the inlining cliff, an unhinted branch
         * cost 26% of a benchmark - see docs/Sand/Tuning-At-a-Glance.md. */
            if (v == MATX_PLANT || v == MATX_LEAF || v == MATX_ICE || v == MATX_ROOT) {
                out[0] = (v == MATX_PLANT)  ? plant_grain[hash & 7u]
                         : (v == MATX_LEAF) ? leaf_grain[hash & 7u]
                         : (v == MATX_ICE)  ? ice_grain[hash & 7u]
                                            : root_grain[root_shade(depth)][hash & 7u];
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

            /* The hash picks each cell's own random starting point, stable
             * forever since glass never moves; glass_phase then slides
             * every cell's point by the same amount, so the whole pane
             * drifts together while staying individually scattered. */

            /* A live LERP8, not a lookup into a precomputed ramp - fine
             * enough to move by a small angle would need hundreds of
             * entries per temperature. Runs once per PAINTED cell, not
             * per pixel, so the blend costs nothing a table would save. */
            const unsigned frac = (unsigned)(((int)(hash & 0xFFu) + glass_phase) & 0xFF);

            /* uint32_t, NOT gfx_color_t - `base` is a raw 0xRRGGBB value,
             * not yet packed by GFX_RGB() below. gfx_color_t is uint16_t,
             * the PANEL format, and storing 24 bits into 16 silently drops
             * red's own byte before LERP8 ever runs - this was the actual
             * "glass reads green under heat": red truncated clean away. */
            const uint32_t base = edge ? GLASS_EDGE_RGB(v) : GLASS_RGB(v);
            out[0] = GFX_RGB(LERP8(base, GLASS_GRADIENT_HI(v), frac));
            out[1] = out[0];
            out[2] = out[0];
            return MATERIAL_SPECKLED;
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
static const char* const extended_names[MATERIAL_EXTENDED_CODES] = {
    [MATX_ICE] = "Ice",
    [MATX_PLANT] = "Plant",
    [MATX_LEAF] = "Leaf",
    [MATX_METAL] = "Metal",
    [MATX_ROOT] = "Root",

    /* GUNPOWDER: all eight codes 0xF8-0xFF (low nibble 8-15) name the same
     * material - see GUNPOWDER_BASE, material.h. */
    [8] = "Gunpowder",
    [9] = "Gunpowder",
    [10] = "Gunpowder",
    [11] = "Gunpowder",
    [12] = "Gunpowder",
    [13] = "Gunpowder",
    [14] = "Gunpowder",
    [15] = "Gunpowder",
};

const char*
material_name(cell_t c) {
    /* Tests the whole MAT_EXTENDED nibble, not cell_is_extended() - see
     * reaction_of()'s own comment (material.h) for why this is the one
     * other place that has to route both the static and gunpowder halves
     * of the nibble into the same extended_names[] table. */
    if (CELL_MATERIAL(c) == MAT_EXTENDED) {
        const char* n = extended_names[CELL_VARIANT(c)];
        return (n != NULL) ? n : "?";
    }
    return material_by_id((material_id_t)CELL_MATERIAL(c))->name;
}

const reaction_t extended_reactions[MATERIAL_EXTENDED_CODES] = {

    [MATX_ICE] =
        {
            /* Snow stays put, drifts on falling (scatter 90%), floats on
             * water, melts in liquids. Ice builds walls, chills harder (60 vs
             * 40), reliable for shocking glass, melts near heat (2 vs 4 thaw
             * rate). */
            .chills = 60,
            .heats_to = MAT_WATER,
            .heat_chance = 90,
            .thaws = 2,
        },

    [MATX_PLANT] =
        {
            /* Grows on wet soil, defies gravity, turns to wood when tall.
             * Uses low nibble for material. Grows 40 units in 256 steps (one
             * cell every 6-7 with moisture). Drying soil halts growth. Six in
             * a line become wood. Low growth rate quickly turns sapling into
             * trunk, high rate prevents flat ground growth. Drains and
             * nourishes via brisk water uptake. */
            .drinks = 40,

            .grows = 12,
            .hardens_to = MAT_WOOD,
            .clings_to = MAT_WOOD,

            /* Growth spends soil moisture when extending, rolled off same
             * spend; roots feature PART 1: ONE-TIME SEED for first root
             * (root_depth == 0, spend_soil_moisture()); aligns with wood's
             * row, must move together, no seam at hardening. */
            .roots = 40,
            .roots_to = MATX(MATX_ROOT),

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

            /* POURS behaviour ensures seeds scatter naturally. KIND_POWDER
             * fails due to reaction_t.falls. Leaves should fall cell by step,
             * not teleport. */
            .falls = 85,

            /* Slowest scale rate: 40 scraps, 3/256 half-life 60 steps (2s),
             * 1/256 half-life 6s, enough for watering can and clearing
             * litter. */
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
            /* FOLIAGE: A tree's canopy material distinct from plant. Each
             * cell is a grower; foliage touches wood, never withers, and can
             * drink via find_water(). Plant canopy would feed growth loop,
             * causing runaway growth. No `grows`, `falls`, `hardens_to`:
             * leaves hang, catch fire, and let water through. */
            .clings_to = MAT_WOOD,
            .sheltered_by = MAT_WOOD, /* a tree in drought keeps its leaves */

            /* DRINK required; all extended materials share KIND_STATIC at
             * stone's density. Water cannot fall through or soak into canopy,
             * and a bowl of leaves holds water indefinitely. A real bug on
             * plant; rain lands on leaves. */
            .drinks = 40,

            /* Must GO. No `falls`; trunk burns, crown hangs. Withering
             * handles, `clings_to` prevents living tree shedding. Shed leaf
             * duration matches plant rate. */
            .withers = 1,

            /* Catches far more readily than green stem (40) or seasoned wood
         * (6). A fire that reaches a canopy should run through it, which
         * is both what happens and the best thing to look at. */
            .flammability = 90,

            .dissolvable = 240, /* the softest thing on the board */

            /* RECOGNITION WITHOUT CREATION. `roots` absent; leaf cannot make
             * root. `roots_to` allows `find_water()` to cross root. Leaf
             * needs to drink from ground via roots. Measured: 17 vs 33 levels
             * of soil moisture delivered. Not a repair, but a fix for leaf
             * unable to reach ground through roots. */
            .roots_to = MATX(MATX_ROOT),
        },

    /* METAL. Follows docs/Sand/Metal-Smelting-Plan.md. No variant to spend.
     * Metal's low nibble is identity, so it cannot glow, hold temperature, or
     * melt. Design focuses on metal moving heat long distances. */
    [MATX_METAL] =
        {
            /* Rolled per cell crossed by conduct_heat()'s walk, depth d
             * succeeds with probability (conducts/256)^d. Mean walk:
             * stone/glass (220), metal (248). CONDUCT_REACH (32) marks when
             * cap starts working, stopping self-growing rod at a designed
             * limit, not arbitrary. Tune on device. */
            .conducts = 248,

            /* Metal resists acid as hard as allowed (110 to match stone's 60,
             * not 0, to keep in reaction docs). Future balance may adjust
             * metal's cost. */
            .dissolvable = 1,

            /* No `heats_to`, `heat_ramp`, `heat_chance`, `chills`. Metal
             * survives heat outright, making it valuable for vessels, unlike
             * stone and glass. Every field not named is 0, as per
             * `reactions[]` comment. */
        },

    /* ROOT. What a plant welds itself to the soil with as it spends that
     * soil's moisture - see reaction_t.roots and PART 1 of the roots
     * feature (docs/Sand/Sand-Simulation.md).
     *
     * A root holds still and holds on; that is the whole row. */
    [MATX_ROOT] =
        {
            /* Part of the tree, checks if cell is part of the same body -
             * handles anchoring, stem walk, distance to water. A trunk on its
             * own root reports anchored like on soil, as anchored() checks
             * for non-kin gravity-ward, which a root provides. */
            .clings_to = MAT_WOOD,

            /* Softer than wood's 160: acid has to be able to dig a root
         * out of the ground, or a tree becomes permanent the moment it
         * roots at all. Close to wood's own figure rather than leaves'
         * far softer one - a root is buried structure, not foliage. */
            .dissolvable = 180,

            /* FIRE, NO. DELIBERATELY ZERO. See reactions[]'s top comment for
             * absent field meanings. Buried root: fire could undo feature by
             * burning anchor, leaving tree vulnerable. */
            .flammability = 0,

            /* LAVA, YES. Molten rock burns root systems, causing FIRE. Lava
             * lights trees from roots up. `melts`, not `heat_chance`,
             * prevents contradiction. `heat_chance` remains 0. 24 in 256 is a
             * starting point. Lava requires sustained contact, about ten
             * steps per root cell. */
            .heats_to = MAT_FIRE,
            .melts = 24,

            /* ROTS when orphaned. Without this, roots remained as litter.
             * Burned forests filled with bone-like cells. Orphaned roots stay
             * if sheltered by wood or roots, and if water is accessible.
             * Otherwise, they die. */
            .withers = 1,
            .sheltered_by = MAT_WOOD,

            /* Roots feature part 2. Cell rolls into moist soil using
             * step_one_rooting_cell(). `roots_to` field allows root to
             * convert more of itself, doubling as a recognition target. Roll
             * rate is 3% (8/256), low to bound system with ROOT_SURFACE_MAX
             * and moisture cost. */
            .roots = 8,
            .roots_to = MATX(MATX_ROOT),

            /* No `grows`, no `falls`, no `drinks`, no `sprouts`, no
         * `buds`. A root does not extend along a stem, does not fall
         * with the tree above it, takes no water of its own, and is
         * never a budding or sprouting site - the whole of how it
         * spreads is `roots` above, cell eating cell. */
        },

/* GUNPOWDER'S ROW, 0xF8-0xFF. Flammability 200. Ignites_to, Heats_to
 * GUNPOWDER_LIT_CELL. Heat_chance 24. Burn_decay 16. Lit_from 7. Explodes
 * SAND_GUNPOWDER_BLAST_RADIUS. Needs_air 0. Dissolvable 200. Soaks 60.
 * Soaks_to 0. Tones 3. Moist_max 4. Dries 1. Soaked_to MAT_OIL. Soaked_chance
 * 8. Residue 0. */
#define GUNPOWDER_REACTION                                                                                             \
    {                                                                                                                  \
        .flammability = 200,                                                                                           \
        .ignites_to = GUNPOWDER_LIT_CELL,                                                                              \
        .heats_to = GUNPOWDER_LIT_CELL,                                                                                \
        .heat_chance = 24,                                                                                             \
        .burn_decay = 16,                                                                                              \
        .lit_from = GUNPOWDER_LIT,                                                                                     \
        .explodes = SAND_GUNPOWDER_BLAST_RADIUS,                                                                       \
        .needs_air = 0,                                                                                                \
        .dissolvable = 200,                                                                                            \
        .soaks = 60,                                                                                                   \
        .soaks_to = 0,                                                                                                 \
        .tones = GUNPOWDER_TONES,                                                                                      \
        .moist_max = GUNPOWDER_MOIST_MAX,                                                                              \
        .dries = 1,                                                                                                    \
        .soaked_to = MAT_OIL,                                                                                          \
        .soaked_chance = 8,                                                                                            \
        .residue = 0,                                                                                                  \
    }
    [8] = GUNPOWDER_REACTION,
    [9] = GUNPOWDER_REACTION,
    [10] = GUNPOWDER_REACTION,
    [11] = GUNPOWDER_REACTION,
    [12] = GUNPOWDER_REACTION,
    [13] = GUNPOWDER_REACTION,
    [14] = GUNPOWDER_REACTION,
    [15] = GUNPOWDER_REACTION,
};
#undef GUNPOWDER_REACTION

const gfx_color_t*
material_palette(void) {
    return palette;
}
