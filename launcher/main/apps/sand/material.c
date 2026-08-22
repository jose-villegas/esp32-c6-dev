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

    /* Slots 4-15 are unused and left zeroed except for these, which make an
     * unknown material inert rather than undefined: it never moves and nothing
     * can displace it. Designated initialisers zero the rest. */
    [MAT_COUNT ... MATERIAL_MAX - 1] = {
        .name    = "?",
        .kind    = KIND_STATIC,
        .density = 255,
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
    UNUSED, UNUSED, UNUSED, UNUSED,
    UNUSED, UNUSED, UNUSED, UNUSED,
    UNUSED, UNUSED, UNUSED, UNUSED,
};

const gfx_color_t *material_palette(void)
{
    return palette;
}
