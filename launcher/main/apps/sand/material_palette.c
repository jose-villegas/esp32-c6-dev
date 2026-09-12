/*
 * material_palette - what a material looks like, built once at compile time.
 *
 * Split off material.c's data half (materials[], reactions[] - see that
 * file's own banner) because everything below is colour, not behaviour: a
 * 256-entry palette and the per-cell function app_sand.c's row painter
 * (paint_row_n()) calls once per cell, every frame. Keeping it separate
 * means a caller of material.c's tables - the simulation's hot loop, the
 * host test runner, tools/dump_reactions.c - never has to pull in
 * gfx_color_t just to ask what a material IS.
 *
 * The palette itself is `const`, so it costs 512 bytes of flash and no RAM.
 * Writing it out by hand would be 256 unreadable, unmaintainable literals,
 * so the material colours are interpolated by macro instead - ugly to read
 * once, and the alternative is either a table nobody can safely edit, or
 * building it at startup and paying for it in the resource there is least
 * of.
 */
#include "material_palette.h"
#include "util/intmath.h" /* see material_set_gravity() below */

/* Channel `sh` of the way from `lo` to `hi`, out of 15. */
#define LERP_CH(lo, hi, shift, sh)                                                                                     \
    ((((((lo) >> (shift)) & 0xFF) * (15 - (sh)) + (((hi) >> (shift)) & 0xFF) * (sh)) / 15) & 0xFF)

#define LERP_RGB(lo, hi, sh) ((LERP_CH(lo, hi, 16, sh) << 16) | (LERP_CH(lo, hi, 8, sh) << 8) | LERP_CH(lo, hi, 0, sh))

#ifdef __CPPCHECK__
/* The tables below expand this a few thousand times, which exhausts the MISRA
 * addon before it reaches any code. Stubbed for the tables only: LERP goes
 * back to LERP_RGB below them, so the functions are analysed as written. */
#define LERP(lo, hi, sh) ((uint32_t)(lo) + (uint32_t)(hi) + (uint32_t)(sh))
#else
#define LERP(lo, hi, sh) LERP_RGB(lo, hi, sh)
#endif

/* glass MAT_GLASS case needs small tilt for finer gradient than palette steps */
#define LERP8_CH(lo, hi, shift, fr)                                                                                    \
    ((((((lo) >> (shift)) & 0xFF) * (255 - (fr)) + (((hi) >> (shift)) & 0xFF) * (fr)) / 255) & 0xFF)

#define LERP8(lo, hi, fr) ((LERP8_CH(lo, hi, 16, fr) << 16) | (LERP8_CH(lo, hi, 8, fr) << 8) | LERP8_CH(lo, hi, 0, fr))

/* Ramp for `n` steps between colours, split for material limits. */
#define SEG(lo, hi, i, n) GFX_RGB(LERP(lo, hi, ((i) * 15) / ((n) - 1)))

/* Carried tone shape for independent moisture levels. */

/* Wet soil darker, 7 point cap. */

/* Removes slot sharing: variant 0 only needs to be paler than variant 1. */
#define DIRT_DRY          0x9A7B52
#define DIRT_WET          0x3A2A18

#define SOIL_SHADES                                                                                                    \
    GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 0)), GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 1)), GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 2)),  \
        GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 3)), GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 4)),                                    \
        GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 5)), GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 6)),                                    \
        GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 7)), GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 8)),                                    \
        GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 9)), GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 10)),                                   \
        GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 11)), GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 12)),                                  \
        GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 13)), GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 15)), /* variant 14: saturated */      \
        GFX_RGB(LERP(DIRT_DRY, DIRT_WET, 15)) /* variant 15: unused - same as 14 */

/* Ember ramp: dying char through glowing orange, redder and darker than
 * fire's yellow-white. */

/* See material_colours() for speckling. */
#define WOOD_UNLIT   0x5A3D24
#define WOOD_CHAR    0x2A0A00
#define WOOD_GLOW    0xFF7A28

#define WOOD_BURN(i) GFX_RGB(LERP(WOOD_CHAR, WOOD_GLOW, ((i) - 1) * 15 / 14))

#define WOOD_SHADES                                                                                                    \
    GFX_RGB(WOOD_UNLIT), WOOD_BURN(1), WOOD_BURN(2), WOOD_BURN(3), WOOD_BURN(4), WOOD_BURN(5), WOOD_BURN(6),           \
        WOOD_BURN(7), WOOD_BURN(8), WOOD_BURN(9), WOOD_BURN(10), WOOD_BURN(11), WOOD_BURN(12), WOOD_BURN(13),          \
        WOOD_BURN(14), WOOD_BURN(15)

