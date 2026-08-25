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

#include "gfx_color.h"

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

/* A liquid cell holds between 1 and 15. Zero is not a very empty cell - it is
 * no cell at all, and must be written as CELL_EMPTY, or the material nibble
 * leaves an occupied cell holding nothing. */
#define MASS_MAX 15

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
    MAT_EMBER,
    MAT_OIL,
    MAT_LAVA,
    MAT_ACID,
    MAT_COUNT
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

/* Indexed by the material nibble. `const`, so it is in flash and costs no RAM. */
extern const material_t materials[MATERIAL_MAX];

/* No bounds check: the nibble is four bits and the table has sixteen rows, so
 * every possible value is already a valid index. */
static inline const material_t *material_of(cell_t c)
{
    return &materials[CELL_MATERIAL(c)];
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
     * into an adjacent empty cell. Meaningless for anything that is not a
     * static heat source with nothing above it - left at zero for
     * everything but the one material that needs to look like it is
     * licking a flame upward while staying put itself. */
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
} reaction_t;

/* Indexed by the material nibble, same as materials[] - `const`, so it
 * costs no RAM either. Rows not given here default to all-zero
 * (designated initializers zero the rest), which reads correctly for
 * every field above: never catches, never a heat source, never conducts,
 * never smokes, vanishes on quench, never flares. */
extern const reaction_t reactions[MATERIAL_MAX];

static inline const reaction_t *reaction_of(cell_t c)
{
    return &reactions[CELL_MATERIAL(c)];
}

/*---------------------------------------------------------------------------
 * Rendering
 *-------------------------------------------------------------------------*/

/* Every possible cell byte, mapped straight to a panel-ready pixel.
 *
 * 256 entries of two bytes: 512 bytes, in flash, costing no RAM. Drawing a
 * cell becomes one array index - no material lookup, no shade arithmetic, no
 * colour conversion. Faster than computing it, and free.
 *
 * Indexed by the raw cell byte, so `material_palette()[cell]` is the whole of
 * "what colour is this?". */
const gfx_color_t *material_palette(void);
