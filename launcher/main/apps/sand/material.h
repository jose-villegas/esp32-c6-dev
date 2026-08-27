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
/* SOIL packs TWO things into its four bits: a carried tone in the top
 * bit and moisture in the low three.
 *
 * Moisture alone had the whole nibble, and the top half of it was dead.
 * Measured over six waterings of a dirt bank, 99.98% of soil cells sat at
 * 7 or below and 94.6% at 3 or below - half-the-difference diffusion
 * spreads water thin almost immediately, so saturation is a state soil
 * passes through rather than one it sits in. Three bits cost 0.02% of
 * observed states.
 *
 * What the freed bit buys is the one thing soil could not have: a shade
 * that TRAVELS WITH THE GRAIN. Dirt was drawn with a positional hash, like
 * stone and wood, and that is fine for materials that never move - but
 * dirt falls and piles, so the texture stayed nailed to the screen while
 * the dirt slid underneath it. Reported as "the dirt shading even seems to
 * be related to screen pos, the pattern repeats", which is exactly what a
 * screen-space hash is.
 *
 * A carried tone instead means a poured bank keeps the pattern it was
 * poured with, and the strata show the shape of the pile - the thing sand
 * has always done with its shade, and the thing dirt was missing. It costs
 * no render-side work at all, because palette[] is indexed by the whole
 * cell byte: the tone and the wetness ramp are simply different entries. */
#define SOIL_MOISTURE_BITS  3
#define SOIL_MOISTURE_MAX   ((1u << SOIL_MOISTURE_BITS) - 1u)
#define SOIL_TONES          (MATERIAL_VARIANTS >> SOIL_MOISTURE_BITS)

#define CELL_MOISTURE(c)    ((uint8_t)(CELL_VARIANT(c) & SOIL_MOISTURE_MAX))
#define CELL_SOIL_TONE(c)   ((uint8_t)(CELL_VARIANT(c) >> SOIL_MOISTURE_BITS))

/* Same cell, same tone, different wetness. */
#define CELL_WITH_MOISTURE(c, m)                                          \
    CELL_MAKE(CELL_MATERIAL(c),                                           \
              (uint8_t)((CELL_VARIANT(c) & ~SOIL_MOISTURE_MAX) |          \
                        ((m) & SOIL_MOISTURE_MAX)))

/* And soil built from scratch, tone and wetness given separately. */
#define CELL_SOIL(mat, tone, m)                                           \
    CELL_MAKE((mat), (uint8_t)((((tone) & (SOIL_TONES - 1))               \
                                    << SOIL_MOISTURE_BITS) |              \
                               ((m) & SOIL_MOISTURE_MAX)))

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
     * one of MATERIAL_EXTENDED_COUNT further materials, so the last slot
     * buys sixteen rather than one.
     *
     * They cost nothing in the sweep, and the reason is entirely about how
     * the tables are already indexed:
     *
     *   materials[]   is read by the NIBBLE, so all sixteen share one row
     *                 and material_of() does not change at all
     *   palette[]     is read by the whole CELL BYTE, so each of the
     *                 sixteen already has its own entry, for free
     *   reactions[]   is read only by sand_reactions.c - the cold pass -
     *                 so reaction_of() can afford to decode
     *
     * What they buy: their own colour and their own reactions. What they
     * cannot have: their own physics, since materials[MAT_EXTENDED] is one
     * shared row; or a variant, since the low nibble is spent saying which
     * one they are. That confines them to inert static solids, which is
     * the price of not touching the hot loop. */
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
 * The same trick as soil's tone and free for the same reason - sand's
 * variant is already a shade, so saying "this grain came from a pane"
 * costs no bits at all, only four of the sixteen shades it could have
 * been. Twelve is still far more variation than a dune needs.
 *
 * Shattered glass was already placed at the very top of the ramp, being
 * whatever place_reacted() hands a new cell, so it was already the
 * brightest sand there is - and it did not read, for two reasons the band
 * fixes together. It was one flat value, so a broken pane was a slab of
 * uniform colour; and the top of a ramp is still on that ramp, so
 * "brightest sand" and "sand" are the same warm tan a shade apart. The
 * cullet band is a different HUE - pale and cool, the colour of ground
 * glass rather than of beach - and it varies inside itself.
 *
 * It is also permanent, which is the nice part: sand's shade never
 * changes, so a heap keeps the memory of having been a window, and mixing
 * it into an ordinary dune leaves the two visibly distinguishable. */
#define SAND_DUNE_SHADES    12
#define SAND_CULLET_BASE    SAND_DUNE_SHADES
#define SAND_CULLET_SHADES  (MATERIAL_VARIANTS - SAND_CULLET_BASE)

/* How many shades a freshly PAINTED grain may pick from. Everything else
 * gets the whole range; sand stops short of its reserved band, which is
 * what keeps the band meaning anything. */
#define MATERIAL_SHADE_SPAN(m)                                            \
    ((m) == MAT_SAND ? SAND_DUNE_SHADES : MATERIAL_VARIANTS)