/* Unlit wood beside a leaf blends live between these two anchors (LERP8 -
 * see material_wood_leaf_wave() and material_colours()'s MAT_WOOD case).
 * Both anchored on leaf's own green, not wood's colour - even at rest this
 * should read as leaf, just a darker shade of it. */
#define WOOD_LEAF_TINT_LO LERP(0x468F26, 0x000000, 6)
#define WOOD_LEAF_TINT_HI LERP(0x468F26, 0x8CD24E, 3)

/* Hot walls appear visibly hot now. */
#define STONE_FROST       0xCEDCE8
#define STONE_AMBIENT     0x5F6673
#define STONE_NEUTRAL     0x8A7466
#define STONE_GLOW        0x9E3A18
#define STONE_MOLTEN      0xE8752A

#define STONE_COOL(v)     LERP(STONE_FROST, STONE_AMBIENT, ((v) * 15) / (SAND_AMBIENT_HEAT > 0 ? SAND_AMBIENT_HEAT : 1))
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

/* Glass at room temp is mid. Below sand, it frosts. At shock, it breaks,
 * glowing like lava. */

/* Shock breaks largest step: material shatters, not cools. Different
 * behaviors should look distinct. */

/* CYAN, and only the cold END of the ramp - the shimmer keeps its own pale
 * target below, kept separate so a chilling cell doesn't drift toward the
 * very colour the per-cell shimmer already blends toward. Cyan holds
 * saturation as it cools, keeping red near 95 rather than washing out
 * toward white. */
#define GLASS_FROST   0x5FE6F0

/* Where a cell's per-cell shimmer blends TO. Was GLASS_FROST; kept at that
 * old value so ambient glass renders exactly as it always has. */
#define GLASS_SHIMMER 0xD6EEF8
#define GLASS_AMBIENT 0x2E6B85
#define GLASS_NEUTRAL 0x8C7E70
#define GLASS_GLOW    0xC8701E
#define GLASS_MOLTEN  0xFFD873

/* Guards on denominators prevent division by zero. */
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

_Static_assert(SAND_AMBIENT_HEAT > 0 && SAND_AMBIENT_HEAT < SAND_SHOCK_HEAT && SAND_SHOCK_HEAT < MATERIAL_VARIANTS - 1,
               "glass needs room below ambient for frost, room above the "
               "shock point to keep climbing, and ambient strictly between");
#define SAND_DUNE 0xB07430
#define SAND_PALE 0xF2CE90

#define SAND_DUNE_RAMP                                                                                                 \
    GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 0)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 1)),                                    \
        GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 2)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 4)),                                \
        GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 5)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 6)),                                \
        GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 8)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 9)),                                \
        GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 10)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 12)),                              \
        GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 13)), GFX_RGB(LERP(SAND_DUNE, SAND_PALE, 15))

/* STARTING POINTS on a 16-step colour cycle, close in hue and lightness.
 * Shared with SAND_CULLET_RAMP's phase-0 colours below, so a build that
 * never calls material_set_cullet_phase() (a host test) still shows the
 * same cullet a running frame does at rest. GLINT is pure white for one
 * phase. */
#define CULLET_CYCLE_A      0xCFEAF2 /* pale cyan */
#define CULLET_CYCLE_B      0xD8D0F0 /* lilac */
#define CULLET_CYCLE_C      0xF0D6DC /* rose */
#define CULLET_CYCLE_D      0xE2F0D2 /* mint */

/* What a glinting grain shows: full white, the panel's highest radiance. */
#define CULLET_GLINT        GFX_RGB(0xFFFFFF)

/* Odds one in 192. Modulo for flexibility. More glints with lower value. */
#define CULLET_GLINT_ONE_IN 192

#define SAND_CULLET_RAMP                                                                                               \
    GFX_RGB(CULLET_CYCLE_A), GFX_RGB(CULLET_CYCLE_B), GFX_RGB(CULLET_CYCLE_C), GFX_RGB(CULLET_CYCLE_D)

/* Define CULLET_CYCLE_LEN in material.h for host tests. Array safety depends
 * on these properties. */
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

#define UNUSED SHADES(0xFF00FF, 0xFF00FF)

