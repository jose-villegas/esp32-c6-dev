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

/* Room temp 0-15 needs middle ambient for cold. SOIL: 0 to SOIL_DRY_TONES-1
 * dry, SOIL_DRY_TONES to SOIL_DRY_TONES+SOIL_MOISTURE_MAX moist, last unused.
 * Split states enhance dry tones, fix uniform wet bed, reduce noise. Tone and
 * moisture form monotone from dry to saturated. */
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

/* Setting m > 0 discards a cell's tone and moisture. NEVER use m == 0 as it
 * leads to flatness; use soil_dry_out() instead for varied drying. */
#define CELL_WITH_MOISTURE(c, m)                                          \
    CELL_MAKE(CELL_MATERIAL(c),                                          \
              (uint8_t)((SOIL_DRY_TONES - 1u) + ((m) & SOIL_MOISTURE_MAX)))

/* Soil built from scratch, tone and moisture separate. Caller may ask for m=0
 * with specific tone. Tone ignored if m > 0, following "wet has no tone"
 * rule. */
#define CELL_SOIL(mat, tone, m)                                           \
    CELL_MAKE((mat), (uint8_t)((m) != 0                                  \
                                    ? (SOIL_DRY_TONES - 1u) +             \
                                          ((m) & SOIL_MOISTURE_MAX)       \
                                    : ((tone) & (SOIL_DRY_TONES - 1u))))

#define SAND_AMBIENT_HEAT 3

/* Heat level where cell with `shatters_to` cracks. THREE agree: rule,
 * palette, tests. FOUR LEVELS ABOVE AMBIENT. Palette from gap. Ambient + 6:
 * 2.8 panes. Ambient + 4: 2.8 panes. Ambient + 2: 6.8 panes. Must be low
 * (touch 3.02). Snow chills contact. Four ensures safety. Two is minimum. */
#define SAND_SHOCK_HEAT (SAND_AMBIENT_HEAT + 2)

/* Below, sudden heat cracks cells. Thermal shock, not high temp, breaks
 * glass. Cold to hot breaks glass; hot to cold does not. Snow on lava makes
 * water, quenching heat. Frost vessel, then add heat. */
#define SAND_SHOCK_COLD (SAND_AMBIENT_HEAT - 2)

/* A liquid cell holds between 1 and 15. Zero is not a very empty cell - it is
 * no cell at all, and must be written as CELL_EMPTY, or the material nibble
 * leaves an occupied cell holding nothing. */
#define MASS_MAX 15

/* LOCAL DEPTH cells for shading saturation; shared with sand_liquid.c for
 * pour-staleness fix. Matches MATERIAL_EDGE_MASK_COUNT to avoid constant
 * disagreement. */
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

    /* ID 15 is an escape hatch. MAT_EXTENDED reads 16 codes from the LOW
     * nibble: 8 STATICS (0xF0-0xF7) and 1 KIND_POWDER split (0xF8-0xFF). No
     * extra cost in sweep. materials[] uses cell >> 3, sharing one row for
     * STATICS, another for KIND_POWDER. palette[] uses the whole cell byte,
     * giving each its own entry. reactions[] are decoded in sand_reactions.c.
     * STATICS have unique color/reactions, not physics/variant. KIND_POWDER
     * escapes these limits, losing half range. */
    MAT_EXTENDED = 15   /* the last nibble value; asserted against
                         * MATERIAL_MAX below, which this enum comes
                         * too early to reference */
} material_id_t;

/* The table pads nibble values, making material_of() a direct array index.
 * Without padding, each lookup needs bounds checking, reducing performance.
 * Flash storage is used as it's free, though CPU usage rises. Unused slots
 * are static and dense, blocking corrupted cells in simulations. */
#define MATERIAL_MAX 16

/* SAND's top band, CULLET, symbolizes shattered glass. It's free, being
 * already a shade, and is the brightest due to its position. CULLET has a
 * distinct, pale, cool hue that shimmers through various tints as part of a
 * shared, slow color cycle. */
#define SAND_DUNE_SHADES    12
#define SAND_CULLET_BASE    SAND_DUNE_SHADES
#define SAND_CULLET_SHADES  (MATERIAL_VARIANTS - SAND_CULLET_BASE)

