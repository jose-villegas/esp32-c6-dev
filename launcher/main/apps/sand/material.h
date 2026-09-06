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
 * That overlap is deliberate.
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

#include "gfx/gfx_color.h"

/*---------------------------------------------------------------------------
 * Cells
 *-------------------------------------------------------------------------*/

typedef uint8_t cell_t;

#define CELL_EMPTY          ((cell_t)0)
#define CELL_MATERIAL(c)    ((uint8_t)((c) >> 4))
#define CELL_VARIANT(c)     ((uint8_t)((c) & 0x0F))
#define CELL_MAKE(m, v)     ((cell_t)(((uint8_t)(m) << 4) | ((uint8_t)(v) & 0x0F)))

/* Tests the material nibble rather than the whole byte, so a variant of zero
 * can never be mistaken for an empty cell. */
#define CELL_IS_EMPTY(c)    (((c) & 0xF0) == 0)

#define MATERIAL_VARIANTS   16

/* WHERE ROOM TEMPERATURE SITS on a heat-ramping material's 0-15 variant.
 *
 * Not 0, and that is the whole point. With ambient at the bottom of the
 * range there is no such thing as colder than resting: a pane at 0 touched
 * by snow has nothing to lose, so it cannot change and cannot show that
 * anything happened. Snow beside glass looked identical to snow beside
 * nothing, which is exactly what it was.
 *
 * Ambient in the middle gives cold somewhere to go. Below it a pane is
 * FROSTED and visibly pale; above it, warming; far enough above, glowing
 * and about to break. Three levels of frost is not much resolution, but it
 * is the difference between a state you can see and one you cannot, and
 * the levels above ambient are the ones doing the interesting work. */
/* SOIL reads its nibble by STATE rather than by a fixed field split: which
 * half of the value it is in says whether it holds a dry TONE or a wet
 * MOISTURE, rather than one bit always meaning tone and three always
 * meaning moisture regardless of which state the cell is actually in.
 *
 *     0 .. SOIL_DRY_TONES - 1     DRY. The value IS the tone, 0 palest to
 *                                 SOIL_DRY_TONES - 1 darkest-dry.
 *     SOIL_DRY_TONES ..           WET. moisture = value - (SOIL_DRY_TONES-1),
 *     SOIL_DRY_TONES - 1          1 up to SOIL_MOISTURE_MAX.
 *       + SOIL_MOISTURE_MAX
 *     MATERIAL_VARIANTS - 1       UNUSED. A corrupt cell here reads as
 *                                 moisture SOIL_MOISTURE_MAX in every
 *                                 accessor below AND in the palette (see
 *                                 material.c), rather than as an
 *                                 unpredictable eighth colour - one more
 *                                 value than the ranges above need, so it
 *                                 degrades instead of aliasing a real one.
 *
 * This was a fixed split instead - one bit of carried tone, three of
 * moisture, so SOIL_TONES was 2 - and it undersold dry soil specifically.
 * A WATERED bank had a real gradient, because moisture is laid out by
 * percolation cell by cell; a DRY one had exactly two colours, because two
 * tones is all one bit can ever say, and that is most of what a pile looks
 * like once it stops being watered. Splitting by state instead of by bit
 * position gives the dry side of the nibble every value the wet side does
 * not need at that moment - eight tones instead of two - and costs the wet
 * side nothing it was using: a saturated bed's own variation was always
 * going to be the moisture gradient itself, not an independent tone,
 * because that gradient is what percolation already draws down a pile.
 * The trade is real - a UNIFORMLY wet bed has one flat colour where it used
 * to have two - but a uniformly wet bed is the one case moisture itself
 * cannot shade regardless, and it is the rarer of the two flat cases this
 * fixes one of.
 *
 * Dry tone is also no longer noise. It is picked at POUR time, banded the
 * way sand's shade is (see random_cell() in sand.c), and re-picked at the
 * moment a cell crosses back to dry, biased by how wet its surroundings
 * still are right then (see soil_dry_out() in sand_reactions.c) - so a
 * pile that dried from the top down keeps that as a visible imprint:
 * pale where the front left nothing behind, darker wherever it was still
 * handing water off when it happened. Tone and moisture together are one
 * monotone luminance ramp across variants 0 through
 * SOIL_DRY_TONES - 1 + SOIL_MOISTURE_MAX (see material.c's SOIL_SHADES) -
 * bone-dry-palest at one end, saturated at the other, with nothing in
 * between reading as an unrelated colour. */
#define SOIL_DRY_TONES      8
#define SOIL_MOISTURE_MAX   7

#define CELL_MOISTURE(c)                                                 \
    ((uint8_t)(CELL_VARIANT(c) < SOIL_DRY_TONES                          \
                   ? 0u                                                  \
                   : (CELL_VARIANT(c) - (SOIL_DRY_TONES - 1u) >          \
                              SOIL_MOISTURE_MAX                          \
                          ? SOIL_MOISTURE_MAX                            \
                          : CELL_VARIANT(c) - (SOIL_DRY_TONES - 1u))))

/* Meaningful only while CELL_MOISTURE(c) == 0 - a wet cell's variant is
 * moisture, not a tone, and this reads it anyway rather than refusing,
 * because a caller that already knows the cell is dry (or does not care)
 * should not have to special-case the read. */
#define CELL_SOIL_TONE(c)   CELL_VARIANT(c)

/* Setting m > 0 makes a WET cell, and a wet cell carries no tone of its
 * own - see this block's own comment - so this DISCARDS whatever the
 * nibble held before, tone or moisture alike.
 *
 * NEVER call this with m == 0. There is no tone to fall back on once the
 * old one is gone, so a naive implementation has exactly one thing it can
 * write for "dry" - a fixed value, the same for every cell regardless of
 * where it is - which is precisely the flatness this whole re-encoding
 * exists to undo. A cell crossing to zero moisture has to go through
 * soil_dry_out() (sand_reactions.c) instead, which picks a tone from what
 * is still wet nearby rather than leaving every drying cell identical. */
#define CELL_WITH_MOISTURE(c, m)                                          \
    CELL_MAKE(CELL_MATERIAL(c),                                          \
              (uint8_t)((SOIL_DRY_TONES - 1u) + ((m) & SOIL_MOISTURE_MAX)))

/* Soil built from scratch, tone and moisture given separately - the one
 * place a caller MAY legitimately ask for moisture 0 with a specific tone,
 * because unlike CELL_WITH_MOISTURE above there is no prior cell whose
 * tone this would be throwing away: it is either brand new (random_cell()
 * picking a pour's tone) or the tone was already worked out by the caller
 * (soil_dry_out() itself). m > 0 wins and tone is ignored, the same "wet
 * has no tone" rule CELL_WITH_MOISTURE follows. */
#define CELL_SOIL(mat, tone, m)                                           \
    CELL_MAKE((mat), (uint8_t)((m) != 0                                  \
                                    ? (SOIL_DRY_TONES - 1u) +             \
                                          ((m) & SOIL_MOISTURE_MAX)       \
                                    : ((tone) & (SOIL_DRY_TONES - 1u))))

#define SAND_AMBIENT_HEAT 3

/* The heat level at or above which a cell with `shatters_to` cracks rather
 * than merely cooling when something cold touches it.
 *
 * Lives here rather than beside the code that uses it because THREE things
 * have to agree on it: the rule, the palette (glass's ramp changes colour
 * at exactly this level, so "will shatter" is a visible state and not a
 * hidden counter), and the tests. It was a private #define in
 * sand_reactions.c, which is how the number and the colour would have
 * drifted apart the first time either moved.
 *
 * FOUR LEVELS ABOVE AMBIENT. The gap is the tuned quantity; the absolute
 * number is bookkeeping, and the palette is computed from it so it can be
 * moved without re-cutting sixteen colours by hand.
 *
 * Measured against the scene people actually build - a drawn glass ring
 * filled about half way with lava, snow poured over the top:
 *
 *     ambient + 6     2.8 panes broken, mean over 12 seeds
 *     ambient + 4     2.8
 *     ambient + 2     6.8
 *
 * The reason it has to be this low is worth writing down, because it is
 * not "heat travels too slowly". Measured at the point that actually
 * decides: the glass a snowflake is TOUCHING, at the moment it touches,
 * sits at 3.02 - room temperature - averaged over 3277 contact steps.
 * Not warm-but-not-quite. Ambient.
 *
 * Two things put it there. A half filled vessel puts the reachable glass -
 * the rim, above the lava line - several cells from the heat, and the
 * gradient decays about two levels per cell. And snow CHILLS what it
 * lands on, so the contact cell is being actively cooled by the very
 * thing that wants to shock it.
 *
 * Four asked the reachable glass to be four levels above where it can
 * get. Two is still a real requirement - a pane at rest or one level up
 * is safe, so ordinary glass beside ordinary weather does nothing - but
 * it is a requirement the scene can actually meet. */
#define SAND_SHOCK_HEAT (SAND_AMBIENT_HEAT + 2)

/* And the other end of the same rule: at or BELOW this, a sudden heat
 * source cracks the cell instead of warming it.
 *
 * Thermal shock is a large temperature CHANGE, not a high temperature, and
 * for a while only half of it existed - cold onto hot broke glass, hot onto
 * cold did not. That asymmetry showed up the moment anyone tried the
 * obvious inversion: chill a vessel with snow, then pour lava in. Nothing
 * happened, for no reason that could be explained to the person doing it.
 *
 * The half that was missing is also the more USABLE half. Pouring snow onto
 * a lava-filled vessel mostly makes water, and water quenches lava to
 * stone, so the snow tends to kill the heat before it ever reaches the
 * glass - measured on a filled ring, the lava had turned to stone and the
 * whole vessel had frosted over with only two panes broken. Frost the
 * vessel first and then introduce the heat, and the two meet at the glass
 * where they are supposed to. */
#define SAND_SHOCK_COLD (SAND_AMBIENT_HEAT - 2)

/* A liquid cell holds between 1 and 15. Zero is not a very empty cell - it is
 * no cell at all, and must be written as CELL_EMPTY, or the material nibble
 * leaves an occupied cell holding nothing. */
#define MASS_MAX 15

/* How many cells of LOCAL DEPTH (app_sand.c's per-puddle depth walk) it
 * takes a liquid interior's shading to reach full saturation - see
 * material.c's DEPTH_SATURATE_CELLS, which is this value, for the shading
 * side of the story. Shared here, rather than left private to material.c,
 * because sand_liquid.c's pour-staleness fix needs the SAME distance to
 * decide how far a newly-claimed surface cell can possibly change any
 * cell's rendered depth - the two have to agree by construction, not by
 * coincidence, the way MATERIAL_EDGE_MASK_COUNT's own comment in material.c
 * warns two same-valued constants can quietly stop agreeing. */
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

    /* THE EXTENDED RANGE. Id 15 is not a material - it is an escape hatch.
     * A cell whose nibble is MAT_EXTENDED reads its LOW nibble as naming
     * one of sixteen further CODES, so the last slot buys sixteen byte
     * values rather than one - not sixteen further MATERIALS, since
     * gunpowder's half spends its eight codes on ONE material's variant
     * instead of eight more distinct ones: eight inert STATICS (0xF0-0xF7,
     * MATERIAL_EXTENDED_COUNT of them) and, since GUNPOWDER_BASE split the
     * upper half of the nibble off, one real KIND_POWDER material spread
     * across the other eight codes (0xF8-0xFF, MATERIAL_EXTENDED_CODES -
     * MATERIAL_EXTENDED_COUNT of them - see MATERIAL_ROWS's own comment
     * below for why the split exists at all).
     *
     * They cost nothing extra in the sweep, and the reason is entirely
     * about how the tables are already indexed:
     *
     *   materials[]   is read by cell >> 3, so all eight statics share
     *                 one row and all eight gunpowder codes share
     *                 another, and material_of() stays one shift and one
     *                 indexed load either way
     *   palette[]     is read by the whole CELL BYTE, so each of the
     *                 sixteen already has its own entry, for free
     *   reactions[]   is read only by sand_reactions.c - the cold pass -
     *                 so reaction_of() can afford to decode
     *
     * What they buy: their own colour and their own reactions. What the
     * eight STATICS cannot have: their own physics, since
     * materials[MATERIAL_ROW(MAT_EXTENDED)] is one shared row; or a
     * variant, since their low three bits are spent saying which one they
     * are. That confines them to inert static solids. Gunpowder is the one
     * extended-range material that escapes both limits, at the cost of
     * half the range. */
    MAT_EXTENDED = 15   /* the last nibble value; asserted against
                         * MATERIAL_MAX below, which this enum comes
                         * too early to reference */
} material_id_t;

