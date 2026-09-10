/*=============================================================================
 * material - what a cell is made of, and how that makes it behave.
 *
 * Pure data. The simulation reads this table and has no idea what "water" is;
 * adding a material is a row here rather than a branch in the movement code,
 * which matters because that code is the tightest loop in the project.
 *
 * ONE BYTE PER CELL, AND WHY
 *
 * The grid is 184 x 224. At one byte per cell it is 41 KB; at two it is 82 KB,
 * against the ~92 KB left after the framebuffer - which would leave nothing for
 * stacks or the UI. So a cell is one byte and always will be:
 *
 *     high nibble   material, 0 meaning empty, so 15 materials
 *     low nibble    a variant - see below
 *
 * WHAT THE LOW NIBBLE MEANS DEPENDS ON THE MATERIAL
 *
 * For a powder it is a SHADE, so a pile has texture rather than reading as one
 * flat block of colour. For a LIQUID it is a FILL LEVEL, 1 to 15 - how much
 * water is in that cell. For a transient material - gas, fire, steam, ember -
 * it will be LIFE REMAINING, counting down to nothing.
 *
 * The fill level is what lets water level itself using nothing but its
 * immediate neighbours. A cell that is either full or empty cannot split, so a
 * full cell beside an empty one has no legal move and a wide pool freezes into
 * a staircase - that is a genuine fixed point of any local rule, not a bug in
 * one. Give the cell an amount and the same pair becomes 15 and 0, averages to
 * 8 and 7, and the difference spreads outward one neighbour at a time.
 *
 * For GLASS it is HEAT, 0 to 15 - see reaction_t.heat_ramp. Glass is static,
 * so its nibble was a shade and nothing read it. Heat is the one piece of
 * per-cell state this simulation would otherwise have no room for: a second
 * byte across the grid is 41 KB it does not have. One material can afford it
 * because one material was not using its nibble.
 *
 * That overlap is deliberate. Transient materials are exactly the ones that
 * need per-cell state, and per-cell state is the one thing there is no room
 * for. Reusing the nibble costs nothing, and it makes fire fade as it burns
 * out, which looks better than a random shade would.
 *
 * EVERYTHING ELSE LIVES IN FLASH
 *
 * The table below is `const`, so it lands in .rodata and is memory-mapped from
 * flash: it costs zero RAM. The board has ~12.8 MB of flash spare and ~50 KB of
 * RAM, so anything that can be a constant should be one - including lookups
 * that would otherwise be computed. See material_palette().
 *===========================================================================*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*---------------------------------------------------------------------------
 * Cells
 *-------------------------------------------------------------------------*/

typedef uint8_t cell_t;

/* A liquid cell holds 1-15; zero is not a very empty cell, it is no cell at
 * all and must be written as CELL_EMPTY, or the material nibble leaves an
 * occupied cell holding nothing. */
#define CELL_EMPTY          ((cell_t)0)
#define CELL_MATERIAL(c)    ((uint8_t)((c) >> 4))
#define CELL_VARIANT(c)     ((uint8_t)((c) & 0x0F))
#define CELL_MAKE(m, v)     ((cell_t)(((uint8_t)(m) << 4) | ((uint8_t)(v) & 0x0F)))

/* Tests nibble for zero variants. */
#define CELL_IS_EMPTY(c)    (((c) & 0xF0) == 0)

#define MATERIAL_VARIANTS   16

/* STATE determines if nibble holds TONE or MOISTURE. */


/* Corrupt cell reads SOIL_MOISTURE_MAX, not unpredictable eighth colour. */





/* Tone and moisture in SOIL_SHADES create a luminance ramp from bone-dry to
 * saturated, no unrelated colors. */
#define SOIL_DRY_TONES      8
#define SOIL_MOISTURE_MAX   7

#define CELL_MOISTURE(c)                                                 \
    ((uint8_t)(CELL_VARIANT(c) < SOIL_DRY_TONES                          \
                   ? 0u                                                  \
                   : (CELL_VARIANT(c) - (SOIL_DRY_TONES - 1u) >          \
                              SOIL_MOISTURE_MAX                          \
                          ? SOIL_MOISTURE_MAX                            \
                          : CELL_VARIANT(c) - (SOIL_DRY_TONES - 1u))))