/* THE source of colour. One place to change. Rows in material_id_t order. */
static const gfx_color_t palette[256] = {
    [MAT_EMPTY * MATERIAL_VARIANTS] = SHADES(0x0A0C14, 0x0A0C14), /* empty - the background */
    [MAT_SAND * MATERIAL_VARIANTS] =
        /* Cullet leaves ramp for cool cast. Phase-0 uses rest look. Live
         * frames use MAT_SAND. Same anchors. */
    SAND_DUNE_RAMP,
    SAND_CULLET_RAMP,
    [MAT_WATER * MATERIAL_VARIANTS] = SHADES(0x77C4E8, 0x14406F), /* water - shallow is pale, deep is dark */
    [MAT_STONE * MATERIAL_VARIANTS] = STONE_SHADES,               /* TEMPERATURE
                                                                   * scale,
                                                                   * same
                                                                   * levels
                                                                   * and
                                                                   * meanings
                                                                   * as glass */
    [MAT_GAS * MATERIAL_VARIANTS] = SHADES(0x445544, 0xC8E8B8),   /* gas   */
    [MAT_FIRE * MATERIAL_VARIANTS] = SHADES(0x400A00, 0xFFE060),  /* fire  - dying ember is dark, freshly
                                    * lit is bright yellow-white; variant
                                    * is life remaining, same trick gas
                                    * already uses */
    [MAT_WOOD * MATERIAL_VARIANTS] = WOOD_SHADES,                 /* UNLIT;
                                                                   * others
                                                                   * indicate
                                                                   * burn
                                                                   * amount.
                                                                   * See
                                                                   * WOOD_SHADES */
    [MAT_STEAM * MATERIAL_VARIANTS] = SHADES(0x6E8496, 0xF2FAFF), /* COOL and
                                                                   * BRIGHT,
                                                                   * contrasting
                                                                   * with
                                                                   * smoke;
                                                                   * two
                                                                   * materials
                                                                   * for
                                                                   * visual
                                                                   * distinction. */
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
    [MAT_LAVA * MATERIAL_VARIANTS] = SHADES(0xFFC24A, 0x8A1400),  /* lava -
                                                                   * fill
                                                                   * level
                                                                   * INVERTED:
                                                                   * thin
                                                                   * yellow,
                                                                   * deep red,
                                                                   * reads as
                                                                   * cooling
                                                                   * crust,
                                                                   * distinct
                                                                   * from
                                                                   * flames */
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
    [MAT_GLASS * MATERIAL_VARIANTS] = GLASS_SHADES,               /* Heat
                                                                   * variant
                                                                   * in
                                                                   * material.h.
                                                                   * Temp
                                                                   * scale;
                                                                   * pane
                                                                   * glows
                                                                   * cool teal
                                                                   * to
                                                                   * 0xFFC24A,
                                                                   * seamless. */
    [MAT_DIRT * MATERIAL_VARIANTS] =
        /* Dirt: Dusty tan to dark damp earth, strictly darkening. Soil
         * shading varies with state. */
    SOIL_SHADES,
    [MAT_SNOW * MATERIAL_VARIANTS] = SHADES(0xC6D8E4, 0xFFFFFF), /* Deliberately
                                                                  * palest,
                                                                  * reads as
                                                                  * COLD */
    /* Named entries, not counted. Magenta tail: statics' codes, padding.
     * Non-zero codes block BLACK. */
    [MAT_EXTENDED * MATERIAL_VARIANTS + MATX_ICE] = GFX_RGB(0xB6E4F2),      /* ice - paler and bluer than snow's
                                    * white, and flat rather than speckled:
                                    * a block of it should read as solid
                                    * and cold, where snow reads as loose */
    [MAT_EXTENDED * MATERIAL_VARIANTS + MATX_PLANT] = GFX_RGB(0x55672D),    /* plant
                                                                             * -
                                                                             * OLIVE:
                                                                             * stem
                                                                             * timber
                                                                             * brown,
                                                                             * leaves
                                                                             * green */
    [MAT_EXTENDED * MATERIAL_VARIANTS + MATX_LEAF] = GFX_RGB(0x69B03A),     /* only
                                                                             * part
                                                                             * meant
                                                                             * to
                                                                             * catch
                                                                             * the
                                                                             * eye */
    [MAT_EXTENDED * MATERIAL_VARIANTS + MATX_METAL] = GFX_RGB(0x7C8794),    /* Visual
                                                                             * separation.
                                                                             * See
                                                                             * docs/sand/Metal.md. */
    [MAT_EXTENDED * MATERIAL_VARIANTS + MATX_ROOT] = GFX_RGB(0xBFA58A),     /* root
                                                                             * -
                                                                             * matches
                                                                             * trunk,
                                                                             * avoids
                                                                             * dirt's
                                                                             * range */
    [MAT_EXTENDED * MATERIAL_VARIANTS + MATX_ROOT + 1] = GFX_RGB(0xFF00FF), /* spare static, unclaimed */
    GFX_RGB(0xFF00FF),                                                      /* spare static, unclaimed */
    GFX_RGB(0xFF00FF),                                                      /* spare static, unclaimed */

    [GUNPOWDER_CELL(0)] = GFX_RGB(0x141014), /* dry, tone 0 - near-black */
    [GUNPOWDER_CELL(1)] = GFX_RGB(0x2B1410), /* dry, tone 1 - black-red */
    [GUNPOWDER_CELL(2)] = GFX_RGB(0x46160F), /* brush_color(), app_sand.c -
                                              * only visible tone */
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

#define GLASS_RGB(v)        ((v) <= SAND_AMBIENT_HEAT ? GLASS_COOL(v) : (v) < SAND_SHOCK_HEAT ? GLASS_WARM(v) : GLASS_HOT(v))

/* Blends towards ambient, not endpoints like STONE_EDGE_RGB. */

/* Averaging HOT and ambient saturates to unrelated green. */

/* Blending TEMPERATURE first keeps glass edges clear on COOL/WARM/HOT. Stone
 * endpoints avoid mixing. */
#define GLASS_EDGE_V_RAW(v) ((v) + (((int)(SAND_AMBIENT_HEAT) - (int)(v)) * 10) / 15)

/* Clamped to SAND_SHOCK_HEAT if already HOT. */
#define GLASS_EDGE_V(v)                                                                                                \
    (((v) >= SAND_SHOCK_HEAT && GLASS_EDGE_V_RAW(v) < SAND_SHOCK_HEAT) ? SAND_SHOCK_HEAT : GLASS_EDGE_V_RAW(v))
#define GLASS_EDGE_RGB(v)    GLASS_RGB(GLASS_EDGE_V(v))
#define STONE_EDGE_RGB(v)    LERP(STONE_RGB(v), STONE_RGB(SAND_AMBIENT_HEAT), 10)

/* GLASS_SHIMMER via COOL. Mix WARM/HOT for icy blue to muddy yellow-green.
 * White stays, brighter. */
#define GLASS_GRADIENT_HI(v) ((v) <= SAND_AMBIENT_HEAT ? GLASS_SHIMMER : 0xFFFFFF)

#define STONE_DARK(rgb)      LERP((rgb), 0x000000, 3)
#define STONE_LIGHT(rgb)     LERP((rgb), 0xFFFFFF, 3)

#define STONE_GRAIN(rgb, k)  GFX_RGB(LERP(STONE_DARK(rgb), STONE_LIGHT(rgb), (k) * 15 / 7))

#define STONE_SPECKLE(v, k)  STONE_GRAIN(STONE_RGB(v), k)

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

/* UNLIT wood speckled; burning logs glow uniformly. */
#define WOOD_GRAIN(k) GFX_RGB(LERP(LERP(WOOD_UNLIT, 0x000000, 3), LERP(WOOD_UNLIT, 0xFFFFFF, 2), (k) * 15 / 7))

/* Dirt movement caused texture repeat; now, tone in cell with palette. */

static const gfx_color_t wood_grain[8] = {
    WOOD_GRAIN(0), WOOD_GRAIN(1), WOOD_GRAIN(2), WOOD_GRAIN(3),
    WOOD_GRAIN(4), WOOD_GRAIN(5), WOOD_GRAIN(6), WOOD_GRAIN(7),
};

/* Extended materials speckle only where they have no shade of their own to
 * spend - the variant already selects WHICH material, so position hash (as
 * for stone and wood) is the only variation left. Leaves get the wider range
 * since real foliage is a messier mix than ice, whose variation is facets on
 * one substance. The stem is deliberately OLIVE rather than green, closer to
 * wood's own colour, since every stem cell is on its way to becoming wood. */
#define PLANT_DARK         0x495422
#define PLANT_LIGHT        0x778746

/* Non-uniform green creates half canopy. Now one material. Gold-brown chain
 * removed for cost. */
#define LEAF_DARK          0x468F26
#define LEAF_LIGHT         0x8CD24E

#define ICE_DARK           0x93C9DE
#define ICE_LIGHT          0xDEF5FD

/* Metal: Like ice, minimal movement; position hash varies, simulating uniform
 * surface. */
#define METAL_DARK         0x7C8794
#define METAL_LIGHT        0xB9C4D2

/* ROOT_DARK is pale; ROOT_LIGHT matches METAL_LIGHT for consistency. */
#define ROOT_DARK          0xBFA58A
#define ROOT_LIGHT         0xDCC5A8

/* A root's shade comes from its live neighbour-root count (`depth`, see
 * material_colours()), not a stored age: age would fade fresh tips too, and
 * could never re-brighten a parent that loses a child to rot or lava.
 * ROOT_OLD stops short of wood's own colour so the oldest root reads as
 * root, not trunk. The grain's light end needs its OWN old colour
 * (ROOT_OLD_LIGHT), not ROOT_OLD too, or the oldest row's grain collapses to
 * one flat colour - see test_the_right_extended_materials_are_grained. */
#define ROOT_OLD           0x7A5535
#define ROOT_OLD_LIGHT     0x976D48
#define ROOT_SHADES        4

#define ROOT_STEP(k)       LERP(ROOT_DARK, ROOT_OLD, (k) * 15 / (ROOT_SHADES - 1))
#define ROOT_STEP_LIGHT(k) LERP(ROOT_LIGHT, ROOT_OLD_LIGHT, (k) * 15 / (ROOT_SHADES - 1))

/* 0 or 1: tip; 2: one child; 3: junction; >3: collar or thicket middle. */
static inline unsigned
root_shade(unsigned n) {
    return n <= 1u ? 0u : n == 2u ? 1u : n == 3u ? 2u : (ROOT_SHADES - 1u);
}

#define GRAIN8(lo, hi, k) GFX_RGB(LERP((lo), (hi), (k) * 15 / 7))

#define GRAIN8_ROW(lo, hi)                                                                                             \
    {GRAIN8(lo, hi, 0), GRAIN8(lo, hi, 1), GRAIN8(lo, hi, 2), GRAIN8(lo, hi, 3),                                       \
     GRAIN8(lo, hi, 4), GRAIN8(lo, hi, 5), GRAIN8(lo, hi, 6), GRAIN8(lo, hi, 7)}

static const gfx_color_t plant_grain[8] = GRAIN8_ROW(PLANT_DARK, PLANT_LIGHT);
static const gfx_color_t ice_grain[8] = GRAIN8_ROW(ICE_DARK, ICE_LIGHT);
static const gfx_color_t metal_grain[8] = GRAIN8_ROW(METAL_DARK, METAL_LIGHT);

/* HATCHED's only effect - lifted off METAL_LIGHT for uniform highlight. */
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

#ifdef __CPPCHECK__
/* Last table is behind us - see LERP's own stub above. */
#undef LERP
#define LERP(lo, hi, sh) LERP_RGB(lo, hi, sh)
#endif

/* liquid_spec[mask] corrects a flat fill-level ramp: two rim cells at the
 * same fill level need different shading depending on whether their open
 * side faces toward or away from gravity (a pool's lit top vs. an
 * overhang's shaded underside). Set once a frame by material_set_gravity(),
 * not read per cell, since material_colours() runs hot, per cell, per row.
 * Sized by MATERIAL_EDGE_MASK_COUNT (cardinal edges only), not
 * MATERIAL_VARIANTS - both are 16 today by coincidence, not a relationship
 * to rely on. */
static int8_t liquid_spec[MATERIAL_EDGE_MASK_COUNT];

/* Adjust if rim highlight is too strong or faint */
#define SPEC_STRENGTH 10

/* Rounds n/d to nearest integer. Plain division truncates, incorrect here:
 * weakens one side. */
static int
fx_round_div(int n, int d) {
    if (n >= 0) {
        return (n + d / 2) / d;
    }
    return -((-n + d / 2) / d);
}

/* Only three outward-normal cases can occur here, since there are only two
 * axes: no empty side (or a cancelling pair), one empty side, or two
 * adjacent ones (a diagonal of length sqrt(2)) - so norm_q8 just picks
 * between unit length and 1/sqrt(2) rather than a general vector length.
 * Liquid interior shading (`depth`/`wave`) is walked fresh per cell against
 * the live grid in paint_row_n() instead of here, since gravity's direction
 * alone can't predict a cell's local surroundings ahead of time. */
void
material_set_gravity(int gx, int gy) {
    const int len = im_len(gx, gy);
    if (len == 0) {
        /* No "up" for light, so no rim highlight. */
        for (unsigned m = 0; m < MATERIAL_EDGE_MASK_COUNT; m++) {
            liquid_spec[m] = 0;
        }
        return;
    }

    /* Unit vector of MINUS gravity, scaled by 256 for highlight catching. */
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
        const int norm_q8 = (nx != 0 && ny != 0) ? 181 : 256;
        const int spec_q8 = (raw_q8 * norm_q8) / 256; /* now in [-256,256] */

        /* Positive specular term subtracts from index; see
         * test_a_liquid_rim_catches_the_light_from_above. */
        liquid_spec[mask] = (int8_t)(-fx_round_div(spec_q8 * SPEC_STRENGTH, 256));
    }
}

