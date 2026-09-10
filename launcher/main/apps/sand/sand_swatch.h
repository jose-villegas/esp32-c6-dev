/*=============================================================================
 * sand_swatch - which shade paints which cell of the brush screen's material
 * swatch.
 *
 * Pure, header-only (the same precedent ui_slider.h/gfx/icon.h set): a brush
 * cell spec and a (col, row) in an N x N grid go in, the exact cell byte to
 * paint there comes out - drawn straight from material_palette(), the same
 * table the grid itself renders with, rather than a second, drifting idea
 * of what a material looks like.
 *
 * MUST STAY DETERMINISTIC ACROSS FRAMES
 *
 * ui_end() skips a repaint when microui's command list hashes the same as
 * last frame (see ui.c's own top comment). A swatch that reshuffled itself
 * every frame would emit different colours every frame, defeat that hash
 * unconditionally, and repaint this otherwise-static panel every frame for
 * nothing. The variant returned here is therefore a pure function of
 * (spec, col, row) alone - never sand_t's RNG, never a frame or call
 * counter. Do not "liven it up" with anything that changes without the
 * brush or the grid size changing.
 *===========================================================================*/
#pragma once

#include "material.h"
#include "material_palette.h"

/* The cell to paint at (col, row) of an N x N swatch for brush cell `spec`.
 *
 * Gunpowder and the MATX() materials spend their low nibble on STATE, not a
 * shade (material.h's top comment), so there is no variant axis to sweep -
 * this returns brush_color()'s (app_sand.c) own representative cell instead,
 * a flat swatch rather than texture built from bytes that mean something
 * else. */
static inline cell_t sand_swatch_cell(cell_t spec, int col, int row, int cells)
{
    if (cell_is_gunpowder(spec)) {
        return GUNPOWDER_CELL(2);
    }
    if (cell_is_extended(spec)) {
        return spec;
    }

    const material_id_t material = (material_id_t)CELL_MATERIAL(spec);
    const int span = MATERIAL_SHADE_SPAN(material);
    if (span <= 1 || cells <= 0) {
        return CELL_MAKE(material, 0);
    }

    const uint8_t variant = (uint8_t)(material_grain_hash(col, row) % (unsigned)span);
    return CELL_MAKE(material, variant);
}