/* The table is padded to every value the nibble can hold.
 *
 * That turns material_of() into a plain array index: without it every lookup
 * needs a bounds check, and those happen several times per cell per step. The
 * padding is flash, which is free, and the CPU is not - the same trade as the
 * 256-entry palette.
 *
 * The unused slots are inert: static, and denser than anything real. A corrupt
 * cell therefore becomes an immovable block rather than something that could
 * confuse the simulation. */
#define MATERIAL_MAX 16

/* SAND's shade range is split near the top, and the top band is CULLET:
 * sand that used to be glass.
 *
 * Free for the same reason soil's dry tones cost nothing beyond the
 * moisture range they sit beside (material.h's SOIL_DRY_TONES comment) -
 * sand's variant is already a shade, so saying "this grain came from a
 * pane" costs no bits at all, only four of the sixteen shades it could
 * have been. Twelve is still far more variation than a dune needs.
 *
 * Shattered glass was already placed at the very top of the ramp, being
 * whatever place_reacted() hands a new cell, so it was already the
 * brightest sand there is - and it did not read, for two reasons the band
 * fixes together. It was one flat value, so a broken pane was a slab of
 * uniform colour; and the top of a ramp is still on that ramp, so
 * "brightest sand" and "sand" are the same warm tan a shade apart. The
 * cullet band is a different HUE - pale and cool, the colour of ground
 * glass rather than of beach.
 *
 * The SHADE is still permanent, which is what makes cullet permanent at
 * all: sand's stored nibble never changes, so a heap keeps the memory of
 * having been a window, and mixing it into an ordinary dune leaves the two
 * visibly distinguishable. What the shade MEANS is not fixed any more,
 * though - it used to simply name one of four fixed pale colours. Now it
 * names which QUARTER of a shared, slowly-advancing colour cycle a grain
 * starts at (material_set_cullet_phase(), material_colours()'s own MAT_SAND
 * case, both material.c), so a heap of cullet keeps shimmering through
 * several pale tints at once, offset a quarter-cycle apart, without one
 * byte of the simulation ever being touched to do it. */
#define SAND_DUNE_SHADES    12
#define SAND_CULLET_BASE    SAND_DUNE_SHADES
#define SAND_CULLET_SHADES  (MATERIAL_VARIANTS - SAND_CULLET_BASE)

/* How many steps the cullet colour cycle has - see cullet_cycle[] and
 * material_colours()'s own MAT_SAND case, both material.c, for the array
 * this sizes and the arithmetic that walks it. Public (not local to
 * material.c) because host tests need to name it too - see suite_sand.c's
 * own cullet phase tests - rather than re-deriving 16 as a magic number
 * that could silently drift from the real constant on a retune.
 *
 * A power of two, so material_colours() wraps the index with a mask rather
 * than a per-cell modulo, and a multiple of SAND_CULLET_SHADES, so each of
 * the four cullet shades lands on an exact quarter-turn of the cycle - both
 * enforced by material.c's own _Static_assert, next to the array. */
#define CULLET_CYCLE_LEN 16

/* How many shades a freshly PAINTED grain may pick from. Everything else
 * gets the whole range; sand stops short of its reserved band, which is
 * what keeps the band meaning anything, and dirt stops short of the wet
 * range for the same reason - a freshly poured cell is dry by definition
 * (see random_cell() in sand.c), so it has no business picking a "shade"
 * from the half of the nibble that means moisture. */
#define MATERIAL_SHADE_SPAN(m)                                            \
    ((m) == MAT_SAND ? SAND_DUNE_SHADES                                  \
                      : (m) == MAT_DIRT ? SOIL_DRY_TONES : MATERIAL_VARIANTS)

/* How many STATIC materials hide behind MAT_EXTENDED's lower half - one per
 * value of the low nibble's bottom three bits, 0xF0-0xF7. Was sixteen (the
 * whole nibble); gunpowder now owns the upper half (0xF8-0xFF, see
 * GUNPOWDER_BASE), so eight codes remain for statics, three of them spare. */
#define MATERIAL_EXTENDED_COUNT 8

/* How many extended REACTION rows exist - sized by the whole low nibble
 * (16), not by MATERIAL_EXTENDED_COUNT, because extended_reactions[] and
 * extended_names[] still need one entry per BYTE VALUE 0xF0-0xFF:
 * entries 0-7 are the statics above, entries 8-15 are gunpowder's eight
 * variant codes, all sharing one GUNPOWDER_REACTION row (material.c) so
 * reaction_of() keeps its one-branch decode - see that function below. */
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

/* How a material moves. Four kinds cover almost everything, and the movement
 * code branches on this once per cell rather than on the material itself. */
typedef enum {
    KIND_NONE = 0,  /* empty space */
    KIND_STATIC,    /* never moves; costs nothing, the loop skips it at once */
    KIND_POWDER,    /* falls, and piles at an angle of repose */
    KIND_LIQUID,    /* falls, but spreads flat instead of piling */
    KIND_GAS,       /* rises, and disperses */
} material_kind_t;

/* Deliberately small, and with the movement fields first.
 *
 * Every one of these is read from the innermost loop, several times per cell
 * per step. The C6's cache line is 32 bytes, so a fat entry straddles two lines
 * and doubles the misses; keeping the whole row inside a few bytes keeps the
 * table resident.
 *
 * Note there is no colour here. The palette is the single source of that, and
 * duplicating it would be two places to change and one to forget. */
typedef struct {
    uint8_t kind;      /* material_kind_t, narrowed - an enum is int-sized */

    /* Heavier displaces lighter: sand sinks through water because its density
     * is higher, and water cannot push its way back up through sand. Empty
     * space is zero, so everything falls through it. */
    uint8_t density;

    /* Chance in 256 that a grain carrying one unit of load may still slide,
     * halving for each further unit. Zero locks a material solid under any
     * load at all; 256 means load is no obstacle, which is what makes a liquid
     * a liquid. */
    uint8_t slip;

    /* Coefficient of friction times ten, so 7 is the ~35 degrees of dry sand.
     * Zero means no angle of repose: a liquid slides sideways however level
     * the surface is, which is why water finds its own level and sand does
     * not. */
    uint8_t repose;

    /* Chance in 256 that a falling cell lags or drifts rather than falling
     * straight, so a stream disperses instead of descending as a block. */
    uint8_t scatter;

    /* Chance in 256, per step, that a grain's LIFE REMAINING (the variant
     * nibble, for a transient material - see this file's top comment) ticks
     * down by one. Zero means immortal: CELL_VARIANT keeps meaning whatever
     * it means for that material's kind instead (a shade, or a liquid's fill
     * level), and nothing ever clears the cell on its own. A material with
     * decay != 0 gates variant-as-life, not its kind - a future transient
     * material of a different kind could reuse this without a second field. */
    uint8_t decay;

    /* Chance in 256, per step, that this cell attempts to move AT ALL.
     * 255 means it moves every step it can, exactly like sand falls;
     * lower values sit still on the steps the roll misses. Ignored while
     * jostled (jostle != 0) - shaking bypasses this the same way it
     * bypasses slide_chance()'s own resistance.
     *
     * Two kinds read it, and it is worth knowing they are the same idea
     * under different names:
     *
     *   KIND_GAS     BUOYANCY. How eagerly a grain rises. See
     *                step_one_gas_grain() in sand_gas.c.
     *   KIND_LIQUID  VISCOSITY, inverted. Water at 255 flows freely;
     *                oil at 90 is syrupy, moving on roughly a third of
     *                its steps. See move_liquid_grain() and
     *                equalise_liquids() in sand_liquid.c.
     *
     * Liquids ignored this field entirely until oil arrived, which meant
     * every liquid flowed at exactly the same rate - a real complaint
     * about how oil and water looked together, and the reason this field
     * grew a second reader rather than the struct growing a field. There
     * was room for neither: material_t is read several times per cell per
     * step from the main sweep (see this file's own struct comment), and
     * on the 32-bit target a ninth byte would push its stride from 12 to
     * 16. A field that already meant "chance this cell tries to move"
     * needed no second copy to mean it for a second kind.
     *
     * Powders leave it at zero and never read it: a grain of sand falls
     * whenever it can, and its resistance is `slip`/`repose` instead. */
    uint8_t mobility;

    /* How far along the perpendicular a KIND_GAS material's spread pass
     * (equalise_gas() in sand_gas.c) will look for an empty cell to hop
     * into. Used to be a single global constant (SAND_GAS_SIGHT) shared
     * by every KIND_GAS material; per-material now so two materials
     * sharing that pass - gas and fire - can disperse by different
     * amounts (fire tighter, gas wider) without either one affecting
     * the other. Meaningless outside a gas pass, so every other
     * material leaves it at zero. */
    uint8_t sight;

    /* Cold: for the UI, never touched by the simulation. Last, so it cannot
     * push the movement fields out of the first cache line. */
    const char *name;
} material_t;

/* THE HOT TABLE, and why it has thirty-two rows instead of sixteen.
 *
 * Every ordinary material still owns one row of physics, unchanged - the
 * split below is entirely about MAT_EXTENDED. Extended range 0xF0-0xFF used
 * to be one shared row (KIND_STATIC, id 15) because a static needs nothing
 * else. Gunpowder needs KIND_POWDER, and that physics cannot live in the
 * SAME row a static ice block reads - so the table is indexed by
 * `cell >> 3` (32 rows) instead of `cell >> 4` (16), splitting nibble 15's
 * row in two by bit 3 of the cell byte: 0xF0-F7 keeps the old static row,
 * 0xF8-FF gets a new one with real POWDER physics (see GUNPOWDER_BASE
 * below).
 *
 * Every ORDINARY material's row is simply written twice, at
 * MATERIAL_ROW(id) and MATERIAL_ROW(id) + 1 - see material.c's TWIN_ROW
 * macro - so material_of() stays one shift and one indexed load, same
 * instruction count as before the split. */
#define MATERIAL_ROWS 32

_Static_assert(MATERIAL_ROWS == MATERIAL_MAX * 2,
               "one twin pair per ordinary material plus the extended "
               "nibble's own pair (static half, gunpowder half) - the hot "
               "table is exactly twice MATERIAL_MAX rows, never a fixed "
               "32 that could quietly stop matching it");

/* Where an ordinary id's row PAIR starts. `+1` is the twin - the same row,
 * reached when bit 3 of the cell's low nibble happens to be set (variant
 * 8-15) - and MAT_EXTENDED's own `+1` is the one row that is NOT a twin:
 * it is gunpowder, a different material entirely. See GUNPOWDER_BASE. */
#define MATERIAL_ROW(id) ((unsigned)(id) << 1)

/* Indexed by cell >> 3, not by the material nibble alone - see this
 * section's own comment. `const`, so it is in flash and costs no RAM. */
extern const material_t materials[MATERIAL_ROWS];

/* No bounds check: the top five bits of a byte are five bits, and the table
 * has thirty-two rows, so every possible value is already a valid index. */
static inline const material_t *material_of(cell_t c)
{
    return &materials[(c) >> 3];
}

/* For a caller that genuinely has an ordinary id - a table-building loop
 * over `m < MAT_COUNT`, random_cell() picking a material to spawn - rather
 * than a cell byte to decode. Deliberately NOT what a CELL_MATERIAL(cell)
 * extraction should feed: that throws away the bit that tells gunpowder
 * apart from an extended static, so anything derived from a cell must go
 * through material_of() above instead. Passing MAT_EXTENDED here reads as
 * the static row (MATERIAL_ROW(MAT_EXTENDED) is the LOWER of the twin
 * rows), which is the same default bare id 15 always meant. */