void
material_shine_direction(int gx, int gy, int* ux_q8, int* uy_q8) {
    const int len = im_len(gx, gy);
    if (len == 0) {
        *ux_q8 = 181;
        *uy_q8 = 181;
        return;
    }
    *ux_q8 = (-(gx + gy) * 181) / len;
    *uy_q8 = ((gx - gy) * 181) / len;
}

/* Perpendicular to gravity, not a fixed grid axis - so the wood-leaf wind
 * (material_wood_leaf_wave()) sweeps level on the panel no matter how the
 * device is held, the same reasoning material_shine_direction() already
 * uses for its own band. Flat: defaults to grid-x. */
void
material_wood_leaf_wind_axis(int gx, int gy, int* ux_q8, int* uy_q8) {
    const int len = im_len(gx, gy);
    if (len == 0) {
        *ux_q8 = 256;
        *uy_q8 = 0;
        return;
    }
    *ux_q8 = (-gy * 256) / len;
    *uy_q8 = (gx * 256) / len;
}

/* Ring order matches sand_priv.h's own ring_dir() (0 = down, clockwise) -
 * duplicated here since the render path does not reach into simulation
 * internals for it. */
static const int8_t wood_leaf_ring[8][2] = {
    {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1},
};

/* Recomputing fresh every frame flips right at a tie between neighbouring
 * ring directions, popping every wood cell whose top5 just changed in one
 * frame - see Shading-and-Colour.md, "hysteresis hides the seam, it does
 * not remove it". `*last_down` is the caller's own state, like
 * glass_last_phase. */