#define CELL_SOIL_TONE(c)   CELL_VARIANT(c)

/* NEVER use m == 0; use soil_dry_out() for varied drying. */
#define CELL_WITH_MOISTURE(c, m)                                          \
    CELL_MAKE(CELL_MATERIAL(c),                                          \
              (uint8_t)((SOIL_DRY_TONES - 1u) + ((m) & SOIL_MOISTURE_MAX)))

/* Caller may ask for m=0 with specific tone. Tone ignored if m > 0. */
#define CELL_SOIL(mat, tone, m)                                           \
    CELL_MAKE((mat), (uint8_t)((m) != 0                                  \
                                    ? (SOIL_DRY_TONES - 1u) +             \
                                          ((m) & SOIL_MOISTURE_MAX)       \
                                    : ((tone) & (SOIL_DRY_TONES - 1u))))


#define SAND_AMBIENT_HEAT 3

/* Lives here, not in sand_reactions.c, since the shatter rule, glass's
 * palette (which changes colour at exactly this level, making "will
 * shatter" visible) and the tests all have to agree on it - a private
 * #define once let the number and the colour drift apart. Set to ambient+2,
 * not a more dramatic ambient+4: the glass a snowflake actually touches
 * sits near ambient, several cells from the heat and actively chilled by
 * the snow itself, so ambient+4 asked for heat the scene can never reach. */
#define SAND_SHOCK_HEAT (SAND_AMBIENT_HEAT + 2)

#define SAND_SHOCK_COLD (SAND_AMBIENT_HEAT - 2)

#define MASS_MAX 15

#define MATERIAL_LIQUID_DEPTH_BAND 24

/*---------------------------------------------------------------------------
 * Materials
 *-------------------------------------------------------------------------*/

typedef enum {
    MAT_EMPTY = 0,
    MAT_SAND,
    MAT_WATER,
    MAT_STONE,
    MAT_GAS,
    MAT_FIRE,
    MAT_WOOD,
    MAT_STEAM,
    MAT_SMOKE,
    MAT_OIL,
    MAT_LAVA,
    MAT_ACID,
    MAT_GLASS,
    MAT_SNOW,
    MAT_DIRT,
    MAT_COUNT,

    /* MAT_EXTENDED (id 15) is an escape hatch, not a material: its low
     * nibble names one of sixteen CODES. Eight are inert STATICS
     * (0xF0-0xF7); the other eight (0xF8-0xFF) are gunpowder, spending its
     * whole range on one material's variant instead of eight more
     * materials. Free in the sweep since materials[] reads by cell>>3, so
     * each half already shares one row. Statics get no physics (shared row)
     * or variant (their bits ARE their identity); gunpowder alone escapes
     * both, at the cost of half the range. */
    MAT_EXTENDED = 15   /* the last nibble value; asserted against
                         * MATERIAL_MAX below, which this enum comes
                         * too early to reference */
} material_id_t;

/* Unused slots dense, block simulations. */
#define MATERIAL_MAX 16

/* Cullet (glass turned to sand) costs no bits: sand's variant is already a
 * shade, so marking "this came from a pane" spends four of sixteen shades,
 * no more. Needed since glass at the ramp's plain top didn't read - one
 * flat colour, and "brightest sand" still looked like sand. Cullet gets a
 * different HUE instead (pale, glass-coloured); the nibble no longer names
 * a fixed colour but which quarter of a shared, slowly-advancing cycle a
 * grain starts at, so a heap shimmers through several pale tints. */
#define SAND_DUNE_SHADES    12
#define SAND_CULLET_BASE    SAND_DUNE_SHADES
#define SAND_CULLET_SHADES  (MATERIAL_VARIANTS - SAND_CULLET_BASE)

/* Public for tests. Mask use. Quarter-turn alignment. */
#define CULLET_CYCLE_LEN 16

/* PAINTED grain shades limit; sand skips reserved; dirt skips wet (see
 * random_cell() in sand.c). */