/* Cullet colour cycle steps - see cullet_cycle[] and material_colours()'s
 * MAT_SAND case in material.c. Public for host tests in suite_sand_tone.c.
 * Power of two for mask use and multiple of SAND_CULLET_SHADES for
 * quarter-turn alignment, enforced by _Static_assert. */
#define CULLET_CYCLE_LEN 16

/* How many shades a freshly PAINTED grain may pick. Everything else gets the
 * full range; sand skips its reserved band, and dirt skips the wet range, as
 * a freshly poured cell is dry (see random_cell() in sand.c). */
#define MATERIAL_SHADE_SPAN(m)                                            \
    ((m) == MAT_SAND ? SAND_DUNE_SHADES                                  \
                      : (m) == MAT_DIRT ? SOIL_DRY_TONES : MATERIAL_VARIANTS)

/* How many STATIC materials hide behind MAT_EXTENDED's lower half - one per
 * value of the low nibble's bottom three bits, 0xF0-0xF7. Was sixteen (the
 * whole nibble); gunpowder now owns the upper half (0xF8-0xFF, see
 * GUNPOWDER_BASE), so eight codes remain for statics, three of them spare. */
#define MATERIAL_EXTENDED_COUNT 8

/* How many extended REACTION rows exist? 16, covering BYTE VALUES 0xF0-0xFF.
 * Entries 0-7 are statics, 8-15 share one GUNPOWDER_REACTION row, ensuring
 * reaction_of() uses a single decode branch. */
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

/* Small, movement fields first. Read multiple times per cell per step. C6
 * cache line is 32 bytes; compact entry avoids cache misses. No colour here;
 * palette is the single source. Duplicating it would lead to errors. */
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

    /* 1 in 256 chance per step for a grain's LIFE REMAINING (variant nibble)
     * to decrement. Zero means immortal: CELL_VARIANT remains constant. Decay
     * != 0 enables variant-as-life, not kind. Transient materials can reuse
     * without second field. */
    uint8_t decay;

    /* Each cell has a 1 in 256 chance to move per step. Lower values mean it
     * moves less often. Ignored when jostled. KIND_GAS (BUOYANCY) and
     * KIND_LIQUID (inverted VISCOSITY) use this. Liquids ignored it until oil
     * was added, then used it like gases. Powders set it to zero. */
    uint8_t mobility;

    /* How far a KIND_GAS material's spread pass (equalise_gas() in
     * sand_gas.c) looks for an empty cell to hop into. Now per-material to
     * allow gas and fire to disperse differently without affecting each
     * other. Meaningless outside a gas pass, so other materials leave it at
     * zero. */
    uint8_t sight;

    /* Cold: for the UI, never touched by the simulation. Last, so it cannot
     * push the movement fields out of the first cache line. */
    const char *name;
} material_t;

/* HOT TABLE has 32 rows instead of 16 due to MAT_EXTENDED. Range 0xF0-0xFF is
 * split into two rows by bit 3 of the cell byte. 0xF0-F7 uses static row,
 * 0xF8-FF uses new row for POWDER physics. ORDINARY materials' rows are
 * duplicated for consistency. */
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

/* For callers with an ordinary id, not a decoded cell byte. Avoids using
 * CELL_MATERIAL(cell) which discards gunpowder info. Use material_of() for
 * derived materials. Passing MAT_EXTENDED reads as static row, matching old
 * id 15 behaviour. */
static inline const material_t *material_by_id(material_id_t m)
{
    return &materials[MATERIAL_ROW(m)];
}

/* Material fire behaviour - read by sand_reactions.c. Separate table to avoid
 * cache line overflow. Not used by movement code, so minimising stride cost
 * is crucial. Adding a material may require two rows, deemed acceptable. */