void
material_wood_leaf_top5(int gx, int gy, int* last_down, int8_t top5[5][2]) {
    const int len = im_len(gx, gy);
    const long margin = len / 4;
    int down = *last_down & 7;
    long best = (long)wood_leaf_ring[down][0] * gx + (long)wood_leaf_ring[down][1] * gy;
    for (int i = 0; i < 8; i++) {
        const long dot = (long)wood_leaf_ring[i][0] * gx + (long)wood_leaf_ring[i][1] * gy;
        if (dot > best + margin) {
            best = dot;
            down = i;
        }
    }
    *last_down = down;

    const int down_left = (down + 7) & 7;
    const int down_right = (down + 1) & 7;
    int n = 0;
    for (int i = 0; i < 8; i++) {
        if (i == down || i == down_left || i == down_right) {
            continue;
        }
        top5[n][0] = wood_leaf_ring[i][0];
        top5[n][1] = wood_leaf_ring[i][1];
        n++;
    }
}

/* Foam is gated by rim curvature instead of a separate motion flag: a still
 * rim already reads as flat and a sloshing one as curved throughout, so a
 * flag would only re-derive what curvature already answers. See the
 * ADD-not-XOR comment below for how foam_phase animates the dither. No
 * dirty-row exception in draw_dirty_rows(): curvature is nonzero only while
 * the liquid is moving, which already marks the row dirty. */

