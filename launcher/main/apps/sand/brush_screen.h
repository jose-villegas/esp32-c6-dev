/*=============================================================================
 * brush_screen - pure layout for the sand app's second screen: header
 * (material swatch/name/info), brush mode (POUR/ERASE/BOOM segments) and
 * brush size (caption/value/slider).
 *
 * Same split palette.h uses, for the same reason: a canvas width and height
 * go in, rects come out, nothing here calls gfx or touches hardware, which is
 * what makes the geometry host-testable (see suite_brush_screen.c) instead of
 * only judgeable by eye on the device. Phase 5b (drawing) and the state
 * machine that decides what the segments and slider DO are separate work -
 * this module only says where things go.
 *
 * WHICH "BRUSH" THIS SCREEN'S SEGMENTS DRIVE
 *
 * This app has three unrelated things called some form of "brush", and this
 * screen only concerns one of them:
 *   - brushes[] (app_sand.c) - the palette's MATERIALS (sand, water, ...).
 *   - brush_mode_t (sand_ui.h) - BRUSH_POUR/BRUSH_SPAWN, whether the
 *     selected material pours or emits from a source.
 *   - sand_mode_t (sand_ui.h) - SAND_MODE_PAINT/ERASE/DETONATE, what the
 *     finger does at all. THIS is the one the POUR/ERASE/BOOM segments below
 *     select - not brush_mode_t, despite the similar name.
 * None of the three are renamed here; this header only exists to say so
 * before the next reader has to work it out from three similarly-named
 * enums at once.
 *===========================================================================*/
#pragma once

#include "microui.h"

/* The three brush-mode segments, in the order they are laid out left to
 * right - matching sand_mode_t's own PAINT/ERASE/DETONATE order (see this
 * file's top comment), labelled POUR/ERASE/BOOM by the design. A local
 * enum rather than reusing sand_mode_t so this pure-layout header stays
 * decoupled from sand_ui.h's state machine, per this phase's scope. */
typedef enum {
    BRUSH_SCREEN_SEG_POUR = 0,
    BRUSH_SCREEN_SEG_ERASE,
    BRUSH_SCREEN_SEG_BOOM,
    BRUSH_SCREEN_SEGMENT_COUNT,
} brush_screen_segment_t;

/* Every rect the drawing code (Phase 5b) needs, and nothing else. Filled by
 * brush_screen_layout() below; no field is meaningful before that call. */
typedef struct {
    mu_Rect header_panel;
    mu_Rect mode_panel;
    mu_Rect size_panel;

    /* Header: swatch on the left, caption above name to its right, info
     * button at the far right edge. */
    mu_Rect swatch;
    mu_Rect material_caption;   /* "MATERIAL" */
    mu_Rect material_name;      /* the large material name */
    mu_Rect info_button;

    /* Brush mode: caption above three equal-width, equal-gap segments. */
    mu_Rect mode_caption;       /* "BRUSH MODE" */
    mu_Rect segments[BRUSH_SCREEN_SEGMENT_COUNT];

    /* Brush size: caption and right-aligned value share a row, above the
     * slider track. */
    mu_Rect size_caption;       /* e.g. "POUR BRUSH SIZE" */
    mu_Rect size_value;         /* e.g. "06 PX" */
    mu_Rect slider_track;
} brush_screen_layout_t;

/* Fills `out` for a `screen_w` x `screen_h` canvas - the LOGICAL canvas
 * (ui_width()/ui_height()), which swap under a quarter turn, exactly as
 * palette_tile_rect() takes them. Never reads GFX_WIDTH/GFX_HEIGHT. */
void brush_screen_layout(int screen_w, int screen_h, brush_screen_layout_t *out);
