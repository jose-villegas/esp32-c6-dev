/*=============================================================================
 * material_palette - what a material LOOKS like, split from what it IS.
 *
 * material.h/material.c hold materials[]/reactions[] - identity and
 * behaviour, read by the whole simulation (and by tools/dump_reactions.c on
 * the host), none of which cares about a single pixel. This half is
 * everything downstream of that: the 256-entry colour table, the per-cell
 * grain/speckle lookups built from it, and material_colours() - the one
 * function the renderer calls per cell, per row, every frame. Splitting it
 * out keeps material.h/.c the small, purely-data pair the rest of the app
 * already treats them as, and makes the dependency on gfx_color_t visible
 * at the include line instead of buried inside a file nothing else there
 * needed it for.
 *===========================================================================*/
#pragma once

#include "gfx/gfx_color.h"
#include "material.h"

static inline unsigned material_grain_hash(int cx, int cy)
{
    unsigned h = (unsigned)cx * 0x9E3779B9u ^ (unsigned)cy * 0x85EBCA6Bu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

const gfx_color_t *material_palette(void);

/* FLAT stays free. Only patterned materials pay for it. */
typedef enum {
    MATERIAL_FLAT = 0,      /* one colour, whole block */
    MATERIAL_SPECKLED,      /* one colour per cell, varied by POSITION */
    MATERIAL_HATCHED,       /* diagonals both ways, bright where they cross */
} material_pattern_t;

#define MATERIAL_EDGE_LEFT       (1u << 0)
#define MATERIAL_EDGE_RIGHT      (1u << 1)
#define MATERIAL_EDGE_UP         (1u << 2)
#define MATERIAL_EDGE_DOWN       (1u << 3)
#define MATERIAL_EDGE_UP_LEFT    (1u << 4)
#define MATERIAL_EDGE_UP_RIGHT   (1u << 5)
#define MATERIAL_EDGE_DOWN_LEFT  (1u << 6)
#define MATERIAL_EDGE_DOWN_RIGHT (1u << 7)

#define MATERIAL_EDGE_CARDINAL                                           \
    (MATERIAL_EDGE_LEFT | MATERIAL_EDGE_RIGHT | MATERIAL_EDGE_UP |       \
     MATERIAL_EDGE_DOWN)

#define MATERIAL_EDGE_MASK_COUNT (MATERIAL_EDGE_CARDINAL + 1u)




/* Test `(mask & MATERIAL_EDGE_CARDINAL) != 0` for "is this cell an edge at
 * all", never `mask != 0`: once the diagonal bits exist, a cell with every
 * cardinal neighbour occupied but one diagonal empty would newly read as an
 * edge, and glass/stone would start outlining cells they used to paint as
 * solid interior. */



/* LOCAL DEPTH, 0 at boundary, 255 max. */

/* paint_row_n() for full mechanism; replaced old screen-position gradient. */

/* INTERIOR reads `depth`; others ignore it. */

/* See DEPTH_SATURATE_CELLS in material_palette.c for scale. */


/* FOR ROOT CELL, `depth` MEANS material_root_neighbours() */

/* READ shape: tip touches 1, cell 2-3, collar more */

/* Shading: parent darkens when child grows, lightens if child lost. */

material_pattern_t material_colours(cell_t c, unsigned hash, unsigned mask,
                                    unsigned depth, gfx_color_t out[3]);

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

/* See material_palette.c. `depth` is local to each puddle. Specular table
 * depends on gravity's direction. */
void material_set_gravity(int gx, int gy);

/* Tracks board, slanting shine. Pure, stateless. Returns (1,1) for degenerate
 * input. */
void material_shine_direction(int gx, int gy, int *ux_q8, int *uy_q8);

/* Separate gravity setter. Phase is time, gravity direction. Test
 * independence. See material_colours() for foam. */
void material_set_foam_phase(unsigned phase);

/* Separate clock for cullet phase. See material_colours(), MAT_SAND case. */
void material_set_cullet_phase(unsigned phase);

/* Not a clock; app_sand.c provides gravity snapshot. */

/* Names direction, not rate; steady tilt stays fixed. */
void material_set_glass_phase(int phase);