/* FOAM's color. No extra slot. Rim dithering works. Brighter than 0x77C4E8
 * for visibility on water. */
static const gfx_color_t water_foam = GFX_RGB(0xE8F6FF);

/* Max 5 to prevent corners appearing more foamed. */
#define WATER_FOAM_CURVATURE_MAX 3

/* Curvature 0 stays flat, never foams (test_a_flat_rim_still_never_foams in
 * suite_sand_foam.c). */
static const uint8_t water_foam_threshold[WATER_FOAM_CURVATURE_MAX + 1] = {
    0, /* curvature 0, flat   - no foam at all */
    3, /* curvature 1, light  - foams on 3 of 8 hash values */
    5, /* curvature 2, medium - foams on 5 of 8 */
    7, /* curvature 3+, heavy - foams on 7 of 8 */
};

/* See material_set_foam_phase() in material_palette.h. Zero until first frame sets
 * it. */
static unsigned foam_phase;

void
material_set_foam_phase(unsigned phase) {
    foam_phase = phase;
}

/* Cullet phase: see material_set_cullet_phase() in material_palette.h. Starts at
 * zero, setting anchor colors; other phases use static entries. */
static unsigned cullet_phase;

void
material_set_cullet_phase(unsigned phase) {
    cullet_phase = phase;
}

/* Zero until first frame sets it */
static int glass_phase;

void
material_set_glass_phase(int phase) {
    glass_phase = phase;
}

/* A short gust, not a slow ramp - a symmetric triangle read as one broad
 * pulse. SCREEN_SPAN_MS is a multiple of PERIOD_MS so several bands show
 * at once. `hash` salts each cell's phase - see glass's own `(hash & 0xFF)
 * + glass_phase`. */
#define WOOD_LEAF_WAVE_PERIOD_MS        600u
#define WOOD_LEAF_WAVE_RISE_MS          60u
#define WOOD_LEAF_WAVE_FALL_MS          140u
#define WOOD_LEAF_WAVE_SCREEN_SPAN_MS   4000u
#define WOOD_LEAF_WAVE_SALT_MS          150u

/* Percent chance a cell actually shows a gust it is otherwise due for -
 * every eligible cell lighting up together read as one shine sweeping
 * through, not real wind, which is patchy. Rolled per gust (not once,
 * ever), so which cells sit one out changes gust to gust. */
#define WOOD_LEAF_WAVE_ACTIVATE_PERCENT 30u