#define MATERIAL_SHADE_SPAN(m)                                            \
    ((m) == MAT_SAND ? SAND_DUNE_SHADES                                  \
                      : (m) == MAT_DIRT ? SOIL_DRY_TONES : MATERIAL_VARIANTS)

/* MAT_EXTENDED's lower half: 8 STATIC materials (0xF0-0xF7). GUNPOWDER_BASE
 * uses 0xF8-0xFF. */
#define MATERIAL_EXTENDED_COUNT 8

#define MATERIAL_EXTENDED_CODES 16

_Static_assert(MAT_EXTENDED == MATERIAL_MAX - 1,
               "the extended range lives in the LAST nibble value, so that "
               "every ordinary material keeps the id it already had");
_Static_assert(MAT_COUNT <= MAT_EXTENDED,
               "an ordinary material has taken the extended range's slot - "
               "there is no room for both");
_Static_assert(MATERIAL_EXTENDED_CODES == MATERIAL_VARIANTS,
               "extended_reactions[]/extended_names[] are sized by the "
               "whole low nibble - one entry per byte value 0xF0-0xFF, the "
               "same range CELL_VARIANT() already covers");

typedef enum {
    KIND_NONE = 0,  /* empty space */
    KIND_STATIC,    /* never moves; costs nothing, the loop skips it at once */
    KIND_POWDER,    /* falls, and piles at an angle of repose */
    KIND_LIQUID,    /* falls, but spreads flat instead of piling */
    KIND_GAS,       /* rises, and disperses */
} material_kind_t;

/* Compact entry avoids cache misses. Single palette source; duplicates cause
 * errors. */
typedef struct {
    uint8_t kind;      /* material_kind_t, narrowed - an enum is int-sized */

    uint8_t density;

    /* Zero locks material, 256 means no load obstacle. */
    uint8_t slip;

    uint8_t repose;

    uint8_t scatter;

    /* 1/256 chance per step. Zero=immortal. Decay ≠ 0 enables
     * variant-as-life, not kind. */
    uint8_t decay;

    /* Ignored while jostled. Doubles as gas BUOYANCY and, inverted, liquid
     * VISCOSITY (see step_one_gas_grain(), move_liquid_grain()) rather than
     * each getting its own field: material_t is read per cell per step, so
     * a ninth byte would push its stride from 12 to 16 on the 32-bit
     * target. Powders leave it at zero and use `slip`/`repose` instead. */
    uint8_t mobility;

    /* Per-material gas spread limit; zero for non-gas materials. */
    uint8_t sight;

    /* Cold: last, avoids cache line push. */
    const char *name;
} material_t;

/* MAT_EXTENDED expands HOT TABLE to 32 rows. 0xF0-0xF7 static, 0xF8-0xFF
 * POWDER physics. ORDINARY materials duplicated. */
#define MATERIAL_ROWS 32

_Static_assert(MATERIAL_ROWS == MATERIAL_MAX * 2,
               "one twin pair per ordinary material plus the extended "
               "nibble's own pair (static half, gunpowder half) - the hot "
               "table is exactly twice MATERIAL_MAX rows, never a fixed "
               "32 that could quietly stop matching it");

/* MAT_EXTENDED's `+1` is gunpowder, see GUNPOWDER_BASE. */
#define MATERIAL_ROW(id) ((unsigned)(id) << 1)

extern const material_t materials[MATERIAL_ROWS];

static inline const material_t *material_of(cell_t c)
{
    return &materials[(c) >> 3];
}

/* Avoids CELL_MATERIAL(cell), use material_of(). MAT_EXTENDED reads as static
 * row, matching old id 15. */
static inline const material_t *material_by_id(material_id_t m)
{
    return &materials[MATERIAL_ROW(m)];
}

/* Separate table avoids cache line overflow. Minimize stride. Two rows may
 * add a material. */