static inline const material_t *material_by_id(material_id_t m)
{
    return &materials[MATERIAL_ROW(m)];
}

/* How a material behaves in a fire - read ONLY by sand_reactions.c.
 *
 * A separate table rather than more fields on material_t, deliberately:
 * material_t is read several times per cell per step from the main sweep,
 * and its own comment above explains why keeping that row inside a cache
 * line matters. None of the fields below are read by any movement code, so
 * paying for them in the hot table's stride would be paying for nothing.
 * The cost of the split is that adding a material is now potentially two
 * rows instead of one - worth it, and the reason for it is here rather
 * than left to be rediscovered. */
typedef struct {
    /* Chance in 256, per adjacent burning cell, per step, that this
     * material catches fire. 0 means it never burns. 255 means it catches
     * the instant fire touches it - and, importantly, costs no random
     * number at all (see try_ignite()), so a material at 255 leaves the
     * RNG stream exactly as it was before this field existed. That is
     * what keeps gas's behaviour, and every existing gas/fire test and
     * device timing, bit-identical. */
    uint8_t flammability;

    /* What this material becomes when it catches - a material_id_t,
     * narrowed. Gas flashes straight to MAT_FIRE; a slower-catching fuel
     * can char into something else instead, so it stays put and keeps
     * burning rather than turning into a flame that immediately floats
     * away (see MAT_WOOD's own row and sand_reactions.c's top comment for
     * why that distinction exists at all). 0 (MAT_EMPTY) is read as
     * MAT_FIRE, so a flammable material that does not care what it turns
     * into gets the obvious default for free. */
    uint8_t ignites_to;

    /* Zero (the default, "ordinary ignition") for every material but
     * gunpowder. Nonzero is a BLAST RADIUS in cells. Ignition itself
     * (try_ignite_given(), try_heat_transform_given()) never reads this
     * field for the radius - catching just writes the LIT code, through
     * `ignites_to`/`heats_to` the same as any other burning material - but
     * it is read in THREE places in step_one_burning_cell() (sand_
     * reactions.c), not one, each asking a different question of it:
     *
     *   BURN-OUT   the actual blast. When a lit cell of this material's
     *              `burn_decay` countdown reaches `lit_from` and would
     *              otherwise simply vanish, find_lit_two_by_two() asks
     *              whether the cell is one corner of a 2x2 that is all
     *              still lit: if impulses are enabled and so,
     *              sand_explode() fires at this radius (a fully-lit 3x3
     *              was asked for first and made blasts rare enough to
     *              look broken - see that helper's own comment); otherwise
     *              the cell becomes plain MAT_FIRE, the gas pocket's own
     *              fallback and for the same reason (sand_explode() is a
     *              documented no-op with no impulse buffer). See
     *              SAND_GUNPOWDER_BLAST_RADIUS's own comment below for why
     *              16.
     *   QUENCH     `!= 0` there stands in for "this material also has a
     *              moisture codec", true only because gunpowder is
     *              currently the one material with both - see that
     *              branch's own comment for why the two questions are
     *              different ones that happen to share an answer today.
     *   SMOTHER    `== 0` skips smothered() outright: gunpowder carries
     *              its own oxidiser, and a fuse buried in the middle of
     *              its own pile has to keep burning or nothing inside a
     *              pile would ever reach burn-out at all.
     *
     * REVISION: earlier versions blasted the instant ignition or heat
     * touched the cell, one grain or one boundary cell at a time, which
     * measured on the device as spending nearly every blast throwing
     * gunpowder at gunpowder. See docs/Sand/Explosion-Plan.md for that
     * history and the reasoning behind the fuse-and-2x2 model this field
     * now describes. */
    uint8_t explodes;

    /* Nonzero: this material only catches where it TOUCHES AIR - a cell
     * with at least one empty cardinal neighbour. Zero, the default, means
     * it catches anywhere fire reaches it, which is right for a solid.
     *
     * This is what makes a pool of liquid fuel burn off its surface
     * instead of detonating through its whole volume the instant a spark
     * lands on it. The interior cells of a pool are surrounded by more
     * pool and never qualify; the ones along the top - and any exposed
     * edge, which is correct too, a slick burns wherever it meets air -
     * do. As each exposed layer converts and rises away, the layer under
     * it becomes exposed in turn, so the pool is eaten from the top down
     * without anything here needing to know which way is up.
     *
     * That last part is deliberate. "Only the top burns" is the obvious
     * phrasing and would need a gravity vector, which this pass used to
     * take and no longer does (see sand_step_reactions()). "Only what
     * touches air burns" needs nothing, describes the same thing for any
     * pool worth looking at, and is more nearly true besides. */
    uint8_t needs_air;

    /* Nonzero: this material IS a heat source, and sand_step_reactions()
     * gives it a turn - decaying, quenching, smothering, igniting
     * neighbours. Replaces the old `CELL_MATERIAL(c) != MAT_FIRE`
     * dispatch, which cannot express two burning materials at once.
     * Still keyed off the material, NOT off `kind`: fire is KIND_GAS and
     * a future burning solid could be KIND_STATIC, and both of those
     * kinds are shared with materials that must not burn (gas, stone). */
    uint8_t burns;

    /* BURNING AS A STATE RATHER THAN A MATERIAL. Non-zero means this
     * material burns while its VARIANT is non-zero, and that variant is
     * how much of it is left to burn - counted down at this chance in 256
     * per step, exactly as `decay` counts a transient down.
     *
     * Wood is the one that has it, and it exists because ember used to be
     * a whole material for this. Ember differed from wood in seven fields
     * and only ONE of them - decay - was in the movement table; the other
     * six were reactions. It was, in other words, wood in a different
     * state, and it cost a slot because the tables are indexed by the
     * material nibble alone and there was nowhere else for a state to
     * live.
     *
     * There is now: the variant. Wood spends its shade on burn progress
     * the way glass spends its on temperature, the dispatch in
     * step_one_reacting_row() asks whether this cell is lit rather than
     * whether this material burns, and ember stops needing to exist.
     *
     * `burns` and `burn_decay` are different claims and must not be
     * confused. `burns` means ALWAYS a heat source - fire, lava. This
     * means SOMETIMES, and the variant says when. */
    uint8_t burn_decay;

    /* THE FIRST CODE THAT COUNTS AS LIT, for a `burn_decay` material -
     * see cell_is_burning() and tick_decay_at() (this file), the two
     * places that read it. Wood's variant IS its whole burn-progress
     * counter, unlit at 0 and lit at every value above, so wood's row
     * sets this to 1 and both of those functions read exactly as they
     * did before this field existed (`CELL_VARIANT(c) != 0`,
     * `life <= 1`). Gunpowder's low bits are not a burn counter below
     * this threshold at all - codes 0-2 are dry tones, 3-6 are moisture
     * levels - so its row sets this to 7 (GUNPOWDER_LIT), the single
     * code that means "on fire", and nothing below it is ever mistaken
     * for embers. A material with no `burn_decay` never reads this
     * field either, so it needs no entry of its own. */
    uint8_t lit_from;

    /* Chance in 256, per step, per burning neighbour, that heat crosses
     * ONE cell of this material - see conduct_heat() in
     * sand_reactions.c. Rolled again for every further cell of the same
     * conductor the heat has to cross, so crossing depth d succeeds with
     * probability (conducts/256)^d: a thin wall conducts briskly and a
     * thick one slowly, for free, with no second "how thick" constant.
     * Meaningless for anything that never sits between a fire and
     * something worth heating; left at zero for everything but the one
     * material that exists to be a heat conductor. */
    uint8_t conducts;

    /* Chance in 256, per step, that a liquid conduct_heat() (sand_reactions.
     * c) has already decided conducted heat reaches THIS step actually
     * boils it into steam - a second roll, not the one `conducts` already
     * made getting the heat there. 0, the default, means immune to
     * conducted-heat boiling entirely; a liquid opts in by setting a
     * nonzero figure, same idiom as `dissolves` below.
     *
     * Water sets a low figure so a poured stream can occasionally outpace
     * evaporation over a hot stone crust rather than every drop flashing
     * to steam the instant heat arrives. Acid, which also qualifies for
     * conduct_heat()'s boiling branch, sets 255 (effectively always) so
     * its own pre-existing instant-boil behaviour is unchanged now that
     * this field exists to gate it. */
    uint8_t boils;

    /* What a `boils` roll above turns this liquid into - a material_id_t,
     * narrowed, same idiom as `quench_to`. 0 means MAT_STEAM, so water
     * (and every other liquid that never sets this) keeps the original,
     * literal reading of "boils". Acid overrides it to MAT_GAS: acid
     * boiling through a wall is not water and has no business leaving
     * the same white kettle-steam behind - it already leaves MAT_GAS
     * everywhere else it evaporates (step_one_dissolver_cell()'s
     * `evaporates` roll, `fizz`'s dissolving residue), and conducted-heat
     * boiling was the one path still hardcoded to steam regardless of
     * which liquid was on the far side of the wall. */
    uint8_t boils_to;

    /* Chance in 256 that a burnt-out cell of this material leaves
     * MAT_SMOKE behind instead of simply clearing.
     *
     * MAT_SMOKE and MAT_STEAM are near-identical rows in materials[] and
     * were deliberately ONE material to begin with: both are a light gas
     * that rises, spreads and fades, so a second row looked like pure
     * duplication. It was not. Steam is water that got hot; smoke is fuel
     * that burned out; and a fire dying in mid-air, nowhere near water,
     * puffing bright white kettle-steam reads as a bug to anyone watching
     * it happen. The two rows exist to be TOLD APART on screen, and the
     * difference that actually matters is in the palette, not here. See
     * sand_reactions.c's own top comment. */
    uint8_t residue;

    /* What this material becomes when a liquid touches it - a
     * material_id_t, narrowed. 0 means it simply vanishes, which is what
     * every burning material did before steam existed. */
    uint8_t quench_to;

    /* Chance in 256, per step, that this material emits a MAT_FIRE cell
     * into an adjacent empty cell (try_flare(), sand_reactions.c) -
     * meant for something that looks like it is licking a flame upward
     * while staying PUT itself, left at zero for everything else. Ember
     * (KIND_STATIC, never moves on its own) is the mechanic's original
     * case; lava (KIND_LIQUID) sets it too, which is the one case where
     * "staying put" is not guaranteed - try_flare() itself now SKIPS the
     * roll entirely while a cell is still free-falling (nothing beneath
     * it, gravity-relative), rather than rolling it every step of a long
     * pour's fall, which used to make lava specifically the most
     * expensive material to pour: many separate falling grains each
     * spending several steps in open air, each independently rolling
     * this every one of those steps, each successful roll latching
     * may_have_burning for a fresh MAT_FIRE cell that then keeps burning
     * (and eventually rolls `residue`, above, for its own smoke) long
     * after the pour itself is done. */
    uint8_t flare;

    /* Chance in 256, per step, that a cell of this material DISSOLVES one
     * of its four cardinal neighbours. Acid is the only thing that does.
     *
     * Paired with `dissolvable` below, on the other material: this is how
     * hard the acid tries, that is how easily the target gives way. Both
     * have to be nonzero for anything to happen, which is what lets a
     * stone tank hold acid while the sand inside it disappears. */
    uint8_t dissolves;

    /* Chance in 256 that an attempt to dissolve THIS material succeeds.
     *
     * 0, the default, means immune - and that default is doing real work.
     * A material is dissolvable only by opting in, so every material that
     * existed before acid did, and every one added without a thought for
     * it, is safe by omission. The alternative default would have acid
     * quietly eating the walls of its own container, the floor, and the
     * air, and the failure would look like acid working rather than like
     * a field nobody set. */
    uint8_t dissolvable;

    /* Chance in 256 that a cell this material dissolves leaves MAT_SMOKE
     * behind instead of simply clearing - the fizz.
     *
     * Its own field rather than reusing `residue`, which fires when a
     * BURNING cell runs out of life. Dissolving is a different event on a
     * different cell (the target's, not the reactor's), and overloading
     * one field to mean both is how `mobility` and `sight` ended up
     * meaning different things to different kinds without saying so.
     *
     * Smoke rather than steam, deliberately: steam in this simulation is
     * water that got hot (see sand_reactions.c's top comment), and acid
     * fumes are not that. Smoke is the generic "something was destroyed
     * here", which is what this is. */
    uint8_t fizz;

    /* Chance in 256, per step, that a cell of this material spontaneously
     * turns into a single MAT_GAS cell - unconditional, no heat or
     * neighbour required, unlike boiling. 0, the default, means never;
     * only acid sets it. */
    uint8_t evaporates;

    /* The inverse of evaporating: chance in 256, per step, that a 2x2
     * square of four cells all holding this same material COLLAPSES into
     * a single cell of `condenses_to`, at the square's own top-left
     * corner, with the other three cleared to empty. 0, the default,
     * means never; only steam sets it, turning a stray puff quietly back
     * into a little water.
     *
     * Deliberately not a real thermal model - no cold surface to check
     * for, no heat reading involved - which is what keeps this a rare
     * cosmetic touch (fake condensation) rather than a second boiler to
     * tune. See step_one_condensing_cell() in sand_reactions.c. */
    uint8_t condenses;

    /* What a successful `condenses` roll above produces - a
     * material_id_t, narrowed, the same relationship `quench_to` has with
     * its own trigger field. Meaningless while `condenses` is 0. */
    uint8_t condenses_to;

    /* What HEAT alone turns this material into, without burning it, and
     * the chance in 256 per step per adjacent heat source that it does.
     *
     * Sand names MAT_GLASS here. Its own pair of fields rather than
     * reusing `flammability`/`ignites_to`, which would work mechanically -
     * a burning neighbour, a roll, a material swap - and would be a lie:
     * sand does not catch fire, and a field called `flammability` on sand
     * would send the next reader looking for the flame. The same
     * overloading is how `mobility` and `sight` came to mean different
     * things to different kinds without saying so.
     *
     * Reached both by direct contact with a burning cell and through a
     * conductor (conduct_heat()), so a fire under a stone slab makes glass
     * of the sand on the other side exactly as it boils water there. */
    uint8_t heats_to;
    uint8_t heat_chance;

    /* MELTS: chance in 256 per step of becoming `heats_to` while in direct
     * contact with a burning LIQUID - lava - and only that. `heat_chance`
     * above answers to any heat at all: fire, an ember, lava, or heat that
     * has crossed a conductor. This is the narrower door, for a material
     * that has to shrug off a flame and still give way to molten rock.
     *
     * Glass already draws that exact line, but it does it with a ramp:
     * fire can raise a pane to shatterable and never to molten, because
     * `cools` drains faster than a flame can bank (see glass's own row).
     * That needs a variant to bank INTO, and an extended material has
     * none - its low nibble is which material it is. So the distinction
     * has to be made at the source instead of in the target's memory:
     * lava is the one heat source that is a liquid, and contact with it
     * is what this rolls on.
     *
     * Contact only - not through a conductor. Heat conducted through a
     * wall has lost its source by the time it arrives, and "lava on the
     * far side of stone melts what fire on the far side would not" is a
     * distinction nobody could see the reason for on the panel. */
    uint8_t melts;

    /* THE IMPURE YIELD. Rolled off the SAME successful heat_chance roll
     * above, not a trigger of its own: chance in 256 that a bone-dry cell
     * converts into `flaw_to` instead of `heats_to`. 0, the default, means
     * every successful roll is clean - exactly the behaviour before this
     * field existed.
     *
     * Dirt names MAT_STONE here: a smelt run too fast, or ore that was
     * never pure to begin with, comes out part stone rather than part
     * metal. Rejected once as "a new field serving exactly one material"
     * (docs/Sand/Metal-Smelting-Plan.md, "Decisions taken") - it still is,
     * and that is fine; not every mechanic has to earn its keep across the
     * whole table the way `heats_to` does.
     *
     * A flaw is not drawn independently per cell - see try_heat_transform()
     * (sand_reactions.c) for the rolling-modulo clump that turns
     * `flaw_chance` into NODULES of stone rather than a speckle. */
    uint8_t flaw_to;
    uint8_t flaw_chance;

    /* THE RUINED YIELD. Rolled off the SAME successful heat_chance roll
     * above, on a cell the wet-earth branch below would otherwise drive one
     * moisture level off of: chance in 256 that the cell spoils into
     * `spoils_to` outright instead. 0, the default, means wet cells never
     * spoil - the moisture-draining behaviour runs exactly as it did before
     * this field existed.
     *
     * Dirt names MAT_SAND here: clay fired too fast while it is still wet
     * cracks rather than firing clean. Independent of `flaw_to` above by
     * construction - a cell that spoils never reaches the dry branch at
     * all, so the two chances are never rolled against the same event.
     *
     * Unconditional - rolled from the FIRST successful heat_chance roll a
     * wet cell ever gets, no exemption. An earlier version tried to
     * guarantee a free first puff of steam before any risk of spoiling by
     * gating on the moisture value; see try_heat_transform()'s own comment
     * (sand_reactions.c) for why that cannot work (ambient drying can beat
     * heat to the first level, unrelated to this field entirely) and was
     * removed rather than patched further. */
    uint8_t spoils_to;
    uint8_t spoils_chance;

    /* HEAT THAT ACCUMULATES, rather than a roll that either fires or does
     * not. Non-zero `heat_ramp` means this material banks heat in its own
     * variant nibble instead of transforming on contact: each step beside a
     * heat source it climbs one level with this chance, and only on reaching
     * the top does it become `heats_to`.
     *
     * The point of it is that `heat_chance` alone CANNOT express "long
     * exposure". A per-step roll has no memory, so a brief fierce flame and
     * a slow banked fire accumulate identically - the only thing that
     * separates them is heat draining back out, which is `cools`. Sand keeps
     * the memoryless form because sand fusing is meant to be quick; glass
     * melting to lava is meant to take a while and to be visible while it
     * does, which is the other half of this: the nibble is what the palette
     * indexes, so the heat level IS the colour and a heating pane glows.
     *
     * `cools` is the chance per step of losing a level with nothing heating
     * it. Together the two set how long "long" is, and the ratio is what
     * decides whether a fire can ever win at all: cooling faster than the
     * ramp climbs means no flame of that size will EVER melt the pane, which
     * is a legitimate thing to want and a very easy thing to do by accident.
     * See test_a_lone_flame_never_melts_glass. */
    uint8_t heat_ramp;
    uint8_t cools;

    /* COLD, which this simulation otherwise has no way to say.
     *
     * `chills` is the chance per step that this material pulls a heat level
     * out of a neighbour that has one. Non-zero also MARKS the material as
     * cold for thermal shock below - the two always want to travel together,
     * so they are one field rather than two that can disagree.
     *
     * `chills` and `cools` do the same thing in the same units - remove one
     * heat level, chance in 256 - and they are still two fields. What
     * separates them is whose row they are on: `cools` belongs to the HOT
     * material and drains it to nothing, `chills` belongs to the COLD one
     * and drains a neighbour. Folding them into a single "rate this
     * material removes heat", read as self-drain when it has a ramp and
     * neighbour-drain when it does not, works mechanically and is exactly
     * the mistake `mobility` and `sight` already made here: a field whose
     * meaning switches on kind without saying so.
     *
     * They do not collapse to one NUMBER either. If a chilling neighbour
     * merely re-ran the hot cell's own `cools`, snow would drain 6 in 256
     * against a ramp of 12 and could never beat even a single flame. Snow's
     * 40 against glass's 6 is most of a factor of seven, and that gap is
     * the mechanic - it is what lets a bank of snow win a race that
     * ambient cooling always loses.
     *
     * Snow is the only material with it, and adding it is what made thermal
     * shock legible. Shock was first drafted as "heat on one side, water on
     * the other", which fails as a design even though it works as a rule:
     * nothing in the simulation says water is COLD, so a pane cracking next
     * to it reads as "glass breaks near water" rather than as a temperature
     * gradient. A material that is visibly, obviously cold fixes that
     * without a temperature scale on anything but the glass itself. */
    uint8_t chills;

    /* CONVECTION: hot gas warming what it touches. Chance in 256 per step
     * that this material raises the temperature of a neighbour that has
     * one - without igniting anything, quenching anything, or being a heat
     * source in any other sense.
     *
     * It is NOT `burns`, which was the cheap way to get the same heating
     * and would have had smoke setting wood alight.
     *
     * This was measured three times against whether it helped SHATTER
     * glass, and three times it did not - warmer air costs snow its life,
     * because a pane above room temperature charges snow for touching it,
     * and snow is the scarce thing. It is here for a different reason:
     * heat rising into a vessel and warming it is TRUE, and it is now
     * visible, because glass and stone both show their temperature. The
     * rates are modest for exactly that reason - enough to see, not enough
     * to make the cold side worthless.
     *
     * High relative to the ramps around it because the carriers are
     * TRANSIENT. A wisp of smoke has to deposit what it is worth during a
     * life measured in steps; a rate tuned as though it would sit there
     * indefinitely deposits nothing before it is gone. */
    uint8_t warms;

    /* Chance in 256 per step, per adjacent LIQUID cell, that this material
     * gives up and becomes `heats_to`. Snow melting in water.
     *
     * A second trigger for the same transformation `heat_chance` drives,
     * and a separate number because one number cannot serve both. Snow
     * beside a flame should be gone almost at once - 120 in 256, two steps
     * - and snow landing on a pond should not, or a snowfall over water
     * would never be seen to land at all. The float is worth a moment:
     * snow is lighter than water and rides on top of it, which is the only
     * reason a drift ends up anywhere useful.
     *
     * Any liquid counts, not water alone. Nothing in this simulation is at
     * a temperature except glass, so "liquid" is the closest thing to
     * "warm and touching you everywhere" available, and oil or acid
     * leaving snow untouched would need explaining in a way that melting
     * does not. */
    uint8_t thaws;

    /* SOAKING UP A LIQUID, which is a different thing from melting in one.
     * Chance in 256 per step, per adjacent liquid cell, that this material
     * takes a UNIT of that liquid into itself - the liquid is consumed, not
     * merely survived, which is the whole difference from `thaws`.
     *
     * Where the unit goes depends on `soaks_to`:
     *
     *   non-zero   the cell BECOMES that material, at moisture 1. Sand
     *              names dirt here: wet sand slowly turns into soil.
     *   zero       the cell keeps what it is and its VARIANT rises. Dirt
     *              names nothing, so watering dirt makes it wetter.
     *
     * One field pair rather than two mechanisms because it is one thing
     * happening - something absorbing water - and the only question is
     * whether the thing it absorbed into already existed. */
    /* WETTING, on the LIQUID's side: whether this liquid is the sort of
     * thing that soaks into something. Water is; oil, lava and acid are
     * not.
     *
     * It has to be said explicitly because the absorbing side cannot tell.
     * `soaks` is a property of sand and soil, and the obvious way to write
     * the rule - take a unit of any adjacent KIND_LIQUID - reads perfectly
     * and is wrong for three of the four liquids on the board. A bank of
     * sand under oil turned entirely into saturated soil, and so did one
     * under LAVA. Reported as oil soaking, which it was, along with
     * everything else.
     *
     * Wetness is not the same question as fluidity, and only the liquid
     * knows the answer. */
    uint8_t wets;

    uint8_t soaks;
    uint8_t soaks_to;

    /* THE MOISTURE CODEC'S SHAPE, table-driven - see moisture_of()/
     * with_moisture()/soil_cell() (this file, above). `tones` is how many
     * DRY codes this material's low bits hold (codes 0..tones-1); moisture
     * 1..moist_max then occupies codes tones..tones+moist_max-1. Dirt sets
     * 8 and 7 - the same SOIL_DRY_TONES/SOIL_MOISTURE_MAX every existing
     * CELL_MOISTURE()/CELL_WITH_MOISTURE() caller already assumes, so this
     * table and those macros MUST agree for dirt - see
     * test_dirt_moisture_macros_and_codec_helpers_agree_on_every_byte
     * (suite_sand.c), which is what proves it.
     *
     * A second material with `dries != 0` needs its own pair here rather
     * than reusing dirt's - gunpowder's codec is three dry tones plus four
     * moisture levels (material.c's GUNPOWDER_REACTION), not eight and
     * seven, because gunpowder only has THREE bits to spend (it shares
     * nibble 15 with the extended statics - see GUNPOWDER_BASE) where dirt
     * has the whole nibble. */
    uint8_t tones;
    uint8_t moist_max;

    /* WHETHER A PLANT MAY TREAT THIS AS SOIL - a different question from
     * `dries` below, which only says "this material's code can mean
     * moisture". Gunpowder has `dries != 0` too (it has its own moisture
     * codec, wetting and drying the same way dirt does) but a fuse is not
     * ground: nothing may root in it, sprout from it, drink it dry or
     * conduct water through it. Every site that asks "is this neighbour
     * soil" - find_water(), step_one_sprouting_cell(),
     * step_one_budding_cell() (through find_water()), step_one_rooting_
     * cell(), step_one_conducting_cell(), spend_soil_moisture() - tests
     * this instead of `dries`. Moisture DIFFUSION and percolation between
     * two cells that are already the same species keep testing `dries`:
     * that is wetness spreading, not a question of what counts as soil.
     * Dirt sets this; nothing else does. */
    uint8_t soil;

    /* And the way back: chance in 256 per step of losing one level of
     * whatever `soaks` put in. Dirt drying out.
     *
     * Non-zero is also what MARKS a material's variant as moisture, the
     * way heat_ramp marks it as temperature and burn_decay as how much is
     * left to burn. A freshly drawn cell of one starts at zero - bone
     * dry - which is why random_cell() has to know. */
    uint8_t dries;

    /* Zero (meaning "never") for every material but gunpowder. Nonzero
     * `soaked_to`: what a cell of this material becomes, chance
     * `soaked_chance` in 256 per step, once it is fully SATURATED
     * (moisture_of(cell, r) == moist_max) - read in
     * step_one_soaking_cell() (sand_reactions.c) after `held` is computed,
     * gated on the saturation check so an unsaturated cell never rolls it
     * and dirt (soaked_to == 0) never draws at all. Gunpowder names
     * MAT_OIL here: soaked through, it slowly turns to oil rather than
     * staying inert forever - one grain and the water it holds becoming
     * one full oil cell, the same mass-not-conserved trade every other
     * reaction that mints a liquid already makes. */
    uint8_t soaked_to;
    uint8_t soaked_chance;

    /* GROWING. Chance in 256 per step that this material, touching soil
     * with moisture in it, extends by one cell - and spends one level of
     * that soil's moisture doing it. Water is what a plant grows ON, so
     * water is what limits how far it gets.
     *
     * It grows AGAINST gravity, from the top of whatever column of itself
     * it is part of rather than from the cell that happened to roll. That
     * matters because a plant has no per-cell state to grow WITH: it is an
     * extended material, so its low nibble is its identity and there is no
     * variant left to hold a stem's height or a growth counter. Walking to
     * the tip is how a stateless material still makes a tree instead of a
     * one-cell shrub. */
    uint8_t grows;

    /* FALLING, in the cold pass: chance/256 per step that this cell moves
     * one step gravity-ward, if the cell it would move into is empty.
     *
     * Which is the sweep's job, and is here anyway, for a reason specific
     * to the extended range: `kind` lives in materials[], and every
     * extended material shares one row of it. Making the plant a
     * KIND_POWDER so it could be poured like a grain would make ICE one
     * too - and would break the plant itself, because a grown stem is made
     * of the same material as the seed, so a column six cells tall would
     * slump the moment it existed.
     *
     * Falling only into EMPTY is what separates the two cases without any
     * per-cell state at all. A seed painted in mid-air has nothing under
     * it and drops until it lands. A stem does not, because what is under
     * every cell of it is the rest of the stem. The rule is the same; the
     * board answers it differently. */
    uint8_t falls;

    /* WITHERING: chance/256 per step that this cell simply ceases to
     * exist, when it can neither reach water through its own roots nor
     * lean on a neighbour of whatever it hardens into.
     *
     * Growth is the only thing on this board that MAKES cells, and until
     * now nothing took them away again except fire and acid. So every
     * fragment a tree shed - a limb broken off by a tilt, a seed poured
     * onto bare stone - was permanent, and the board slowly filled with
     * green litter that could never do anything or go anywhere.
     *
     * The "or wood" half is what keeps it from being cruel. A grown tree
     * whose soil has dried out keeps its foliage, because the foliage is
     * touching the trunk; what withers is loose greenery with no tree and
     * no water behind it, which is exactly the stuff that should not be
     * lying around. */
    uint8_t withers;

    /* And what a long enough straight run of it turns into: `hardens_to`
     * once `harden_run` cells line up along the gravity axis.
     *
     * A stem that has grown tall becomes a trunk. Measured along gravity
     * only, so a creeper spreading sideways stays soft - "grew tall enough
     * to be wood" is the reading, and a horizontal mat hardening into a
     * plank floor is not.
     *
     * It also closes a loop that already existed: wood burns, and since
     * burning became a STATE of wood rather than its own material, a tree
     * can catch, be rained on halfway, and leave the soft growth around it
     * alive. None of that needed anything new. */
    uint8_t hardens_to;
    uint8_t harden_run;

    /* And the chance in 256 that a run long enough actually hardens, on
     * any one growth. It is a delay, not a gate: without it a run turns to
     * wood the instant it is long enough, and wood does not grow, so a
     * seedling became a post before it could put out a limb.
     *
     * Lower means greener and shaggier, higher means woodier and squatter.
     * Measured over eight trees - see the roll in sand_reactions.c. */
    uint8_t harden_chance;

    /* What this material is PART OF: the other material its own body may
     * be made of, for every question of the form "is this cell more of the
     * same tree".
     *
     * It was `hardens_to` doing both jobs, which worked only for as long
     * as the plant was the only material with either. The two are
     * genuinely different questions - one is what a run of me BECOMES,
     * the other is what I hold on to - and they part company the moment
     * anything else joins a tree. Foliage is part of a tree and never
     * hardens into anything.
     *
     * Read by the walks that decide whether a cell is anchored, whether a
     * stem continues, how far it is to water, and whether a trunk is
     * already thick enough. `hardens_to` is now read only where hardening
     * actually happens. */
    uint8_t clings_to;

    /* ROOTING: one field, two readings, by which material's row it sits
     * on - the same table-reuse `hardens_to` and `clings_to` already
     * practise elsewhere in this struct.
     *
     * ON A GROWER (plant, wood): chance in 256 that SPENDING this
     * material's own soil moisture (growing, budding, sprouting - see
     * each of their own comments) also welds the CONTACT cell - the
     * collar, where the stem actually touches ground - into `roots_to`.
     * It exists because dirt is a powder and shifts: when the soil
     * directly under a tree's collar slides away, find_water()'s walk
     * down the stem finds neither stem nor ground below it and the tree
     * simply stops growing, stranded above water it can no longer reach.
     * A root embeds the tree in the bed instead of resting it on top of
     * that bed, so a shifting surface cannot disconnect the two. This is
     * a ONE-TIME SEED, gated (spend_soil_moisture(), sand_reactions.c) to
     * `root_depth == 0` - it plants the first root under a bare collar
     * and then gets out of the way; everything the root system becomes
     * after that first cell grows the other way, below.
     *
     * ON ROOT ITSELF: chance in 256 that a root cell, touching a moist
     * dirt neighbour, converts that neighbour into more root - see
     * step_one_rooting_cell() (sand_reactions.c) and MATX_ROOT's own row
     * (material.c) for the shape this produces. The conversion IS the
     * water cost: a dirt cell's moisture lives in its own variant, and
     * turning it into root discards that variant along with everything
     * else the old cell was, so there is nothing left to separately
     * spend. A root that already has several root neighbours does not
     * roll at all - see ROOT_SURFACE_MAX (sand_reactions.c) - which is
     * most of what keeps the system a filigree of roots instead of a
     * block of them growing to fill the bed. */
    uint8_t roots;
    uint8_t roots_to;

    /* What SHELTERS this from withering: touch a cell of it and a dry
     * spell cannot take you.
     *
     * The third time a field here has turned out to be answering two
     * questions. `clings_to` was doing structure - is this cell part of
     * the same body, for anchoring, for the stem walk, for the walk down
     * to water - AND shelter, and the two only looked like one while
     * foliage and growth wanted the same answer.
     *
     * They do not. FOLIAGE should be sheltered: a tree in a drought keeps
     * its leaves, and a leaf cannot fall, so nothing else would ever clear
     * a crown whose trunk burned away. GROWTH should not: sheltered green
     * never dies, so every stem that failed to finish its run stayed on
     * the tree for ever, and that - not the growing tip, and not the buds
     * - is what "stacking plant" was. Measured over eight trees: sheltered
     * growth leaves 44 green cells and 12 columns clinging to trunks;
     * unsheltered, 18 and 2, with MORE timber (262 against 255). */
    uint8_t sheltered_by;

    /* What a run of this leaves BEHIND when it hardens, and how much:
     * `canopy` is a chance in 256 per candidate space around the top of
     * the new trunk, `canopy_to` is the cell spec to put there.
     *
     * It exists because hardening is the only moment that holds a whole
     * run - foot, length and direction - in one place. After it, the tree
     * has no green cells low down at all: everything that grew is timber,
     * and growth is the only thing that makes cells. So nothing working
     * through ordinary growth can ever leaf a crown or fatten a trunk;
     * both have to happen here or not at all.
     *
     * `canopy_to` is a cell spec rather than a material id, like
     * `shatters_to` and `sprouts_to`, because what a tree puts out is an
     * extended material whose identity is its low nibble. */
    uint8_t canopy;
    uint8_t canopy_to;

    /* How much wider than one cell a hardened run may be at its foot,
     * tapering to nothing at its top. Zero leaves a stick. */
    uint8_t trunk_girth;

    /* HOLDING A LINE: chance in 256 that growth continues along the run's
     * OWN direction rather than straight against gravity.
     *
     * Without it a limb cannot exist. Growth always reckoned from "up", so
     * a branch went out one cell and then climbed - and a one-cell branch
     * is a run of one, which trips the `run < 3` gate that forces the
     * straight-up arm, so it climbed on every single attempt. Every limb
     * turned into a thin vertical thread beside the trunk, which is the
     * same shape that made basal suckers look like floating debris.
     *
     * The direction is not remembered anywhere. It is re-derived from the
     * shape, the way the tip walk answers "where is my top": the step from
     * the cell below a run to the cell itself IS the way that run has been
     * going. A trunk's is straight up and nothing changes for it; a limb
     * that set out up-and-left keeps going up-and-left.
     *
     * SET IT TO ZERO to get the previous behaviour back exactly - every
     * direction reckoned from gravity, limbs as stubs. That is the whole
     * rollback, and it is why this is a field rather than a rewrite. */
    uint8_t holds_line;

    /* SPROUTING: chance/256 per step that this material, standing in soil
     * with water in it, puts a cell of `sprouts_to` into an empty space
     * beside it - and spends a level of that soil's moisture doing it.
     *
     * The other direction of the same loop `grows` makes: growth turns a
     * plant into wood, and this turns wood back into new growth. It is
     * what stops a tree being a thing that happens once. Hardening
     * consumes the very cells that could grow, so a trunk that reached its
     * full height was finished for good, and anything that took its
     * foliage - fire, acid, a passing landslide - left a bare post that
     * could never recover. A trunk with wet ground at its foot buds
     * again.
     *
     * `sprouts_to` is a cell SPEC rather than a material id, the same as
     * `shatters_to` and for the same reason: what a tree buds is an
     * extended material, whose identity is its low nibble. */
    uint8_t sprouts;
    uint8_t sprouts_to;

    /* BUDDING: chance in 256 that this material, ALREADY IN LEAF and able
     * to reach water, puts out a cell of `buds_to` beside it.
     *
     * This is where a tree's growth comes from, and moving it here is the
     * point of the whole arrangement. It used to come from a growing tip:
     * hardening stopped one cell short so the stem kept a green cell, and
     * that cell - and every other green cell - rolled to grow, every step,
     * for as long as it existed.
     *
     * Two things were wrong with that. Growth scaled with how much green
     * was already there, which is a positive feedback loop, and the three
     * dampers on it (the packed brake, the lift cap, the hardening roll)
     * were all fighting the same runaway. And the green was PERMANENT: a
     * settled forest of four trees still ran nine root-walks a step and
     * grew nothing at all, for ever, because eleven cells that could not
     * grow kept asking whether they could.
     *
     * Budding from wood makes growth an EVENT rather than a population. A
     * finished tree has no plant cells at all, so it costs nothing and
     * cannot compound; plant becomes a transient phase a cell passes
     * through on its way to being timber.
     *
     * The `already in leaf` gate is what keeps the rate from scaling with
     * the tree again. Requiring a cell of `sprouts_to` alongside means only
     * crowned wood buds - a roughly constant handful per tree however fat
     * its trunk grows - rather than every cell of it. */
    uint8_t buds;
    uint8_t buds_to;

    /* DRINKING: chance/256 per step that this material, touching a liquid
     * AND rooted in soil that has room for more, takes a unit of that
     * liquid and puts a level of moisture into the ground it is standing
     * in. Water on the leaves comes out at the roots.
     *
     * It exists because a plant is `KIND_STATIC` with stone's density -
     * every extended STATIC is, they share one physics row (gunpowder is
     * the one extended-range material that does not, see MATERIAL_ROWS's
     * own comment, and it is not a plant) - so water cannot fall through a
     * thicket and cannot be soaked up by it either.
     * Pour into a bowl of foliage and the water sits there for ever with
     * nowhere to go, which is what it looked like.
     *
     * It cannot simply be `soaks`. That path raises the cell's own variant
     * to hold what it took, and a plant's variant is WHICH EXTENDED
     * MATERIAL IT IS - soaking one level would silently turn a plant into
     * the next thing in the extended table. Conducting the water to the
     * ground is both the safe answer and the better one: it is what a
     * plant does with water, and it means watering a canopy feeds the
     * tree instead of feeding a puddle. */
    uint8_t drinks;

    /* What a THERMALLY SHOCKED cell of this material becomes: hot enough to
     * be near the top of its ramp, and touching something that `chills`.
     *
     * Glass names MAT_SAND, which closes the loop it opens - sand becomes
     * glass under heat, glass becomes sand again when shocked - so the
     * material can be un-made by the player without a new material and
     * without a slot. */
    uint8_t shatters_to;
} reaction_t;