unsigned
material_wood_leaf_wave(uint32_t time_ms, int pos, int span, unsigned hash) {
    const int32_t shift_ms = span != 0 ? (int32_t)(((int64_t)pos * WOOD_LEAF_WAVE_SCREEN_SPAN_MS) / span) : 0;
    const uint32_t salt_ms = hash % WOOD_LEAF_WAVE_SALT_MS;
    const uint32_t shifted = time_ms + (uint32_t)shift_ms + salt_ms;
    const uint32_t m = shifted % WOOD_LEAF_WAVE_PERIOD_MS;
    if (m >= WOOD_LEAF_WAVE_RISE_MS + WOOD_LEAF_WAVE_FALL_MS) {
        return 0u;
    }

    const uint32_t cycle = shifted / WOOD_LEAF_WAVE_PERIOD_MS;
    if ((hash ^ (cycle * 0x9E3779B1u)) % 100u >= WOOD_LEAF_WAVE_ACTIVATE_PERCENT) {
        return 0u;
    }

    if (m < WOOD_LEAF_WAVE_RISE_MS) {
        return (unsigned)((m * 255u) / WOOD_LEAF_WAVE_RISE_MS);
    }
    const uint32_t since_peak = m - WOOD_LEAF_WAVE_RISE_MS;
    return (unsigned)(255u - (since_peak * 255u) / WOOD_LEAF_WAVE_FALL_MS);
}

/* No floating point, suitable for water rim cells. See paint_row_n(). */
static unsigned
material_popcount8(unsigned mask) {
    unsigned count = 0;
    for (unsigned bit = 0; bit < 8u; bit++) {
        count += (mask >> bit) & 1u;
    }
    return count;
}

/* Index 4 tweaks gradient; 54-85 luminance, no rim. Adjust as needed. */
#define DEPTH_RANGE          4

/* Local depth for full darkening - clamped to material's body colour. */

/* SHARED with sand_liquid.c - must be 24 to prevent drift. */
#define DEPTH_SATURATE_CELLS MATERIAL_LIQUID_DEPTH_BAND

