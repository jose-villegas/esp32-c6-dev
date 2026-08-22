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
 * water is in that cell. For a transient material - fire, steam - it will be
 * LIFE REMAINING, counting down to nothing.
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