/* Indexed by the material nibble, same as materials[] - `const`, so it
 * costs no RAM either. Rows not given here default to all-zero, and that is
 * NOT the same as neutral. Zero reads harmlessly for most fields - never
 * catches, never a heat source, never smokes - but `dissolvable` 0 means
 * IMMUNE TO ACID and `conducts` 0 means HEAT STOPS HERE, which are
 * behaviours, not absences. Glass shipped with both from one missing row:
 * the acid immunity was the whole reason it exists and the heat block was a
 * bug, and nothing in the source told them apart. Ask what zero means field
 * by field before leaving a row out. */
extern const reaction_t reactions[MATERIAL_MAX];

/* The extended materials' own reaction rows, indexed by the low nibble of
 * a cell whose material is MAT_EXTENDED - the WHOLE nibble, statics and
 * gunpowder alike (MATERIAL_EXTENDED_CODES, not MATERIAL_EXTENDED_COUNT;
 * see that constant's own comment). Rows not given are all-zero, which for
 * an inert decorative block is the right answer - but read reaction_t's own
 * note on what zero means field by field before relying on that. */
extern const reaction_t extended_reactions[MATERIAL_EXTENDED_CODES];

/* The extended materials, by low nibble. */
typedef enum {
    MATX_ICE = 0,
    MATX_PLANT,
    MATX_LEAF,

    /* METAL: dirt smelted by sustained heat - see
     * docs/Sand/Metal-Smelting-Plan.md. Briefly slot 5 while the leaf
     * ageing chain held slots 3 and 4; back to 3 now that chain is gone.
     * Eleven extended slots remain after it. */
    MATX_METAL,

    /* ROOT: what a plant welds itself to the soil with as it drinks - see
     * reaction_t.roots. Not more wood: wood's variant is burn progress
     * (reactions[MAT_WOOD].burn_decay), so a root wearing a wood cell
     * would read as a nearly-burnt-out log rather than as buried anchor.
     * An extended material needs no variant of its own - the low nibble
     * IS the identity - so ROOT costs a slot rather than a byte, the same
     * trade METAL made above. Ten extended slots remain after it. */
    MATX_ROOT,
} material_extended_t;