typedef struct {
    /* Chance in 256 for burning. 0=never, 255=instant, no RNG. */
    uint8_t flammability;

    /* MAT_EMPTY treated as MAT_FIRE. */
    uint8_t ignites_to;

    /* Zero for every material but gunpowder; nonzero is gunpowder's BLAST
     * RADIUS, read only in step_one_burning_cell() (sand_reactions.c),
     * never at ignition time. A fully-lit 2x2 (not the rarer 3x3 first
     * tried) triggers sand_explode() at this radius instead of decaying to
     * plain fire - see SAND_GUNPOWDER_BLAST_RADIUS for why 16. Earlier
     * versions blasted the instant a spark touched one grain, which
     * measured as gunpowder spending nearly every blast on itself - see
     * docs/sand/Impulse-Mechanics.md. */
    uint8_t explodes;

    /* Only what touches air burns. */
    uint8_t needs_air;

    uint8_t burns;

    uint8_t burn_decay;

    uint8_t lit_from;

    /* Heat spreads at 1/256 chance per step per burning neighbor, decreasing
     * exponentially with depth d: (conducts/256)^d. Zero chance for
     * non-conductors or materials not between fire and cooler surroundings. */
    uint8_t conducts;

    uint8_t boils;

    /* Acid overrides to MAT_GAS due to conducted-heat boiling hardcoded to
     * steam. */
    uint8_t boils_to;

    /* MAT_SMOKE and MAT_STEAM distinct for display; see sand_reactions.c. */
    uint8_t residue;

    uint8_t quench_to;

    uint8_t flare;

    uint8_t dissolves;

    uint8_t dissolvable;

    /* Overrides materials[]'s shared density for the dislodge-toughness
     * roll (queue_flying_grain(), sand_impulse.c) - the only place an
     * extended static's own density matters, since every extended code
     * otherwise inherits one shared materials[] row. 0 = no override. */
    uint8_t dislodge_density;

    /* Separate from `residue` to avoid overload. Smoke means generic
     * destruction, not steam or acid. */
    uint8_t fizz;

    uint8_t evaporates;

    /* Not a real model; just cosmetic. See step_one_condensing_cell() in
     * sand_reactions.c. */
    uint8_t condenses;

    uint8_t condenses_to;

    /* Separate `flammability`/`ignites_to` for sand. Activated by direct
     * contact or conductors. */
    uint8_t heats_to;
    uint8_t heat_chance;

    uint8_t melts;

    uint8_t flaw_to;
    uint8_t flaw_chance;

    uint8_t spoils_to;
    uint8_t spoils_chance;

    /* Non-zero `heat_ramp` banks heat in the variant instead of transforming
     * on contact, climbing with `heat_chance` toward `heats_to`; `cools`
     * drains it back. The ratio between them decides whether a fire can
     * ever win at all - see test_a_lone_flame_never_melts_glass. */
    uint8_t heat_ramp;
    uint8_t cools;

    /* `chills` is on the COLD material's row, `cools` on the HOT one - kept
     * separate rather than one field re-read by kind, the same mistake
     * `mobility`/`sight` avoid elsewhere. They can't collapse to one number
     * either: snow's chills (40) has to sit far above glass's own cools (6),
     * or a chilling neighbour reusing the hot cell's own rate would mean
     * snow could never win a race against even one flame - that gap is the
     * mechanic thermal shock depends on. */
    uint8_t chills;

    uint8_t warms;

    uint8_t thaws;

    /* Per-step chance per adjacent liquid cell. Liquid is consumed.
     * `soaks_to`: non-zero changes cell; zero keeps material, raises variant.
     * `wets`: whether a LIQUID soaks into anything at all (water does; oil,
     * lava, acid don't) - wetness, not fluidity, is what decides it. */
    uint8_t wets;

    uint8_t soaks;
    uint8_t soaks_to;

    uint8_t tones;
    uint8_t moist_max;

    /* WHETHER A PLANT MAY TREAT THIS AS SOIL - distinct from `dries`. Only
     * dirt sets this. */
    uint8_t soil;

    /* Marks material's moisture variant. Zero means bone dry. */
    uint8_t dries;

    /* Gunpowder turns to oil: one grain plus water becomes one oil cell,
     * ignoring mass. */
    uint8_t soaked_to;
    uint8_t soaked_chance;

    /* Grows against gravity from column top. Growth from tip creates tree,
     * not shrub. */
    uint8_t grows;

    /* KIND_POWDER falls into empty cells; seeds drop, stems remain upright. */
    uint8_t falls;

    /* Hardens to align cells, makes tall stems trunks. Creeper stays soft.
     * Burns as wood, catches fire, rain keeps growth soft. */
    uint8_t hardens_to;
    uint8_t harden_run;

    /* Lower values make trees greener and shaggier, higher values woodier and
     * squatter. */
    uint8_t harden_chance;

    /* PART OF: other material in a tree. `hardens_to` now only for hardening. */
    uint8_t clings_to;

    /* ROOTING is one field, two readings. On a GROWER: spending moisture
     * welds the collar into `roots_to` (one-time, gated to
     * `root_depth == 0`), anchoring the tree if the dirt beneath it later
     * shifts - without it, find_water()'s stem walk finds neither stem nor
     * ground and strands the tree. On ROOT ITSELF: a root can convert a
     * touching moist dirt cell into root - the conversion IS the cost, since
     * becoming root discards its moisture. A root with several root
     * neighbours doesn't roll (ROOT_SURFACE_MAX). */
    uint8_t roots;
    uint8_t roots_to;

    uint8_t canopy;
    uint8_t canopy_to;

    uint8_t trunk_girth;

    /* HOLDING A LINE: chance in 256 for growth direction */
    uint8_t holds_line;

    /* SPROUTING lets a trunk regrow after losing its foliage (fire, acid, a
     * landslide): wet ground at the foot buds again even after hardening
     * has consumed every growable cell above. `sprouts_to` is a cell SPEC,
     * not a material id, like `shatters_to`. */
    uint8_t sprouts;
    uint8_t sprouts_to;

    /* BUDDING makes growth an EVENT, not a population: a finished tree has
     * no growable cells left, so only budding from wood (`already in leaf`,
     * a `sprouts_to` cell alongside) restarts it - a roughly constant
     * handful per tree rather than every cell of it. */
    uint8_t buds;
    uint8_t buds_to;

    uint8_t drinks;

    /* MAT_SAND closes loop; glass to sand when shocked. */
    uint8_t shatters_to;

    /* SETTLED-ONLY: a cell at rest slowly becomes crusts_to. Rolled against
     * CRUST_ROLL_MAX, not the usual 256, because this is the one rate a whole
     * bank pays at once.
     *
     * The denominator used to be 65536, chosen when EVERY settled cell was
     * eligible. Only cells with a face on another material are now, which is
     * a thin skin rather than the bank, and against that population 65536 put
     * the fastest expressible rate - this field is a byte, so 255 - at over an
     * hour to convert a 32-cell cover. */
    uint8_t crusts;
    uint8_t crusts_to;

    /* NEVER READ. Rounds the row to 64 bytes so reaction_of()'s index is one
     * shift: at 61 GCC strength-reduces the stride into slli/sub/slli/add,
     * four ALU ops at every call site. Declared rather than left to the
     * compiler because sizeof(reaction_t) is this table's documentation
     * contract - dump_reactions.c insists every byte is exactly one
     * documented field - and silent padding would be an undocumented hole. */
    uint8_t stride_pad2;
    uint8_t stride_pad3;
    uint8_t stride_pad4;
} reaction_t;