material_pattern_t
material_colours(cell_t c, unsigned hash, unsigned mask, unsigned depth, gfx_color_t out[3]) {
    const uint8_t v = CELL_VARIANT(c);

    /* A LIQUID's full body colour is always painted, as fill level is a
     * SOLVER transient (see material.h). */

    /* Flat colour hides tilt comb. */

    /* FLAT INTERIOR READS AS FLAT: `depth` is a new cue. */

    /* depth shifts index toward BRIGHT end by DEPTH_RANGE steps, removes
     * water special case */

    /* See DEPTH_SATURATE_CELLS for range mismatch bug. Wave bands caused axis
     * seam issues. */

    /* LOCAL DEPTH, NOT SCREEN POSITION. Clamped at 255. */

    /* Obstacle shows dip, not band. */

    /* Staleness under dirty-row optimisation; see
     * local_depth_row_a[]/local_depth_row_b[] in app_sand.c. */

    if (material_of(c)->kind == KIND_LIQUID) {
        const uint8_t id = CELL_MATERIAL(c);
        const unsigned cardinal = mask & MATERIAL_EDGE_CARDINAL;

        if (cardinal == 0) {
            /* Prevent unsigned wrap-around when `depth` exceeds
             * DEPTH_SATURATE_CELLS. */
            const unsigned depth_capped = depth < DEPTH_SATURATE_CELLS ? depth : DEPTH_SATURATE_CELLS;

            const int bright =
                ((int)DEPTH_RANGE * (int)(DEPTH_SATURATE_CELLS - depth_capped)) / (int)DEPTH_SATURATE_CELLS;
            int idx = (int)MASS_MAX - bright;
            idx = idx < 0 ? 0 : (idx > MASS_MAX ? MASS_MAX : idx);
            out[0] = palette[CELL_MAKE(id, (uint8_t)idx)];
        } else {

            /* rim cell uses fill-indexed lookup shifted by liquid_spec[]
             * indexed by CARDINAL bits; see material_set_gravity() and
             * MATERIAL_EDGE_CARDINAL in material_palette.h. */
            int idx = (int)v + liquid_spec[cardinal];
            idx = idx < 0 ? 0 : (idx > MASS_MAX ? MASS_MAX : idx);
            out[0] = palette[CELL_MAKE(id, (uint8_t)idx)];

            if (id == MAT_WATER) {
                const unsigned empty_count = material_popcount8(mask);
                unsigned curvature = (empty_count > 3) ? (empty_count - 3) : (3 - empty_count);
                if (curvature > WATER_FOAM_CURVATURE_MAX) {
                    curvature = WATER_FOAM_CURVATURE_MAX;
                }

                /* WARNING: Safe ONLY because foam is SOLE consumer of water's
                 * hash. */

                /* ADD, not XOR: XOR maps a power-of-two-aligned threshold
                 * window either onto itself or another aligned window
                 * depending only on phase's low bits, so about half of all
                 * phase steps left the foam set unchanged - addition has no
                 * such alignment to preserve. */
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
            if (v < SAND_CULLET_BASE) {
                break;
            }
            const unsigned i = ((v - SAND_CULLET_BASE) * (CULLET_CYCLE_LEN / SAND_CULLET_SHADES) + cullet_phase)
                               & (CULLET_CYCLE_LEN - 1);

            /* GLINT: Flash white; mix uses hash & phase, not RNG. Rarity
             * controlled by CULLET_GLINT_ONE_IN. */
            const bool glint = ((hash + cullet_phase * 0x9E37u) % CULLET_GLINT_ONE_IN) == 0;
            out[0] = glint ? CULLET_GLINT : cullet_cycle[i];
            out[1] = out[0];
            out[2] = out[0];
            return MATERIAL_FLAT;
        }
        case MAT_EXTENDED:
            /* Switched on low nibble for identity - see MATX(). Non-grained
             * fall through. */

            /* Metal returns HATCHED pattern, not compatible with ternary's
             * MATERIAL_SPECKLED. */

            if (v == MATX_METAL) {
                out[0] = metal_grain[hash & 7u];
                out[1] = out[0];
                out[2] = metal_shine;
                return MATERIAL_HATCHED;
            }

            if (v == MATX_LEAF) {
                /* depth carries the wave's fraction (0-255) plus one here
                 * too - the same live sweep MAT_WOOD's near-leaf case
                 * reads, so a leaf and the wood beside it catch the same
                 * gust together. No stored grain table: LERP8 needs
                 * 0xRRGGBB, not a packed gfx_color_t (see GLASS's own
                 * note on this exact trap). */
                const unsigned frac = depth != 0 ? depth - 1u : 0u;
                const uint32_t base = LERP(LEAF_DARK, LEAF_LIGHT, (hash & 7u) * 15 / 7);
                out[0] = GFX_RGB(LERP8(base, WOOD_LEAF_TINT_HI, frac));
                out[1] = out[0];
                out[2] = out[0];
                return MATERIAL_SPECKLED;
            }

            /* Guard-plus-ternary: switch costs 14%, unhinted branch 26% - see
             * Tuning-At-a-Glance.md. */
            if (v == MATX_PLANT || v == MATX_ICE || v == MATX_ROOT) {
                out[0] = (v == MATX_PLANT) ? plant_grain[hash & 7u]
                         : (v == MATX_ICE) ? ice_grain[hash & 7u]
                                           : root_grain[root_shade(depth)][hash & 7u];
                out[1] = out[0];
                out[2] = out[0];
                return MATERIAL_SPECKLED;
            }
            break;
        case MAT_GLASS: {
            /* `mask != 0` wrong; see MATERIAL_EDGE_CARDINAL. */
            const bool edge = (mask & MATERIAL_EDGE_CARDINAL) != 0;

            /* glass_phase slides starting points, drifting together */

            /* Fine enough to move by a small angle without a table. */
            const unsigned frac = (unsigned)(((int)(hash & 0xFFu) + glass_phase) & 0xFF);

            /* uint32_t, not gfx_color_t - `base` is 0xRRGGBB, not packed by
             * GFX_RGB(). gfx_color_t drops red byte, causing "glass reads
             * green under heat". */
            const uint32_t base = edge ? GLASS_EDGE_RGB(v) : GLASS_RGB(v);
            out[0] = GFX_RGB(LERP8(base, GLASS_GRADIENT_HI(v), frac));
            out[1] = out[0];
            out[2] = out[0];
            return MATERIAL_SPECKLED;
        }
        case MAT_STONE:
            /* See MATERIAL_EDGE_CARDINAL's comment in material_palette.h. */
            out[0] =
                ((mask & MATERIAL_EDGE_CARDINAL) != 0) ? stone_edge_speckle[v][hash & 7u] : stone_speckle[v][hash & 7u];
            out[1] = out[0];
            out[2] = out[0];
            return MATERIAL_SPECKLED;
        case MAT_WOOD:
            if (v != 0) {
                break; /* alight: one flat glow, not grain */
            }
            if (depth != 0) {
                /* depth carries the wave's fraction (0-255) plus one, from
                 * material_wood_leaf_wave() via paint_row_n() - see
                 * material_wood_near_leaf() in material_palette.h for the
                 * gate. A live LERP8, not a stored step, so the blend is
                 * smooth rather than snapping between fixed shades. */
                out[0] = GFX_RGB(LERP8(WOOD_LEAF_TINT_LO, WOOD_LEAF_TINT_HI, depth - 1u));
                out[1] = out[0];
                out[2] = out[0];
                return MATERIAL_FLAT;
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

const gfx_color_t*
material_palette(void) {
    return palette;
}
