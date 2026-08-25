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

        .buoyancy = 96,        /* ~2.7 steps average between rises - was 32
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
        .buoyancy = 96,         /* matches gas's own figure as a
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
        /* slip/repose/scatter/decay/buoyancy/sight all meaningless for a
         * KIND_STATIC material and left at zero, same as stone's own row. */
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

    /* Slots 8-15 are unused and left zeroed except for these, which make an
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
        /* .quench_to left at 0: touching water still just vanishes, the
         * same behaviour fire always had before steam existed - see
         * material.h's own comment on quench_to. */
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
    },

    [MAT_EMBER] = {
        .burns = 1,     /* a second heat source, alongside fire - see
                         * sand_reactions.c's dispatch, which is why that
                         * now keys off reaction_t.burns rather than
                         * CELL_MATERIAL(c) == MAT_FIRE */
        /* .quench_to left at 0 for the same reason fire's is, above. */

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

/* One material's sixteen variants, darkest to lightest. */
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
    SHADES(0x2A0A00, 0xFF7A28),   /* ember - dying char to glowing orange,
                                    * deliberately redder and darker than
                                    * fire's own yellow-white so a
                                    * smouldering log reads differently
                                    * from the flame above it */
    UNUSED, UNUSED, UNUSED, UNUSED,
    UNUSED, UNUSED, UNUSED, UNUSED,
};

const gfx_color_t *material_palette(void)
{
    return palette;
}