/* Denominator of the crusts roll. A power of two so the read site masks. */
#define CRUST_ROLL_MAX 1024

_Static_assert(sizeof(reaction_t) == 64, "reaction_of()'s stride must stay a power of two - resize stride_pad");

/* Zero means acid immune, heat block; check fields. */
extern const reaction_t reactions[MATERIAL_MAX];

/* Rows not given are all-zero; see reaction_t note on zero meaning. */
extern const reaction_t extended_reactions[MATERIAL_EXTENDED_CODES];

/* The extended materials, by low nibble. */
typedef enum {
    MATX_ICE = 0,
    MATX_PLANT,
    MATX_LEAF,

    /* METAL: see docs/sand/Metal.md. Slot 5, now 3. 11 slots
     * remain. */
    MATX_METAL,

    /* ROOT: see reaction_t.roots. Wood shows burn. ROOT uses a slot; 10 slots
     * left. */
    MATX_ROOT,
} material_extended_t;

_Static_assert(MATX_ROOT < MATERIAL_EXTENDED_COUNT,
               "a static's low nibble must fit in the 0-7 range MATX() now "
               "masks to - the upper half of the nibble is gunpowder's");

const char *material_name(cell_t c);

/* Masks to 0x07, not 0x0F, to avoid gunpowder's half. */
#define MATX(k) ((cell_t)((MAT_EXTENDED << 4) | ((k) & 0x07)))