_Static_assert(MATX_ROOT < MATERIAL_EXTENDED_COUNT,
               "a static's low nibble must fit in the 0-7 range MATX() now "
               "masks to - the upper half of the nibble is gunpowder's");

/* What to call one cell, decoding the extended range. materials[].name is
 * shared within each half of the range - "Extended" for all eight statics,
 * "Gunpowder" for all eight gunpowder codes - which is right for the
 * physics row and too coarse for a label (a static needs its own name, and
 * even gunpowder's single physics-row name would rather come from the same
 * table every other extended material's name does). extended_names[]
 * below gives every one of the sixteen codes its own entry instead. */
const char *material_name(cell_t c);

/* One extended STATIC material as a whole cell. There is no variant to
 * choose - the low nibble IS the identity - so this is the complete cell
 * byte, and it is what gets passed to sand_spawn_cell() and stored in a
 * brush list.
 *
 * Masks to 0x07, not 0x0F, now that nibble 15 is split in half (see
 * MATERIAL_ROWS's own comment in this file) - k names one of
 * MATERIAL_EXTENDED_COUNT statics, and a value of 8 or more would land in
 * gunpowder's half instead of wrapping an extended id, silently. */
#define MATX(k) ((cell_t)((MAT_EXTENDED << 4) | ((k) & 0x07)))