/* How many materials hide behind MAT_EXTENDED - one per value of the low
 * nibble. */
#define MATERIAL_EXTENDED_COUNT 16

_Static_assert(MAT_EXTENDED == MATERIAL_MAX - 1,
               "the extended range lives in the LAST nibble value, so that "
               "every ordinary material keeps the id it already had");
_Static_assert(MAT_COUNT <= MAT_EXTENDED,
               "an ordinary material has taken the extended range's slot - "
               "there is no room for both");

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

    /* And the way back: chance in 256 per step of losing one level of
     * whatever `soaks` put in. Dirt drying out.
     *
     * Non-zero is also what MARKS a material's variant as moisture, the
     * way heat_ramp marks it as temperature and burn_decay as how much is
     * left to burn. A freshly drawn cell of one starts at zero - bone
     * dry - which is why random_cell() has to know. */
    uint8_t dries;

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
     * every extended material is, they share one physics row - so water
     * cannot fall through a thicket and cannot be soaked up by it either.
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
 * a cell whose material is MAT_EXTENDED. Rows not given are all-zero,
 * which for an inert decorative block is the right answer - but read
 * reaction_t's own note on what zero means field by field before relying
 * on that. */
extern const reaction_t extended_reactions[MATERIAL_EXTENDED_COUNT];

/* The extended materials, by low nibble. */
typedef enum {
    MATX_ICE = 0,
    MATX_PLANT,
    MATX_LEAF,

    /* METAL: dirt smelted by sustained heat - see
     * docs/Sand/Metal-Smelting-Plan.md. Briefly slot 5 while the leaf
     * ageing chain held slots 3 and 4; back to 3 now that chain is gone.
     * Twelve extended slots remain after it. */
    MATX_METAL,
} material_extended_t;

/* What to call one cell, decoding the extended range. materials[].name is
 * shared across all sixteen extended materials, so it says "Extended" for
 * every one of them - which is right for the physics row and useless for a
 * label. */
const char *material_name(cell_t c);

/* One extended material as a whole cell. There is no variant to choose -
 * the low nibble IS the identity - so this is the complete cell byte, and
 * it is what gets passed to sand_spawn_cell() and stored in a brush list. */
#define MATX(k) ((cell_t)((MAT_EXTENDED << 4) | ((k) & 0x0F)))

/* Whether this cell is one of the extended materials. */
static inline bool cell_is_extended(cell_t c)
{
    return CELL_MATERIAL(c) == MAT_EXTENDED;
}

/* Whether this cell may be used as an emitter's material - see
 * sand_add_emitter() in sand.h. True for KIND_POWDER, KIND_LIQUID and
 * KIND_GAS; false for KIND_STATIC and KIND_NONE, because a static source
 * buries itself on its first emitted cell and jams forever.
 *
 * material_of() deliberately does not decode the extended range - see its
 * own comment above - so every extended material (Ice, Plant, Leaf, Metal)
 * reads as KIND_STATIC through the one physics row all sixteen share, and
 * they are excluded together. That is the right answer today only because
 * they all happen to be static - see
 * test_the_extended_row_being_static_is_what_emitter_eligibility_leans_on in
 * suite_sand.c, which pins the assumption this derives from. */
static inline bool material_can_emit(cell_t c)
{
    const uint8_t kind = material_of(c)->kind;
    return kind == KIND_POWDER || kind == KIND_LIQUID || kind == KIND_GAS;
}


/* The reaction row for a cell, decoding the extended range.
 *
 * The one place that has to know MAT_EXTENDED exists, and it can afford
 * to: this is only ever called from sand_reactions.c, which is the cold
 * pass. material_of() deliberately does NOT decode - the sweep reads it
 * per cell per step, and all sixteen extended materials sharing one
 * physics row is exactly what keeps that a single shift and index. */
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
    if (cell_is_extended(c)) {
        return &extended_reactions[CELL_VARIANT(c)];
    }
    return &reactions[CELL_MATERIAL(c)];
}

static inline bool cell_is_burning(cell_t c)
{
    const reaction_t *r = reaction_of(c);
    return r->burns != 0 || (r->burn_decay != 0 && CELL_VARIANT(c) != 0);
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

/* Fills in the colours this cell is painted with and says how to arrange
 * them: `out[0]` is the body, `out[1]` the diagonal lines, `out[2]` where
 * two lines cross. A flat or speckled material sets all three the same.
 *
 * `hash` is any stable per-cell number - a speckled material uses it to
 * pick its shade, so the same cell keeps the same one frame to frame. */
/* `edge` marks a cell with empty space cardinally beside it - the outline
 * of whatever it is part of. A material whose colour tracks a temperature
 * moves much less on its outline than in its body, so a wall keeps its
 * shape as it heats instead of the silhouette itself changing colour. The
 * heat is still perfectly visible; it is just shown by the inside of the
 * wall rather than by its edge against the background. */
material_pattern_t material_colours(cell_t c, unsigned hash, bool edge,
                                    gfx_color_t out[3]);
