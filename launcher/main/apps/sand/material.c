#include <stddef.h> /* NULL - material.h does not pull it in, and
                      * whether anything else does is a property of
                      * the toolchain rather than of this file */
#include "material.h"

/*=============================================================================
 * The table.
 *
 * `const`, so it lives in flash rather than RAM. Adding a material is a row.
 *===========================================================================*/

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
            .slip = 96,
            .repose = 7, /* about 35 degrees, dry sand */
            .scatter = 40,
        }),

    TWIN_ROW(MAT_WATER,
        {

            .name = "Water",
            .kind = KIND_LIQUID,
            .density = 30, /* lighter than sand, so sand sinks through it */

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
            .density = 181, /* Nothing displaces it via ordinary movement -
                              * this only feeds the dislodge-toughness roll
                              * (queue_flying_grain(), sand_impulse.c). Rank
                              * 2 of 8 on `density = 221 - 20*rank`, the
                              * fragility curve every solid sits on - see
                              * MATX_METAL's own comment for the full
                              * ranking and the curve's own derivation. */
            .slip = 0,
            .repose = 0,
            .scatter = 0,
        }),

    TWIN_ROW(MAT_GAS,
        {
            .name = "Gas",
            .kind = KIND_GAS,
            .density = 10, /* 1-25 works the same */

            .slip = 255,
            .repose = 0,
            .scatter = 120, /* well above sand's 40 - a visibly turbulent,
                               * wispy rise rather than a rigid column */

            .decay = 6, /* Gas is whole-grain. Decay prevents infinite solid
                         * buildup. Adjust on device. */

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
            .kind = KIND_GAS, /* replaces KIND_STATIC (immobile, never
                               * buriable) */

            .density = 15, /* Gas (10) vs. sand (60). Sand sinks through fire.
                            * Fire avoids dense gases, mixes with ignited
                            * elements. */
            .slip = 255,   /* no resistance, same reasoning as
                                 * gas's own row above */
            .repose = 0,
            .scatter = 120, /* matches gas's own figure - equally
                                 * turbulent rise, tune independently
                                 * later if it should read differently */

            .decay = 96,    /* Gap > 32: ~40 steps, under a second at 60fps.
                             * Fire burns faster than gas. Tune on device. */
            .mobility = 96, /* Tune independently if fire should rise
                             * faster/slower than gas */
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
            .density = 141,      /* above sand (60) and water (30), so
                                     * neither can displace a log - it
                                     * holds its shape under a pour, the
                                     * way a real log does not wash away.
                                     * Rank 4 of 8 on the fragility curve
                                     * (see MATX_METAL's own comment) -
                                     * tougher than glass, which shatters,
                                     * but more brittle than a root. */
        }),

    TWIN_ROW(MAT_STEAM,
        {
            .name = "Steam",
            .kind = KIND_GAS, /* Rises and disperses like gas and fire;
                               * lighter and faster. Steam is boiled water or
                               * flashed off fire. Fuel burning leaves
                               * MAT_SMOKE. Initially one material, but
                               * separate rows for correct visuals. */

            .density = 5, /* Below gas and fire so it can't displace them
                           * (can_enter() needs strictly greater density);
                           * lighter than water, so it bubbles up through it. */
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
            .mobility = 160, /* Steam rises eagerly, not like gas. Tune on
                              * device. */
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

            .density = 7, /* Smoke heavier; steam rises, correct for basin
                           * over fire. */
            .slip = 255,  /* no resistance, same reasoning as
                                     * gas's own row */
            .repose = 0,
            .scatter = 150, /* just above steam's 140 - smoke
                                     * curls a little more than steam
                                     * does. Starting point, not final -
                                     * tune on device like every other
                                     * constant here. */

            .decay = 16,     /* Decay chance per step; smaller means slower
                              * decay. Tune on device. */
            .mobility = 120, /* between gas's 96 and steam's 160 -
                                     * smoke climbs, but lazily, where
                                     * steam comes off a boil eagerly.
                                     * Starting point, not final - tune
                                     * on device like every other
                                     * constant here. */
            .sight = 24,     /* smoke spreads into haze, not column. Tune on
                              * device. */
        }),

    TWIN_ROW(MAT_OIL,
        {
            .name = "Oil",
            .kind = KIND_LIQUID,
            .density = 22, /* Oil floats, fire can't move it, sand sinks. */

            .mobility = 140, /* VISCOSITY inverted - see material.h. Water
                              * spreads in 8 steps, oil in 18. Tune on device. */

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

            .mobility = 70, /* VISCOSITY inverted in material.h. Set unset
                             * byte to zero for free flow. Tune on device. */

            .decay = 0, /* decay != 0 changes lava meaning, lava immortal,
                         * cools by water */
        }),

    TWIN_ROW(MAT_ACID,
        {
            .name = "Acid",
            .kind = KIND_LIQUID,
            .density = 38, /* Acid sinks in water, floats on lava. Sand sinks
                            * in acid. Grain must enter acid. */
            .slip = 255,   /* the usual "no resistance" values a liquid
                              * leaves these at - see water's own row */
            .repose = 0,
            .scatter = 0,

            .mobility = 220, /* VISCOSITY, inverted - see material.h. Acid is
                              * runny, slower to read as heavier. Tune on
                              * device. */
        }),

    TWIN_ROW(MAT_GLASS,
        {
            .name = "Glass",
            .kind = KIND_STATIC,
            .density = 121, /* Brittle - rank 5 of 8 on the fragility curve
                              * (see MATX_METAL's own comment), more easily
                              * dislodged than wood or a root but tougher
                              * than ice/plant/leaf. Differs from stone in
                              * ACID resistance too. */
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
            .slip = 64,    /* Snow clumps, bank shape hold. Packs onto glass
                            * pane. */
            .repose = 9,   /* ~42 degrees, steeper than dry sand's ~35 -
                              * again so a bank holds. */
            .scatter = 90, /* Falling snow drifts instead of dropping
                            * straight, making it look like snow. */
        }),

    /* NIBBLE 15, TWO ROWS - varied materials. No unique density, kind, slip,
     * repose, or scatter. */
    [MATERIAL_ROW(MAT_EXTENDED)] =
        {
            .name = "Extended", .kind = KIND_STATIC, .density = 181, /* stone's figure: undisplaceable, and it
                              * smothers a buried flame the way stone
                              * does. A named extended static overrides
                              * this via its own dislodge_density. */
        },

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
    /* Water's effects on materials come from the outside; `soaks` in sand and
     * soil, causing confusion. */
    [MAT_WATER] =
        {
            .wets = 1,

            /* Acid dissolves roll land on water. Water turns to acid as with
             * sand, wood, ash. Adjust as needed. */
            .dissolvable = 220,

            /* Low, deliberately: a poured stream can occasionally outpace
             * evaporation over hot stone crust. Acid, below, is at the
             * opposite end. Tune on device. */
            .boils = 29,
        },

    [MAT_ACID] =
        {
            .dissolves = 60,

            .fizz = 40, /* Adjust as needed. */

            .evaporates = 1, /* Rarest byte-wide roll: 1 in 256 per cell per
                              * step. Still too frequent in real puddles. See
                              * `step_one_dissolver_cell()` for extra gate
                              * (now 1-in-60, effective 1 in 15360).
                              * `sand_set_evaporates()` override bypasses
                              * gate. Tune on device. */

            .boils = 255,

            /* Acid evaporates to MAT_GAS, not kettle-steam. */
            .boils_to = MAT_GAS,
        },

    [MAT_OIL] =
        {
            .flammability = 50,
            .needs_air = 1,
            .ignites_to = MAT_FIRE, /* burns straight to flame, unlike
                                     * wood: there is no log left to
                                     * smoulder, the fuel simply goes.
                                     * The flame rises off, exposing the
                                     * layer beneath, which is what eats
                                     * the pool downward */

            .dissolvable = 16,
        },

    [MAT_LAVA] =
        {
            /* KIND_LIQUID and `burns` prove independent axes. */
            .burns = 1,

            .quench_to = MAT_STONE, /* water turns lava into rock, paying mass
                                     * like quenching fire; puddle cannot pave
                                     * ocean for free */

            .flare = 16, /* Pool reads dangerous, not decorative. Starting
                          * point, not final. */

        },

    [MAT_SAND] =
        {
            .dissolvable = 200,

            .soaks = 8,
            .soaks_to = MAT_DIRT,

            .heats_to = MAT_GLASS,
            .heat_chance = 8, /* ~3% a step: vitrifying should be a slow
                               * change you watch happen, not a flash. */
        },

    [MAT_GLASS] =
        {
            .conducts = 220,

            /* Heat drains, ramp mean duration. `cools` at half ramp. */
            .heats_to = MAT_LAVA,
            .heat_ramp = 64,

            .cools = 5,

            .shatters_to = MAT_SAND,
        },

    [MAT_DIRT] =
        {
            .tones = SOIL_DRY_TONES,       /* MUST agree with
                                            * SOIL_DRY_TONES/SOIL_MOISTURE_MAX */
            .moist_max = SOIL_MOISTURE_MAX,
            .soaks = 60,

            /* see reaction_t.soil for why separate from `dries` */
            .soil = 1,
            .dries = 2,

            .dissolvable = 200, /* the same as sand: it is mostly sand */

        .heats_to    = MATX(MATX_METAL),
        .heat_chance = 10,

        /* METAL IS WHAT SURVIVES THIS ROLL, so the number is the STONE
         * share: 230/256 stone leaves ~10% metal. Clumped, not sprinkled -
         * HEAT_FLAW_CLUMP (sand_reactions.c) re-rolls only every fifth cell,
         * so ore arrives in short veins and one run in ten is metal. */
        .flaw_to     = MAT_STONE,
        .flaw_chance = 230,

        /* Wet ground now yields to heat more often than it crumbles: 77/256
         * is about 30%, against the 235 that made wet dirt almost never
         * produce anything but sand. Digging into damp earth is meant to
         * be worth doing, not a reason to dry it out first. */
        .spoils_to     = MAT_SAND,
        .spoils_chance = 77,
    },

    [MAT_SNOW] =
        {
            /* 40 in 256 cools a pane without cracking; 120 ensures snow near
             * heat sources melts quickly. */
            .chills = 40,
            .heats_to = MAT_WATER,
            .heat_chance = 120,

            /* See reaction_t.thaws. Snow melts slower. */
            .thaws = 4,
        },

    [MAT_STEAM] =
        {
            .warms = 48, /* the hotter carrier: water that has just boiled */

            /* Cosmetic, no real thermal model. Tune as needed. */
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
            .heat_ramp = 32,
            .cools = 5,

            .dissolvable = 60, /* Stone fails at 200; MAT_GLASS replaces it.
                                * Glass immune due to no `dissolvable`. */

            /* Heat crosses ONE cell with ~0.86 chance; attenuates with depth.
             * Was 0.69, too timid */
            .conducts = 220,
        },

    [MAT_GAS] =
        {
            /* gas catches fire on contact, costing no RNG draw; keeps tests
             * and frame-budget captures identical */
            .flammability = 255,
            .ignites_to = MAT_FIRE, /* MAT_FIRE is explicit, not default 0 */
        },

    [MAT_FIRE] =
        {
            .burns = 1, /* see sand_reactions.c's dispatch */

            .residue = 40, /* 1 in 256 chance a burnt-out fire cell leaves
                            * MAT_STEAM behind. Lower than ember's 90%.
                            * Starting point; tune as needed. */

            .quench_to = MAT_STEAM, /* Steam byproduct, not free lunch. Pot
                                     * runs dry eventually. */
        },

    [MAT_WOOD] =
        {
            .sprouts = 6,
            .sprouts_to = MATX(MATX_LEAF),

            .buds = 32,
            .buds_to = MATX(MATX_PLANT),

            .roots = 40,
            .roots_to = MATX(MATX_ROOT),

            /* Trunk waters roots at third green growth rate. */
            .drinks = 12,

            /* 6 in 256 is 43 steps before catch. Wood != MAT_FIRE. Tune on
             * device. */
            .flammability = 6,

            .ignites_to = MAT_WOOD,

            .burn_decay = 24,

            /* Similar to gunpowder's row stating 7. */
            .lit_from = 1,

            .residue = 90, /* Log burn > flame guttering, more smoke */

            .quench_to = 0, /* Water extinguishes fire, replacing it with
                             * steam; step_one_burning_cell() does this for
                             * burn_decay material. Ember named MAT_STEAM as
                             * the fire itself was extinguished. */

            .flare = 48, /* Wood is KIND_STATIC; otherwise, it would glow
                          * without flame. Flame rises via sand_step_gas(). */

            .dissolvable = 160, /* slower than sand's 200: a plank holds
                                   * out a moment longer than a loose pile
                                   * does */
        },

};