/* GUNPOWDER. The other half of nibble 15 - see MATERIAL_ROWS's own comment
 * for why the split exists at all: gunpowder needs KIND_POWDER physics no
 * extended STATIC could ever have, and there was no ordinary slot left to
 * give it (docs/Sand/Explosion-Plan.md's "Gunpowder's slot").
 *
 * 0xF8-0xFF: same high nibble as an extended static (MAT_EXTENDED), bit 3
 * of the low nibble set instead of clear. That single bit is the whole
 * difference material_of() reads - cell >> 3 lands one row lower for a
 * static than for gunpowder - so telling the two apart costs nothing more
 * than the split already costs. */
#define GUNPOWDER_BASE ((cell_t)((MAT_EXTENDED << 4) | 0x08))
#define GUNPOWDER_CELL(v) ((cell_t)(GUNPOWDER_BASE | ((v) & 0x07)))

_Static_assert((GUNPOWDER_BASE >> 3) == MATERIAL_ROW(MAT_EXTENDED) + 1,
               "gunpowder's row has to be the upper half of MAT_EXTENDED's "
               "twin pair - material_of()'s cell >> 3 and this constant's "
               "own bit-3 split have to land on the exact same row or the "
               "hot table and the byte layout silently disagree");

/* THE MOISTURE CODEC'S OWN SHAPE - the same two numbers material.c's
 * GUNPOWDER_REACTION spends on `.tones`/`.moist_max`, pulled out here so
 * GUNPOWDER_LIT below can be checked against them by construction (the
 * _Static_assert right after it) rather than by two places in the source
 * happening to agree today and silently drifting apart tomorrow. */
#define GUNPOWDER_TONES 3
#define GUNPOWDER_MOIST_MAX 4

/* THE LIT CODE - the one gunpowder code above its moisture range (codes
 * 0-2 dry, 3-6 moisture 1-4, see the reaction row's own comment,
 * material.c) rather than one more moisture level, the way section 2's
 * design once spent it. A cell at this code is on fire, the same
 * standing wood's variant != 0 has always had - see reaction_t.lit_from.
 */
#define GUNPOWDER_LIT 7
#define GUNPOWDER_LIT_CELL GUNPOWDER_CELL(GUNPOWDER_LIT)

_Static_assert(GUNPOWDER_LIT == GUNPOWDER_TONES + GUNPOWDER_MOIST_MAX,
               "GUNPOWDER_LIT has to sit exactly one past every dry tone "
               "and every moisture level - moisture_of()'s clamp and "
               "cell_is_burning()'s own threshold both rely on nothing "
               "between the wet codes and the lit one");

/* GUNPOWDER'S BLAST RADIUS - read off `reaction_t.explodes` (see that
 * field's own comment) at burn-out, in step_one_burning_cell()
 * (sand_reactions.c). Sized against the other two radii this simulation
 * already has, not invented fresh: the confined-gas pocket detonates at
 * SAND_GAS_IGNITE_BLAST_RADIUS 8 (sand_reactions.c), the covered-lava
 * burst at SAND_LAVA_BURST_RADIUS 12 (sand.h). Gunpowder is the BIGGEST
 * reaction-driven blast on the board, on purpose: it started at 6, under
 * both, and on the device the gas pocket then read as the more meaningful
 * blast - wrong for the one material whose whole point is to go off - so
 * it took the lava burst's old 16, the lava burst dropped to 12, and 16
 * then went to 20 on a second look at the panel. Only the hand-fired
 * detonate mode (app_sand.c, ~25 cells) is larger, and the impulse buffer
 * (APP_IMPULSE_MAX 2048) is what stops this growing much further. With
 * blasts spaced out by a board-wide cooldown
 * (SAND_GUNPOWDER_BLAST_COOLDOWN), the radius is what carries the punch.
 * The CORE that becomes fire is radius / SAND_EXPLODE_CORE_DIVISOR
 * (5, sand.h): four cells here, against one for the gas pocket. */
#define SAND_GUNPOWDER_BLAST_RADIUS 20

/* Whether this cell is gunpowder - the high nibble is MAT_EXTENDED AND bit
 * 3 of the low nibble is set. Every gunpowder byte, whatever its 3-bit
 * variant, tests true here and false for cell_is_extended() below - the two
 * are a partition of nibble 15, never both true of the same byte. */
static inline bool cell_is_gunpowder(cell_t c)
{
    return (c & 0xF8) == GUNPOWDER_BASE;
}

/* Whether this cell is one of the extended STATICS - narrowed from "nibble
 * is MAT_EXTENDED" to "and bit 3 of the low nibble is clear" now that
 * gunpowder has claimed the other half of that nibble. Every existing
 * caller of this function already meant statics only (ice, plant, leaf,
 * metal, root are all inert), so the narrowing changes nothing for them;
 * see reaction_of() below for the one place that has to see BOTH halves
 * and therefore does not use this test. */
static inline bool cell_is_extended(cell_t c)
{
    return (c & 0xF8) == (uint8_t)(MAT_EXTENDED << 4);
}

/* Whether this cell may be used as an emitter's material - see
 * sand_add_emitter() in sand.h. True for KIND_POWDER, KIND_LIQUID and
 * KIND_GAS; false for KIND_STATIC and KIND_NONE, because a static source
 * buries itself on its first emitted cell and jams forever.
 *
 * material_of() now DOES tell gunpowder apart from an extended static - see
 * MATERIAL_ROWS's own comment - so this is no longer "every extended
 * material reads as KIND_STATIC together"; it is exactly as true as it ever
 * was for ice, plant, leaf, metal and root (still one shared static row),
 * and gunpowder is the one extended-range byte that now reads KIND_POWDER
 * and is emitter-eligible with them. See
 * test_the_extended_row_being_static_is_what_emitter_eligibility_leans_on in
 * suite_sand.c, extended to pin the new boundary. */
static inline bool material_can_emit(cell_t c)
{
    const uint8_t kind = material_of(c)->kind;
    return kind == KIND_POWDER || kind == KIND_LIQUID || kind == KIND_GAS;
}