/* GUNPOWDER. MATERIAL_ROWS explains split. MAT_EXTENDED high nibble, low bit
 * 3 set. */
#define GUNPOWDER_BASE ((cell_t)((MAT_EXTENDED << 4) | 0x08))
#define GUNPOWDER_CELL(v) ((cell_t)(GUNPOWDER_BASE | ((v) & 0x07)))

_Static_assert((GUNPOWDER_BASE >> 3) == MATERIAL_ROW(MAT_EXTENDED) + 1,
               "gunpowder's row has to be the upper half of MAT_EXTENDED's "
               "twin pair - material_of()'s cell >> 3 and this constant's "
               "own bit-3 split have to land on the exact same row or the "
               "hot table and the byte layout silently disagree");

/* GUNPOWDER_REACTION's `.tones`/`.moist_max` used here for GUNPOWDER_LIT
 * check. */
#define GUNPOWDER_TONES 3
#define GUNPOWDER_MOIST_MAX 4

#define GUNPOWDER_LIT 7
#define GUNPOWDER_LIT_CELL GUNPOWDER_CELL(GUNPOWDER_LIT)

_Static_assert(GUNPOWDER_LIT == GUNPOWDER_TONES + GUNPOWDER_MOIST_MAX,
               "GUNPOWDER_LIT has to sit exactly one past every dry tone "
               "and every moisture level - moisture_of()'s clamp and "
               "cell_is_burning()'s own threshold both rely on nothing "
               "between the wet codes and the lit one");

/* Gunpowder's largest blast. See `reaction_t.explodes`. Core size: radius /
 * 5. */
#define SAND_GUNPOWDER_BLAST_RADIUS 20

/* Gunpowder and cell_is_extended() below partition nibble 15: every
 * gunpowder byte tests true here and false there, never both for the same
 * byte, whatever its 3-bit variant. */
static inline bool cell_is_gunpowder(cell_t c)
{
    return (c & 0xF8) == GUNPOWDER_BASE;
}

static inline bool cell_is_extended(cell_t c)
{
    return (c & 0xF8) == (uint8_t)(MAT_EXTENDED << 4);
}

/* Cullet shares MAT_SAND's reaction row, so anything true of cullet alone
 * has to be asked per cell rather than read out of reactions[]. */
static inline bool cell_is_cullet(cell_t c)
{
    return CELL_MATERIAL(c) == MAT_SAND && CELL_VARIANT(c) >= SAND_CULLET_BASE;
}

/* KIND_STATIC/NONE excluded: a static source buries itself on its first
 * emitted cell and jams forever. See
 * test_the_extended_row_being_static_is_what_emitter_eligibility_leans_on
 * in suite_sand_roots.c. */
static inline bool material_can_emit(cell_t c)
{
    const uint8_t kind = material_of(c)->kind;
    return kind == KIND_POWDER || kind == KIND_LIQUID || kind == KIND_GAS;
}

/* GENERIC CELL-CODE HELPERS. Shared 0xF0-0xFF range with extended statics,
 * split by bit 3 (GUNPOWDER_BASE). Unified moisture codec for dirt and
 * gunpowder. Defined before reaction_of()/cell_is_burning(). */
static inline uint8_t cell_code(cell_t c)
{
    return cell_is_gunpowder(c) ? (uint8_t)(c & 0x07) : CELL_VARIANT(c);
}

