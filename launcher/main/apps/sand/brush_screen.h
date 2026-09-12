/*
 * brush_screen - pure layout for the sand app's second screen: header
 * (material swatch/name/info), brush mode (POUR/ERASE/BOOM segments) and
 * brush size (caption/value/slider).
 *
 * Same split palette.h uses, for the same reason: a canvas width and height
 * go in, rects come out, nothing here calls gfx or touches hardware, which is
 * what makes the geometry host-testable (see suite_brush_screen.c) instead of
 * only judgeable by eye on the device. Drawing, and the state machine that
 * decides what the segments and slider DO, live elsewhere (app_sand.c and
 * sand_ui.c) - this module only says where things go.
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
 */
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
    mu_Rect material_caption; /* "MATERIAL" */
    mu_Rect material_name;    /* the large material name */
    mu_Rect info_button;

    /* Brush mode: caption above three equal-width, equal-gap segments. */
    mu_Rect mode_caption; /* "BRUSH MODE" */
    mu_Rect segments[BRUSH_SCREEN_SEGMENT_COUNT];

    /* Brush size: caption and right-aligned value share a row, above the
     * slider track. */
    mu_Rect size_caption; /* e.g. "POUR BRUSH SIZE" */
    mu_Rect size_value;   /* e.g. "06 PX" */
    mu_Rect slider_track;
} brush_screen_layout_t;

/* The scale every caption and segment label on this screen is drawn at.
 * It lives here, beside the rects those strings have to fit inside, because
 * the two only mean anything together: suite_brush_screen.c measures the
 * strings below against their own rects at this scale, which is what caught
 * "POUR BRUSH SIZE" overflowing its row in portrait. */
#define BRUSH_SCREEN_CAPTION_SCALE    2

/* The screen's fixed strings. Here rather than at the drawing call site so
 * the fit check above can reach them - a caption the layout has never seen
 * is a caption nothing can prove fits. */
#define BRUSH_SCREEN_MATERIAL_CAPTION "MATERIAL"
#define BRUSH_SCREEN_MODE_CAPTION     "BRUSH MODE"

/* The segment's own label, and the size row's caption naming whichever mode
 * is selected - "POUR SIZE" rather than the design's "POUR BRUSH SIZE",
 * which needs 240px of a row that is only 232px wide once the value has its
 * 80. The dropped word is the one carrying no information: the panel is
 * already the brush screen and the segment above already says POUR. */
const char* brush_screen_segment_label(brush_screen_segment_t seg);
const char* brush_screen_size_caption(brush_screen_segment_t seg);

/* The screen's own chrome palette - dark navy panels, a gold accent for the
 * selected mode segment. Unlike the material palette (app_sand.c's
 * draw_palette()), none of these derive from a material's own colour. Here
 * rather than at the drawing call site for the same reason the strings
 * above are: anything that draws this screen, or previews it off-device,
 * must agree on these exact values rather than each keeping its own copy
 * that can drift. */
#define BRUSH_PANEL_FACE_COLOR       0x131C2E
#define BRUSH_PANEL_BORDER_COLOR     0xE8ECF4
#define BRUSH_SEG_SELECTED_COLOR     0xE0A63C
#define BRUSH_SEG_UNSELECTED_COLOR   0x1B2740
#define BRUSH_CAPTION_COLOR          0x8FA3C0
#define BRUSH_TEXT_COLOR             0xF2F6FF
#define BRUSH_SEG_SELECTED_INK_COLOR 0x2A1A06

/* Fills `out` for a `screen_w` x `screen_h` canvas - the LOGICAL canvas
 * (ui_width()/ui_height()), which swap under a quarter turn, exactly as
 * palette_tile_rect() takes them. Never reads GFX_WIDTH/GFX_HEIGHT. */
void brush_screen_layout(int screen_w, int screen_h, brush_screen_layout_t* out);