/* GENERIC CELL-CODE HELPERS. The sub-nibble value a cell's OWN material
 * spends on shade, tone or moisture - "code" rather than "variant" because
 * gunpowder does not get the whole nibble: it shares byte range 0xF0-0xFF
 * with the extended statics by splitting on bit 3 (GUNPOWDER_BASE), so its
 * code is only the low THREE bits. Everything else still has the whole
 * nibble, exactly as CELL_VARIANT() already reads it.
 *
 * These exist so the moisture codec below (and anything else keyed off "the
 * value this cell's variant is currently holding") can be written once and
 * work for both dirt's four-bit codec and gunpowder's three-bit one,
 * instead of the two needing separate copies of every helper. Defined here,
 * ahead of reaction_of()/cell_is_burning() below, because cell_is_burning()
 * itself now reads cell_code() (gunpowder's lit code is code 7, not
 * variant != 0). */
static inline uint8_t cell_code(cell_t c)
{
    return cell_is_gunpowder(c) ? (uint8_t)(c & 0x07) : CELL_VARIANT(c);
}

/* Keeps every identity bit - the material nibble, or gunpowder's whole top
 * five bits - and replaces only the code. `v` is masked to whichever width
 * this cell's own codec uses, the same way CELL_MAKE() already masks to
 * four bits for an ordinary cell. */
static inline cell_t cell_with_code(cell_t c, uint8_t v)
{
    return cell_is_gunpowder(c)
               ? (cell_t)((c & 0xF8) | (v & 0x07))
               : (cell_t)((c & 0xF0) | (v & 0x0F));
}


/* The reaction row for a cell, decoding the extended range.
 *
 * The one place that has to know MAT_EXTENDED exists, and it can afford
 * to: this is only ever called from sand_reactions.c, which is the cold
 * pass. material_of() deliberately does NOT decode - the sweep reads it
 * per cell per step, and the hot table already keeps statics and gunpowder
 * a single shift-and-index apart (MATERIAL_ROWS's own comment).
 *
 * Tests the whole MAT_EXTENDED nibble, NOT cell_is_extended() - that test
 * narrowed to statics only once gunpowder existed, and this is the one
 * place that has to route BOTH halves into extended_reactions[], keyed by
 * the full low nibble (0-7 statics, 8-15 gunpowder - see
 * MATERIAL_EXTENDED_CODES). One branch either way, same as before. */
/* Whether this cell is a heat source RIGHT NOW.
 *
 * Two different claims, deliberately in one place. `burns` means always -
 * fire, lava. `burn_decay` means while lit, and the variant says whether
 * it is. Writing the pair out by hand at each of the places that dispatch
 * on it is how the two would drift, and the one that matters most is the
 * may_have_burning latch: get it wrong there and a burning log never wakes
 * the reactions pass at all. */
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

/* TABLE-DRIVEN MOISTURE, the general form of soil's own state split
 * (SOIL_DRY_TONES / SOIL_MOISTURE_MAX above): codes below `r->tones` are a
 * dry TONE, codes from `r->tones` up to `r->tones + r->moist_max - 1` are
 * MOISTURE 1..moist_max. Dirt sets `.tones = 8, .moist_max = 7` (material.c),
 * which makes these byte-identical to CELL_MOISTURE()/CELL_WITH_MOISTURE()
 * for every code a dirt cell can hold - see
 * test_dirt_moisture_macros_and_codec_helpers_agree_on_every_byte
 * (suite_sand.c), which pins that equivalence. The dirt-only macros stay,
 * documented as dirt's own fixed instance of this codec, because 123 tests
 * already spell it that way; the ENGINE (sand_reactions.c, sand_priv.h,
 * sand.c) reads through these instead, so a second material with `dries !=
 * 0` needs no new copy of the soaking/heat/percolation code, only its own
 * `tones`/`moist_max` in its reaction row. */
static inline uint8_t moisture_of(cell_t c, const reaction_t *r)
{
    const uint8_t code = cell_code(c);
    if (code < r->tones) {
        return 0;
    }
    /* THE LIT CODE READS AS DRY, NOT SATURATED - `lit_from` (material.h),
     * not one more moisture level. Gated on `lit_from != 0` (only a
     * `burn_decay` material ever sets it) and checked before the moisture
     * arithmetic below, which would otherwise clamp a code this high to
     * moist_max exactly the way an ordinary out-of-range code does: harmless
     * for dirt, whose own one spare code (15) is simply never produced, but
     * gunpowder's spare code is GUNPOWDER_LIT and is asked about
     * constantly. Reading a lit grain as "moisture == moist_max" would let
     * try_heat_transform_given()'s wet-earth stage mistake it for damp soil
     * the moment some OTHER neighbour tried to heat-transform onto it,
     * driving a "moisture" level off a cell that has none and stepping on
     * the lit state's own bits. */
    if (r->lit_from != 0 && code >= r->lit_from) {
        return 0;
    }
    const uint8_t m = (uint8_t)(code - (uint8_t)(r->tones - 1u));
    return (m > r->moist_max) ? r->moist_max : m;
}

/* Setting m > 0 makes a WET cell, discarding whatever tone the code held
 * before - same "wet has no tone of its own" rule CELL_WITH_MOISTURE()
 * documents. NEVER call with m == 0; see soil_cell() below for the dry
 * case, which needs a tone to land on and this does not have one to fall
 * back to. */
static inline cell_t with_moisture(cell_t c, uint8_t m, const reaction_t *r)
{
    const uint8_t code = (uint8_t)((uint8_t)(r->tones - 1u) + ((m) > r->moist_max ? r->moist_max : (m)));
    return cell_with_code(c, code);
}

/* Soil built from scratch, tone and moisture given separately - the
 * table-driven form of CELL_SOIL(). `identity` already carries whichever
 * material/gunpowder bits the result should have; only the code changes.
 * m > 0 wins and tone is ignored, same rule with_moisture() follows. */
static inline cell_t soil_cell(cell_t identity, uint8_t tone, uint8_t m,
                               const reaction_t *r)
{
    const uint8_t code =
        (m != 0) ? (uint8_t)((uint8_t)(r->tones - 1u) + ((m) > r->moist_max ? r->moist_max : (m)))
                 : (uint8_t)((tone) >= r->tones ? (uint8_t)(r->tones - 1u) : (tone));
    return cell_with_code(identity, code);
}

/* The table-driven form of random_cell()'s dry-tone pick: scales a
 * neighbour's moisture (0..moist_max) down into a dry tone (0..tones-1) the
 * same way soil_dry_out() (sand_reactions.c) does for dirt - see that
 * function's own comment for why dirt needs no rescaling at all (its two
 * ranges are both seven wide, by construction, pinned by a
 * _Static_assert). A material whose ranges are NOT the same width - as
 * gunpowder's are not - genuinely needs this division. */
static inline uint8_t dry_tone_from_moisture(uint8_t m, const reaction_t *r)
{
    return (uint8_t)((m * (uint8_t)(r->tones - 1u)) / r->moist_max);
}

/* Whether two cells are the same SPECIES, for the one question that used to
 * be answered by comparing material nibbles alone: "is this neighbour more
 * of what I already am". That test conflated every extended material
 * together (all read nibble 15), which was harmless while all of them were
 * inert statics and stops being harmless the moment one half of that
 * nibble (gunpowder) actually reacts - a static ice block must not read as
 * "the same species" as gunpowder just because both carry MAT_EXTENDED.
 * Masking to the top five bits keeps every static reading as one species
 * together (unchanged from before - none of them soak) while separating
 * gunpowder from them, which is the one distinction that now matters. */
static inline bool same_species(cell_t a, cell_t b)
{
    if (CELL_MATERIAL(a) == MAT_EXTENDED || CELL_MATERIAL(b) == MAT_EXTENDED) {
        return (a & 0xF8) == (b & 0xF8);
    }
    return CELL_MATERIAL(a) == CELL_MATERIAL(b);
}

/* MATERIAL_SHADE_SPAN(m) is by id and cannot see gunpowder, whose shade
 * range is not a material_id_t's to look up - it lives in the extended
 * reaction row instead (`.tones`, 3 for gunpowder - see GUNPOWDER_REACTION
 * in material.c). `spec` is a whole cell byte (identity bits, any code),
 * the same shape random_cell()'s callers already pass around. */
static inline int material_shade_span_cell(cell_t spec)
{
    if (cell_is_gunpowder(spec)) {
        return reaction_of(spec)->tones;
    }
    return MATERIAL_SHADE_SPAN((material_id_t)CELL_MATERIAL(spec));
}

/*---------------------------------------------------------------------------
 * Rendering
 *-------------------------------------------------------------------------*/

/* A stable scatter value for one cell, so a speckled material shows the
 * same grain in the same place every frame.
 *
 * It lives here, next to the tables that consume it, rather than in
 * app_sand.c where it started - because in app_sand.c nothing could test
 * it, and it was badly wrong for a long time in a way that only a test
 * would have caught. The low three bits, which are the ones every caller
 * actually uses, came out very nearly CONSTANT ALONG A ROW:
 *
 *     077777777777777777777460
 *     433333333333333333333024
 *     166666666666666666666571
 *
 * So stone and wood were not speckled at all. They were drawn in flat
 * horizontal stripes, one shade per row, which is exactly what "the same
 * screenspace shade issue" and "banding, the pattern repeats" describe.
 * The cause is dull: xor two multiplied words and the low bits of the
 * result depend only on the low bits of the inputs, and one shift-xor is
 * not enough to fix that. It needs a real finalising round - a multiply by
 * an odd constant with good avalanche, then another shift-xor - so that
 * every output bit depends on every input bit.
 *
 * Measured after: over 128x128, horizontally adjacent cells share a shade
 * 2077 times out of 16256, against an ideal of 2032, and the eight buckets
 * differ by 131 on a mean of 2048.
 *
 * The extra multiply is per cell per PAINTED row, and painted rows are
 * only the ones that changed - a settled pile costs nothing. */
