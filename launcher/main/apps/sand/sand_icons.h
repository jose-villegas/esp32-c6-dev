/*=============================================================================
 * sand_icons - the brush screen's own artwork: POUR, ERASE, BOOM and INFO.
 *
 * Lives in the app's own folder, not gfx/icons.h, because an app is a folder
 * - deleting apps/sand/ must delete its icons with it (CLAUDE.md's
 * "adding/removing an app touches no other file"). gfx/icons.h's own header
 * comment argues for hand-drawn bitmaps over computed shapes and for
 * splitting geometry from drawing; this module is those bitmaps, reusing
 * icons.h's icon_bitmap_blocks()/ICON_BITMAP_SIZE rather than repeating
 * either argument. See suite_sand_icons.c for the structural facts a bitmap
 * like this can actually be tested against.
 *===========================================================================*/
#pragma once

#include <stdint.h>

#include "gfx/icons.h"

/* A funnel/hopper: a filled triangle tapering from a wide rim to a narrow
 * spout, with three drops trailing below it. The drops step right-left-right
 * rather than falling straight down, which is what reads as motion rather
 * than a static tail - deliberately NOT left-right symmetric, unlike the
 * funnel body above it. */
static const uint16_t icon_pour_bitmap[ICON_BITMAP_SIZE] = {
    0b0111111111111110,
    0b0011111111111100,
    0b0001111111111000,
    0b0000111111110000,
    0b0000011111100000,
    0b0000001111000000,
    0b0000000110000000,
    0b0000000110000000,
    0b0000000110000000,
    0b0000000110000000,
    0b0000000000000000,
    0b0000000100000000,
    0b0000000000000000,
    0b0000000010000000,
    0b0000000000000000,
    0b0000000100000000,
};

/* A bold diagonal cross, both strokes 2px wide except the three centre rows
 * where they overlap into one 4px band. Symmetric on every axis - vertical,
 * horizontal and both diagonals - the same "read as a stroke, not a
 * staircase" reasoning icons.h gives for icon_check_bitmap. */
static const uint16_t icon_erase_bitmap[ICON_BITMAP_SIZE] = {
    0b1100000000000011,
    0b0110000000000110,
    0b0011000000001100,
    0b0001100000011000,
    0b0000110000110000,
    0b0000011001100000,
    0b0000001111000000,
    0b0000001111000000,
    0b0000001111000000,
    0b0000001111000000,
    0b0000011001100000,
    0b0000110000110000,
    0b0001100000011000,
    0b0011000000001100,
    0b0110000000000110,
    0b1100000000000011,
};

/* A four-pointed sparkle: one connected body whose sides curve inward, so
 * the four arms taper to a spike instead of reading as a plus sign.
 * Symmetric on both axes.
 *
 * Deliberately not a starburst with separate diagonal rays. A ray drawn one
 * pixel wide survives only at scale 1: every scale above it turns each pixel
 * into a detached block, and the glyph reads as scattered dots around a
 * blob rather than as anything radiating. Everything here is contiguous at
 * any integer scale. */
static const uint16_t icon_boom_bitmap[ICON_BITMAP_SIZE] = {
    0b0000000110000000,
    0b0000000110000000,
    0b0000000110000000,
    0b0000001111000000,
    0b0000011111100000,
    0b0000111111110000,
    0b0011111111111100,
    0b1111111111111111,
    0b1111111111111111,
    0b0011111111111100,
    0b0000111111110000,
    0b0000011111100000,
    0b0000001111000000,
    0b0000000110000000,
    0b0000000110000000,
    0b0000000110000000,
};

/* A lowercase i: a square dot over a taller stem, one blank row of air
 * between them so they read as two shapes rather than one blob. Symmetric
 * left-right only - a dot centred over its own stem, not top-bottom, which
 * is what makes it read as a letter rather than a burst. */
static const uint16_t icon_info_bitmap[ICON_BITMAP_SIZE] = {
    0b0000000000000000,
    0b0000000000000000,
    0b0000001111000000,
    0b0000001111000000,
    0b0000001111000000,
    0b0000000000000000,
    0b0000001111000000,
    0b0000001111000000,
    0b0000001111000000,
    0b0000001111000000,
    0b0000001111000000,
    0b0000001111000000,
    0b0000001111000000,
    0b0000001111000000,
    0b0000001111000000,
    0b0000000000000000,
};