typedef struct {
    /* Chance in 256 per adjacent burning cell per step for material to catch
     * fire. 0 means never burns, 255 means catches instantly, no RNG cost
     * (try_ignite()), preserving gas and fire test/device timing. */
    uint8_t flammability;

    /* Material ID when catching: MAT_FIRE for gas, other fuels may char,
     * stay, and burn. MAT_EMPTY treated as MAT_FIRE. */
    uint8_t ignites_to;

    /* Zero (default) for materials except gunpowder. Nonzero is BLAST RADIUS
     * in cells. In step_one_burning_cell(), if a lit cell's `burn_decay`
     * reaches `lit_from`, find_lit_two_by_two() checks if it's a 2x2 fully
     * lit corner. If impulses are enabled, it fires sand_explode() at the
     * radius. Otherwise, it becomes MAT_FIRE. QUENCH (`!= 0`) applies to
     * gunpowder. SMOTHER (`== 0`) skips smothered() as gunpowder
     * self-oxidizes. See SAND_GUNPOWDER_BLAST_RADIUS for radius details. */
    uint8_t explodes;

    /* Nonzero: catches with air contact (one empty neighbor). Zero: catches
     * on fire reach, suitable for solids. Burns liquid fuel pools from the
     * surface, avoiding instant detonation. Interior cells don't qualify;
     * only exposed edges do. No gravity vector needed (see
     * sand_step_reactions()). */
    uint8_t needs_air;

    /* Nonzero: Material acts as heat source. `sand_step_reactions()` manages
     * decay, quench, smother, and ignite neighbors. Replaces
     * `CELL_MATERIAL(c) != MAT_FIRE` check. Uses material, not kind: fire is
     * KIND_GAS, future burning solid could be KIND_STATIC, both shared with
     * non-burning materials. */
    uint8_t burns;

    /* BURNING AS A STATE. Non-zero `burns` means material burns while VARIANT
     * is non-zero, counted down by this chance in 256 per step like `decay`.
     * Wood uses it, replacing ember which differed in reactions. Wood uses
     * shade for burn progress, `burns` indicates always a heat source,
     * VARIANT indicates when. */
    uint8_t burn_decay;

    /* `burn_decay` tracks burn progress. Wood: 0=unlit, >0=lit, starts at 1.
     * Gunpowder: 0-2=dry, 3-6=moist, 7=lit. Unused materials ignore this
     * field. */
    uint8_t lit_from;

    /* Heat crosses ONE cell with chance 1/256 per step per burning neighbour
     * in conduct_heat() (sand_reactions.c). Chance decreases exponentially
     * with crossing depth d: (conducts/256)^d. Meaningless for materials not
     * between fire and something to heat; left at zero except for heat
     * conductor material. */
    uint8_t conducts;

    /* 1/256 chance per step that conduct_heat() boils liquid into steam,
     * despite previous heat conduction. 0 means immune to boiling; nonzero
     * values allow boiling. Water uses a low value to sometimes outpace
     * evaporation, while acid uses 255 for instant boiling. */
    uint8_t boils;

    /* `boils` roll converts this liquid into a material_id_t, using same
     * idiom as `quench_to`. 0 means MAT_STEAM, keeping original for water.
     * Acid overrides to MAT_GAS due to conducted-heat boiling hardcoded to
     * steam. */
    uint8_t boils_to;

    /* 1/256 chance a burnt-out cell leaves MAT_SMOKE. MAT_SMOKE and MAT_STEAM
     * are near-identical in materials[] but distinct for screen display.
     * Steam is water; smoke is fuel. See sand_reactions.c. */
    uint8_t residue;

    /* What this material becomes when a liquid touches it - a
     * material_id_t, narrowed. 0 means it simply vanishes, which is what
     * every burning material did before steam existed. */
    uint8_t quench_to;

    /* Chance in 256 for this material to emit a MAT_FIRE cell into an
     * adjacent empty cell (try_flare(), sand_reactions.c). Ember
     * (KIND_STATIC) and lava (KIND_LIQUID) activate it. Free-falling cells
     * skip this check to save computation. */
    uint8_t flare;

    /* 1/256 chance per step for a cell to dissolve one of its four cardinal
     * neighbours. Acid is the only cause. Paired with `dissolvable`: acid's
     * strength and target material's vulnerability. Both must be nonzero for
     * effect. Allows stone tank to hold acid while sand inside dissolves. */
    uint8_t dissolves;

    /* Chance in 256 of dissolving THIS material. 0 means immune. Default 0
     * protects existing materials. Opt-in required for dissolvable materials.
     * Alternative default would cause acid to dissolve everything. */
    uint8_t dissolvable;

    /* Chance in 256 for dissolving cell to leave MAT_SMOKE; separate from
     * `residue` to avoid field overload. Smoke represents generic
     * destruction, not steam or acid fumes. */
    uint8_t fizz;

    /* Chance in 256, per step, that a cell of this material spontaneously
     * turns into a single MAT_GAS cell - unconditional, no heat or
     * neighbour required, unlike boiling. 0, the default, means never;
     * only acid sets it. */
    uint8_t evaporates;

    /* Inverse of evaporating: 1/256 chance per step, a 2x2 square condenses
     * into a single `condenses_to` cell at top-left, others cleared. 0 means
     * never. Only steam activates. Not a real model; just cosmetic. See
     * step_one_condensing_cell() in sand_reactions.c. */
    uint8_t condenses;

    /* What a successful `condenses` roll above produces - a
     * material_id_t, narrowed, the same relationship `quench_to` has with
     * its own trigger field. Meaningless while `condenses` is 0. */
    uint8_t condenses_to;

    /* HEAT transforms this material (MAT_GLASS) with a 256 per step chance
     * per adjacent heat source. Separate fields for this, not
     * `flammability`/`ignites_to`, as sand does not catch fire. `mobility`
     * and `sight` have similar issues. Activated by direct contact or through
     * conductors. */
    uint8_t heats_to;
    uint8_t heat_chance;

    /* MELTS: 1/256 chance per step to become `heats_to` near lava or another
     * heat source. Glass handles fire via ramp. Extended material melts only
     * in lava. Heat through conductors doesn't count. */
    uint8_t melts;

    /* Rolled off `heat_chance`. Chance in 256 for a bone-dry cell to convert
     * to `flaw_to` instead of `heats_to`. 0 means no flaw. `MAT_STONE`
     * indicates impure smelts. `flaw_chance` does not trigger independently
     * per cell; see `try_heat_transform()` for details. */
    uint8_t flaw_to;
    uint8_t flaw_chance;

    /* Rolled off successful heat_chance roll on wet cells, spoiling to
     * `spoils_to` with 1/256 chance. 0 means no spoilage. Independent of
     * `flaw_to`. Always rolled on first successful heat_chance. Earlier
     * attempt to delay spoilage by moisture check was removed due to ambient
     * drying issues. */
    uint8_t spoils_to;
    uint8_t spoils_chance;

    /* Heat builds in a material's nibble, rising with `heat_ramp` chance to
     * `heats_to`. `heat_chance` alone doesn't show long exposure. The
     * nibble's heat is visible as color. `cools` is the chance to lose heat
     * without gaining, setting exposure duration and melt resistance. */
    uint8_t heat_ramp;
    uint8_t cools;

    /* COLD: `chills` and `cools` reduce heat by one level (chance in 256).
     * `chills` draws from neighbors, `cools` from self, creating thermal
     * shock. Snow's `chills` (40) and glass's `cools` (6) ensure this effect.
     * COLD avoids a flawed "heat-water" design without needing a temperature
     * scale. */
    uint8_t chills;

    /* CONVECTION: Hot gas warms neighbors with a 1 in 256 chance per step.
     * Not 'burns'; warms snow to melt. Visible heating in vessels with modest
     * rates, higher than carriers. */
    uint8_t warms;

    /* Chance in 256 per adjacent LIQUID cell for material to become
     * `heats_to`. Snow melts in water or near flames (120 in 256 steps). Snow
     * on ponds stays, allowing snowfall. Snow floats, representing warmth.
     * Oil or acid not melting snow needs explanation. */
    uint8_t thaws;

    /* SOAKING UP A LIQUID, chance 1/256 per step, per adjacent liquid cell.
     * Consumes liquid. Cell becomes `soaks_to` material at moisture 1 or
     * variant rises if zero. `soaks` property indicates if liquid soaks in.
     * Water soaks, oil, lava, acid do not. Wetness ≠ fluidity; liquid
     * determines soak. */
    uint8_t wets;

    uint8_t soaks;
    uint8_t soaks_to;

    /* MOISTURE CODEC: `tones` holds DRY codes (0..tones-1); moisture
     * (1..moist_max) uses codes tones..tones+moist_max-1.
     * SOIL_DRY_TONES/SOIL_MOISTURE_MAX (8/7) align with
     * CELL_MOISTURE()/CELL_WITH_MOISTURE(). New `dries != 0` materials need
     * separate entries. Gunpowder uses 3 dry tones, 4 moisture levels
     * (GUNPOWDER_REACTION), limited to 3 bits (GUNPOWDER_BASE). */
    uint8_t tones;
    uint8_t moist_max;

    /* WHETHER A PLANT MAY TREAT THIS AS SOIL - distinct from `dries`.
     * Gunpowder has `dries != 0` but is not ground. Sites check this for soil
     * properties instead of `dries`. Moisture diffusion and percolation
     * within same species use `dries`. Only dirt sets this. */
    uint8_t soil;

    /* Chance of losing one `soaks` level per step: 1 in 256. Marks material's
     * moisture variant. Zero means bone dry, so `random_cell()` must
     * initialise correctly. */
    uint8_t dries;

    /* Zero materials except gunpowder. When fully saturated, cells turn to
     * this with `soaked_chance` in 256 per step, checked after `held`.
     * Gunpowder turns to oil: one grain plus water becomes one oil cell,
     * ignoring mass. */
    uint8_t soaked_to;
    uint8_t soaked_chance;

    /* 256 chance per step for material to grow by one cell if touching moist
     * soil, spending one level of soil's moisture. Grows against gravity from
     * column top. No per-cell state; identity in low nibble. Growth from tip
     * creates tree, not shrub. */
    uint8_t grows;

    /* In the cold pass, with a 1/256 chance per step, objects with
     * KIND_POWDER (like ICE) fall into empty cells. This distinguishes seeds
     * from stems: seeds drop until they land, while stems remain upright,
     * each cell supporting the next. */
    uint8_t falls;

    /* WITHERING: Chance/256 per step for a cell to die if no water or
     * neighbor. Growth adds cells, while fire and acid remove them. Fragments
     * like limbs or seeds were permanent. "Or wood" keeps foliage on living
     * trees. */
    uint8_t withers;

    /* `hardens_to` when `harden_run` cells align along gravity, making tall
     * stems trunks. Creeper spread sideways remains soft. Burns as wood
     * STATE, tree can catch fire and rain halfway, leaving soft growth alive. */
    uint8_t hardens_to;
    uint8_t harden_run;

    /* 1/256 chance a run hardens, delaying wood formation. Lower values make
     * trees greener and shaggier, higher values woodier and squatter.
     * Measured over eight trees in sand_reactions.c. */
    uint8_t harden_chance;

    /* PART OF: other material in a tree. `hardens_to` now only for hardening.
     * Read by walks for cell anchoring, stem continuation, water distance,
     * and trunk thickness. */
    uint8_t clings_to;

    /* ROOTING: Two readings per field. `hardens_to`, `clings_to` elsewhere.
     * GROWER: 1/256 chance, SPENDING soil moisture welds CONTACT to
     * `roots_to`, disconnects tree as dirt shifts. ONE-TIME SEED:
     * (`root_depth == 0`) plants first root, then grows below. ON ROOT: 1/256
     * chance converts moist dirt to root, costing variant, if <
     * ROOT_SURFACE_MAX neighbours. */
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
 * suite_sand_roots.c, extended to pin the new boundary. */
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
 * (suite_sand_roots.c), which pins that equivalence. The dirt-only macros stay,
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
 * test_a_diagonal_neighbour_alone_is_not_an_edge in suite_sand_foam.c, which
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

/* Called once per frame, before painting - not really a clock at all,
 * unlike the three above: app_sand.c hands this a plain SNAPSHOT of
 * gravity's current bearing (gravity_bearing_q16() there, shifted down
 * by GLASS_PHASE_SHIFT), not a value accumulated over time. */

/* Signed, unlike foam's and cullet's phase - this one names a direction,
 * not a rate, and holding any tilt steady leaves it exactly where it is. */
void material_set_glass_phase(int phase);