static inline unsigned material_grain_hash(int cx, int cy)
{
    unsigned h = (unsigned)cx * 0x9E3779B9u ^ (unsigned)cy * 0x85EBCA6Bu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

/* A body index shared by GLASS and METAL - position projected onto
 * CURRENT GRAVITY (ux_q8/uy_q8, material_shine_direction()'s own Q8 unit
 * vector), not a hash. Steps read as laminae perpendicular to "down",
 * turning with a tilt instead of sitting fixed to the screen. */

/* `jitter` is the one thing the two callers disagree on: metal passes 0
 * and keeps a razor edge between steps, which reads as brushed; glass
 * passes a per-cell hash there instead, since that same sharp edge read
 * as machined metal, not a hand-blown pane. */
static inline unsigned material_gravity_band(int cx, int cy, int ux_q8,
                                             int uy_q8, int period, int jitter)
{
    /* >> 8 undoes the Q8 scale, so `step` climbs by about one per cell
     * moved along the gravity axis; the double modulo wraps the jittered
     * sum into range from either side, since a dot product can go
     * negative. */
    const long step = (((long)cx * ux_q8 + (long)cy * uy_q8) >> 8) + jitter;
    return (unsigned)(((step % period) + period) % period);
}

/* Every possible cell byte, mapped straight to a panel-ready pixel.
 *
 * 256 entries of two bytes: 512 bytes, in flash, costing no RAM. Drawing a
 * cell becomes one array index - no material lookup, no shade arithmetic, no
 * colour conversion. Faster than computing it, and free.
 *
 * Indexed by the raw cell byte, so `material_palette()[cell]` is the whole of
 * "what colour is this?". */
const gfx_color_t *material_palette(void);

/* How one cell is painted inside its own block of pixels. PURELY VISUAL -
 * nothing here is read by the simulation, and changing any of it changes
 * only what the panel shows.
 *
 * The point of the split is that FLAT stays free. A flat material fills
 * its block with one colour in the same tight loop it always did; only the
 * materials that ask for a pattern pay for one, and only where they
 * actually appear on the board. */
typedef enum {
    MATERIAL_FLAT = 0,      /* one colour, whole block */
    MATERIAL_SPECKLED,      /* one colour per cell, varied by POSITION */
    MATERIAL_HATCHED,       /* diagonals both ways, bright where they cross */
} material_pattern_t;

/* Which of a cell's eight neighbours are empty, as an 8-bit mask - what
 * `edge` used to collapse to a single bool before a liquid needed to know
 * WHICH side was open rather than merely whether one was.
 *
 * The order is arbitrary but fixed, and every reader of a mask has to
 * agree on it: app_sand.c's paint_row_n(), which builds one per cell, and
 * material_set_gravity() below, which turns one into a specular shift.
 * Grid/screen coordinates throughout this app have y increasing DOWN the
 * screen, so "up" is the -y direction and "down" is +y.
 *
 * THE FOUR CARDINALS, bits 0-3, are the whole of the mask that existed
 * before this comment did, and they alone decide whether a cell is an
 * EDGE at all - see MATERIAL_EDGE_CARDINAL below. Glass and stone only
 * ever ask that question; a liquid's interior/rim split asks it too,
 * before asking anything finer.
 *
 * THE FOUR DIAGONALS, bits 4-7, were added later and refine a WATER rim's
 * FOAM - see material_colours()'s own comment on curvature for what they
 * are for. Nothing else reads them: a diagonal alone, with every cardinal
 * neighbour occupied, must NOT read as an edge, which is exactly the trap
 * MATERIAL_EDGE_CARDINAL exists to avoid (see its own comment, and
 * test_a_diagonal_neighbour_alone_is_not_an_edge in suite_sand.c, which
 * pins it). */
#define MATERIAL_EDGE_LEFT       (1u << 0)
#define MATERIAL_EDGE_RIGHT      (1u << 1)
#define MATERIAL_EDGE_UP         (1u << 2)
#define MATERIAL_EDGE_DOWN       (1u << 3)
#define MATERIAL_EDGE_UP_LEFT    (1u << 4)
#define MATERIAL_EDGE_UP_RIGHT   (1u << 5)
#define MATERIAL_EDGE_DOWN_LEFT  (1u << 6)
#define MATERIAL_EDGE_DOWN_RIGHT (1u << 7)

/* THE "IS THIS CELL AN EDGE AT ALL" TEST, and ONLY the four cardinal bits.
 *
 * Before the diagonals existed, `mask != 0` was that test, because the
 * mask held nothing else. Once bits 4-7 exist, `mask != 0` silently
 * changes meaning: a cell with every cardinal neighbour occupied but ONE
 * diagonal empty would newly read as an edge, and glass and stone would
 * start outlining cells they used to paint as solid interior. Both of
 * them, and the liquid interior/rim split, test `(mask &
 * MATERIAL_EDGE_CARDINAL) != 0` instead - never `mask != 0` - so that
 * adding the diagonals could not change what either of them draws. */
#define MATERIAL_EDGE_CARDINAL                                           \
    (MATERIAL_EDGE_LEFT | MATERIAL_EDGE_RIGHT | MATERIAL_EDGE_UP |       \
     MATERIAL_EDGE_DOWN)

/* How many distinct values the CARDINAL mask alone can take - 16, one per
 * combination of its four bits. `liquid_spec[]` in material.c is sized and
 * indexed by this, not by MATERIAL_VARIANTS: the two happen to both be 16
 * today, which is exactly the kind of coincidence that looks like a
 * decision and is not one - see liquid_spec's own comment in material.c. */
#define MATERIAL_EDGE_MASK_COUNT (MATERIAL_EDGE_CARDINAL + 1u)

/* Fills in the colours this cell is painted with and says how to arrange
 * them: `out[0]` is the body, `out[1]` the diagonal lines, `out[2]` where
 * two lines cross. A flat or speckled material sets all three the same.
 *
 * `hash` is any stable per-cell number - a speckled material uses it to
 * pick its shade, so the same cell keeps the same one frame to frame. */
/* `mask` is which of this cell's eight neighbours are empty - see the
 * MATERIAL_EDGE_* bits above. Anything that only cares WHETHER this cell
 * is an edge at all, rather than which side, tests
 * `(mask & MATERIAL_EDGE_CARDINAL) != 0`: that is exactly what the old
 * bool `edge` meant, back when the mask held only the four cardinals, and
 * every existing reader (glass, stone) keeps exactly its old behaviour
 * under that test. Testing `mask != 0` instead would be wrong now that
 * diagonal bits exist - see MATERIAL_EDGE_CARDINAL's own comment.
 *
 * A material whose colour tracks a temperature moves much less on its
 * outline than in its body, so a wall keeps its shape as it heats instead
 * of the silhouette itself changing colour. The heat is still perfectly
 * visible; it is just shown by the inside of the wall rather than by its
 * edge against the background.
 *
 * A LIQUID reads the mask more finely still - see material_colours()'s own
 * comment in material.c for why its interior ignores fill level entirely
 * and its rim both keeps the fill ramp and shades by which way the empty
 * side faces against gravity. WATER's rim reads the mask finer again, all
 * eight bits of it, to decide where FOAM gathers - see that same comment's
 * discussion of curvature.
 *
 * `depth` is this liquid cell's LOCAL DEPTH: 0 at this material's own
 * boundary - the neighbour one step toward the surface is a different
 * material, empty space, or solid, anything that is not the same body of
 * liquid - climbing by one for each further cell into the body, clamped at
 * 255. LOCAL, not a screen position: see paint_row_n() in app_sand.c, which
 * walks it fresh from the live grid every row, for the full mechanism and
 * the two device reports ("the sensibility against gravity makes it behave
 * almost like platinum"; "maybe it's better if the depth just follows the
 * shape of the puddle") that replaced the old screen-position gradient with
 * this one. Only a liquid's INTERIOR reads it; everything else - the rim
 * included - ignores the value entirely, so any depth may be passed where
 * it does not apply.
 *
 * `depth` no longer feeds anything but a plain shade-index shift, for every
 * liquid including water - see DEPTH_SATURATE_CELLS's own comment in
 * material.c for the scale that shift saturates against, and for why water
 * used to also read an animated wave-table index and a haze blend here and
 * no longer does (a genuine bug in the fog blend's own arithmetic, plus a
 * column-artifact risk from riding straight over local depth's own axis
 * seam - both still exist, untouched, on the water-wave-fog-depth-banked
 * branch for anyone who wants to revisit that approach).
 *
 * FOR A ROOT CELL, `depth` MEANS SOMETHING ELSE: how many of its eight
 * neighbours are also root - material_root_neighbours() below, which the
 * painter substitutes for the liquid walk's number on exactly that one
 * material. A root has no variant to carry an age in (its low nibble is
 * which extended material it is), and the ordinary table is full, so
 * "how old is this root" cannot be stored. It can be READ off the shape,
 * though: a fresh tip touches one other root, a cell that has put out
 * children touches two or three, the collar touches more. Shading by that
 * count is what the maintainer asked for - "as one root grows from
 * another, it darkens its parent" - with no state at all, and it heals in
 * the direction stored age never could: lose a child to rot or lava and
 * the parent lightens again. Reusing `depth` rather than adding a
 * parameter keeps the painter's call and this function's signature exactly
 * as measured. */
material_pattern_t material_colours(cell_t c, unsigned hash, unsigned mask,
                                    unsigned depth, gfx_color_t out[3]);

/* How many of (cx)'s eight neighbours, across the three grid rows handed
 * in, are root. `above`/`below` may be NULL at the top and bottom of the
 * grid, exactly as paint_row_n() already passes them for the edge mask.
 * Eight reads, paid only for a root cell - the painter gates the call on
 * the cell's own byte, one compare, the same cost metal's own leading
 * equality test already added to that path. Header-inline so app_sand.c
 * and the host suite compile the one definition. */
static inline unsigned material_root_neighbours(const uint8_t *above,
                                                const uint8_t *row,
                                                const uint8_t *below,
                                                int cx, int w)
{
    const cell_t root = MATX(MATX_ROOT);
    unsigned n = 0;
    const int l = cx - 1, r = cx + 1;
    if (l >= 0) {
        n += (row[l] == root);
        if (above) n += (above[l] == root);
        if (below) n += (below[l] == root);
    }
    if (r < w) {
        n += (row[r] == root);
        if (above) n += (above[r] == root);
        if (below) n += (below[r] == root);
    }
    if (above) n += (above[cx] == root);
    if (below) n += (below[cx] == root);
    return n;
}

/* Called once per frame, before painting, with the same gravity vector
 * this frame's sand_step() was given.
 *
 * Fills a MATERIAL_EDGE_MASK_COUNT-entry table - one entry per possible
 * CARDINAL MATERIAL_EDGE_* mask, diagonals excluded, since gravity has no
 * opinion about foam - with how much a liquid rim cell at that mask should
 * brighten (negative) or darken (positive) relative to its own fill level.
 * All the trig this needs lives here, ONCE, so that material_colours() -
 * called per cell per painted row, and hot - pays for it with a single
 * array index and nothing else. See material.c for the derivation.
 *
 * Does NOT derive `depth` any more, and no longer takes a grid size for
 * that reason - `depth` is LOCAL to each puddle now, walked fresh per cell
 * against the live grid by app_sand.c's paint_row_n(), which gravity alone
 * cannot work out ahead of time the way the old screen-position gradient
 * could (that walk needed only gravity's direction and the grid's extent,
 * both known up front; this one needs to look at neighbouring cells as
 * they are painted). The one thing left that still depends purely on
 * gravity's DIRECTION is the specular table above. */
void material_set_gravity(int gx, int gy);

/* The Q8 unit vector a travelling shine should sweep along, for this frame's
 * gravity - what turns paint_row_n()'s HATCHED shine band from a fixed
 * diagonal into one that tracks the way the board is held.
 *
 * MINUS gravity, same convention liquid_spec's highlight above uses, then
 * turned 45 degrees to the left as seen on the panel: the shine sweeps
 * toward wherever up currently is, leaning, so its bands cross gravity at
 * a slant instead of lying flat across it. See material.c for why the
 * slant was added and how the turn folds into the arithmetic.
 *
 * Pure and stateless, unlike material_set_gravity() - there is no per-cell
 * table to fill ahead of a hot loop, just two numbers the caller reads once
 * a frame and carries into paint_row_n() itself (see shine_ux_q8/shine_uy_q8
 * there). Degenerate input (flat, or free fall - `im_len(gx, gy) == 0`) has
 * no "up" to sweep toward, so it returns the plain (1, 1) diagonal the shine
 * always travelled before gravity had any say in it, rather than inventing
 * a direction nothing measured. */
void material_shine_direction(int gx, int gy, int *ux_q8, int *uy_q8);

/* Called once per frame, before painting, with a value that climbs steadily
 * over real time - see app_sand.c's own FOAM_PHASE_MS for how it derives one
 * from dt_ms.
 *
 * Deliberately a SEPARATE call from material_set_gravity() above, even
 * though both run at the same point in sand_frame() and both feed
 * material_colours(). Gravity is a per-frame FACT about the board - where
 * "down" currently points - and phase is a per-frame fact about the CLOCK -
 * how much time has passed. Folding a clock reading into the gravity setter
 * would make the two impossible to test apart: a test that wants to sweep
 * phase alone would have to also supply a gravity vector, and a test that
 * wants to sweep gravity alone would be at the mercy of whatever the phase
 * happened to be. Two setters, two independent things to reason about.
 *
 * See material_colours()'s own comment on foam for what the phase is
 * for and how it is mixed into the foam dither - it is not simply added to
 * the hash, and the reason why is written there. */
void material_set_foam_phase(unsigned phase);

/* Called once per frame, before painting, with a value that climbs steadily
 * over real time - see app_sand.c's own CULLET_PHASE_MS for how it derives
 * one from dt_ms. Exactly the same shape as material_set_foam_phase() just
 * above, and deliberately a THIRD, separate call rather than folded into
 * either of the other two: this is a fact about a different clock, ticking
 * at a different rate, feeding a different material - a test sweeping
 * cullet's phase must not have to also feed a gravity vector or a foam
 * phase, and vice versa.
 *
 * See material_colours()'s own MAT_SAND case for what the phase means -
 * which quarter-turn of the shared cullet_cycle[] (material.c) each cullet
 * shade currently reads as - and material.h's own rewritten comment on
 * SAND_CULLET_BASE for why the shade no longer names one fixed colour by
 * itself. */
void material_set_cullet_phase(unsigned phase);