/* Keeps identity bits, `v` masked to codec width. */
static inline cell_t cell_with_code(cell_t c, uint8_t v)
{
    return cell_is_gunpowder(c)
               ? (cell_t)((c & 0xF8) | (v & 0x07))
               : (cell_t)((c & 0xF0) | (v & 0x0F));
}


/* Decodes MAT_EXTENDED. Calls extended_reactions[] by nibble. `burns`:
 * fire/lava. `burn_decay`: lit. `may_have_burning`: critical for reactions. */
static inline bool cell_is_burning(cell_t c);

static inline const reaction_t *reaction_of(cell_t c)
{
    if (CELL_MATERIAL(c) == MAT_EXTENDED) {
        return &extended_reactions[CELL_VARIANT(c)];
    }
    return &reactions[CELL_MATERIAL(c)];
}

static inline bool cell_is_burning(cell_t c)
{
    const reaction_t *r = reaction_of(c);
    return r->burns != 0 || (r->burn_decay != 0 && cell_code(c) >= r->lit_from);
}

/* TABLE-DRIVEN MOISTURE: Codes < `r->tones` are dry, `r->tones` to `r->tones
 * + r->moist_max - 1` are moist. Dirt uses `.tones = 8, .moist_max = 7`. See
 * `test_dirt_moisture_macros_and_codec_helpers_agree_on_every_byte`. Macros
 * for dirt; ENGINE uses for reactions. */
static inline uint8_t moisture_of(cell_t c, const reaction_t *r)
{
    const uint8_t code = cell_code(c);
    if (code < r->tones) {
        return 0;
    }
    /* Reading a lit grain as "moisture == moist_max" misidentifies it as damp
     * soil, removing moisture from a dry cell and corrupting its lit state. */
    if (r->lit_from != 0 && code >= r->lit_from) {
        return 0;
    }
    const uint8_t m = (uint8_t)(code - (uint8_t)(r->tones - 1u));
    return (m > r->moist_max) ? r->moist_max : m;
}

/* Set m > 0 for WET cell. NEVER use m == 0; see soil_cell() for dry case. */
static inline cell_t with_moisture(cell_t c, uint8_t m, const reaction_t *r)
{
    const uint8_t code = (uint8_t)((uint8_t)(r->tones - 1u) + ((m) > r->moist_max ? r->moist_max : (m)));
    return cell_with_code(c, code);
}

static inline cell_t soil_cell(cell_t identity, uint8_t tone, uint8_t m,
                               const reaction_t *r)
{
    const uint8_t code =
        (m != 0) ? (uint8_t)((uint8_t)(r->tones - 1u) + ((m) > r->moist_max ? r->moist_max : (m)))
                 : (uint8_t)((tone) >= r->tones ? (uint8_t)(r->tones - 1u) : (tone));
    return cell_with_code(identity, code);
}

/* Scales moisture for dry tone; see soil_dry_out(). Needed for non-uniform
 * materials. */
static inline uint8_t dry_tone_from_moisture(uint8_t m, const reaction_t *r)
{
    return (uint8_t)((m * (uint8_t)(r->tones - 1u)) / r->moist_max);
}

/* Mask to top five bits to distinguish gunpowder from statics. */
static inline bool same_species(cell_t a, cell_t b)
{
    if (CELL_MATERIAL(a) == MAT_EXTENDED || CELL_MATERIAL(b) == MAT_EXTENDED) {
        return (a & 0xF8) == (b & 0xF8);
    }
    return CELL_MATERIAL(a) == CELL_MATERIAL(b);
}

/* `spec` is a cell byte. MATERIAL_SHADE_SPAN misses gunpowder; see
 * GUNPOWDER_REACTION in material.c. */
static inline int material_shade_span_cell(cell_t spec)
{
    if (cell_is_gunpowder(spec)) {
        return reaction_of(spec)->tones;
    }
    return MATERIAL_SHADE_SPAN((material_id_t)CELL_MATERIAL(spec));
}

/* Rendering (colour tables, material_colours(), the grain/speckle helpers
 * and the MATERIAL_EDGE_* masks they read) lives in material_palette.h -
 * this half of the split stays pure data, with no gfx_color_t dependency. */