static const char* const extended_names[MATERIAL_EXTENDED_CODES] = {
    [MATX_ICE] = "Ice",
    [MATX_PLANT] = "Plant",
    [MATX_LEAF] = "Leaf",
    [MATX_METAL] = "Metal",
    [MATX_ROOT] = "Root",

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
    /* Tests MAT_EXTENDED nibble; see reaction_of() for why both halves route
     * to extended_names[]. */
    if (CELL_MATERIAL(c) == MAT_EXTENDED) {
        const char* n = extended_names[CELL_VARIANT(c)];
        return (n != NULL) ? n : "?";
    }
    return material_by_id((material_id_t)CELL_MATERIAL(c))->name;
}

const reaction_t extended_reactions[MATERIAL_EXTENDED_CODES] = {

    [MATX_ICE] =
        {
            /* Snow melts in liquids, ice melts near heat. */
            .chills = 60,
            .heats_to = MAT_WATER,
            .heat_chance = 90,
            .thaws = 2,

            .dislodge_density = 101, /* brittle - rank 6 of 8 on the
                                       * fragility curve (MATX_METAL's own
                                       * comment); shatters/dislodges more
                                       * easily than any solid but plant
                                       * and leaf */
        },

    [MATX_PLANT] =
        {
            .drinks = 40,

            .grows = 12,
            .hardens_to = MAT_WOOD,
            .clings_to = MAT_WOOD,

            /* PART 1: ONE-TIME SEED for first root, aligns with wood's row,
             * no seam at hardening. */
            .roots = 40,
            .roots_to = MATX(MATX_ROOT),

            .canopy = 110,
            .canopy_to = MATX(MATX_LEAF),
            .trunk_girth = 2,

            /* High, but not certain. Occasional reversion to gravity bends
             * limb towards upright. */
            .holds_line = 200, /* and it is part of one, which is the
                                    * same material here and will not be
                                    * once foliage exists */
            .harden_run = 6,
            .harden_chance = 64, /* one in four; measured */

            /* KIND_POWDER fails due to reaction_t.falls. Leaves should fall
             * cell by step, not teleport. */
            .falls = 85,

            .flammability = 40,

            .dissolvable = 220, /* softer than wood's 160 - acid goes
                                * through leaves faster than through a
                                * plank */

            .dislodge_density = 81, /* soft - rank 7 of 8 on the fragility
                                      * curve (MATX_METAL's own comment);
                                      * easier to dislodge than any solid
                                      * but leaf */
        },

    [MATX_LEAF] =
        {
            /* FOLIAGE: Grows, catches fire, lets water through. No `grows`,
             * `falls`, `hardens_to`. */
            .clings_to = MAT_WOOD,

            /* DRINK required; leaves hold water. Real bug on plant. Rain
             * lands on leaves. */
            .drinks = 40,

            .flammability = 90,

            .dissolvable = 240, /* the softest thing on the board */

            .roots_to = MATX(MATX_ROOT),

            .dislodge_density = 61, /* rank 8 of 8, the bottom of the
                                      * fragility curve (MATX_METAL's own
                                      * comment) - the most easily
                                      * dislodged solid on the board, same
                                      * story as dissolvable above */
        },

    /* METAL. See docs/sand/Metal.md. Low nibble is identity, no
     * glow, heat, or melt. Focuses on heat movement. */
    [MATX_METAL] =
        {
            /* CONDUCT_REACH (32) marks cap activation, halting rod growth.
             * Adjust device. */
            .conducts = 248,

            /* Acid barely touches it - the one axis on which metal beats
             * both other walls (stone 60, glass immune but melts). */
            .dissolvable = 1,

            /* NO heats_to, deliberately. Metal has no variant to ramp, so
             * the only way to melt it would be a memoryless roll - which
             * would make a metal wall beside lava randomly turn into lava.
             * Heatproof instead, so the three walls differ on one axis
             * each: stone survives heat and dissolves slowly, glass melts
             * but is acid-immune, metal survives both and conducts best. */

            /* THE FRAGILITY CURVE every solid's toughness sits on:
             * density = 221 - 20*rank, rank 1 (toughest) to 8 (softest).
             * Metal(1)=201, Stone(2)=181, Root(3)=161, Wood(4)=141,
             * Glass(5)=121, Ice(6)=101, Plant(7)=81, Leaf(8)=61 - see each
             * material's own density/dislodge_density for its rank. One
             * knob (rank) instead of eight independently-tuned numbers. */
            .dislodge_density = 201,
        },

    /* See reaction_t.roots and PART 1 of the roots feature
     * (docs/sand/Sand-Simulation.md). */
    [MATX_ROOT] =
        {
            /* Handles anchoring, stem walk, distance to water. Trunk on root
             * reports anchored like soil. */
            .clings_to = MAT_WOOD,

            .dissolvable = 180,

            .dislodge_density = 161, /* rank 3 of 8 on the fragility curve
                                       * (MATX_METAL's own comment) -
                                       * embedded in soil, tougher than
                                       * even wood, but not a trunk */

            /* NO. Zero. Fire risks burning anchor, leaving tree vulnerable. */
            .flammability = 0,

            /* LAVA, YES. Molten rock burns roots (`melts`, not
             * `heat_chance`). Lava needs ten steps per root cell. */
            .heats_to = MAT_FIRE,
            .melts = 24,

            /* Root cell rolls into soil, `roots_to` doubles as target. Kept
             * low (8 in 256) since, unlike the one-time collar seed, this
             * rolls every step for every root cell - a low base rate is
             * part of what bounds the cost, alongside ROOT_SURFACE_MAX
             * (sand_reactions.c). */
            .roots = 8,
            .roots_to = MATX(MATX_ROOT),

        },

/* GUNPOWDER_BASE, material.h - one row, eight designators */

/* flammability = 200: catches instantly - key trait for powder keg. */

/* Lights fuse, not MAT_FIRE. */

/* heat_chance = 24: wood's own smoulder figure - conducted heat is a
 * slower fuse than a direct flame. */

/* lit_from = GUNPOWDER_LIT (7): codes below it are dry tones and moisture,
 * never mistaken for embers. */

/* catches through volume, not just face */

/* soaks = 60: water wets it - moisture climbs, water is consumed - dirt's
 * own rate. */

/* soaks_to = 0: stays gunpowder while it wets, only wetter, same as
 * dirt. */

/* dries = 1: far under dirt's 2 - a powder keg holds water a long time
 * once soaked. */

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
