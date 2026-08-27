/*=============================================================================
 * sand_ui - the falling-sand app's UI state machine: which button/touch
 * edges move the app between RUNNING and the material picker overlay, and
 * what a tap on the panel itself does.
 *
 * Pure decision logic, no gfx and no touch/IMU driver behind it - not even
 * gfx.h, which drags in bsp/esp-bsp.h - so it links on the host and can be
 * tested there, the same reasoning palette.h gives for keeping its own grid
 * arithmetic hardware-free. See suite_sand_ui.c.
 *
 * WHY THIS MODULE EXISTS
 *
 * Four bugs have shipped to hardware out of this exact logic, all the same
 * shape: an input edge consumed by the state that should not own it.
 *
 *   - a BOOT hold also cycled the brush, because cycling sat on `.pressed`
 *   - closing the palette on `.pressed` let the matching release advance the
 *     brush afterwards (commit faad9bb)
 *   - a touch release from a pour in progress selected a palette tile
 *   - the swallow-release guard armed unconditionally, so it ate the
 *     player's first real tap (commit eef97e4)
 *
 * Every one is a pure state-machine bug - a sequence of edges producing the
 * wrong state - and every one reached the device because this logic used to
 * live in app_sand.c, the one file in the app the host test runner cannot
 * compile (see run_tests.sh's SOURCES comment on the app_*.c convention).
 * Moving it here is what lets suite_sand_ui.c pin all four down for good.
 *
 * WHAT STAYS BEHIND
 *
 * Nothing here draws anything or touches the IMU. sand_ui_step() returns a
 * bitmask of what the caller should do - open the panel, close it, redraw
 * it, show the mode label - and app_sand.c is the one that calls gfx_*,
 * reads gravity, and runs the simulation. See sand_ui_step()'s own comment
 * in sand_ui.c.
 *===========================================================================*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../../app.h"
#include "material.h"

/* Which brush mode: pour, or a persistent source - toggled by tapping the
 * already-selected tile in the palette (see handle_palette_input() in
 * sand_ui.c) and read by app_sand.c's handle_pour_input(). */
typedef enum { BRUSH_POUR, BRUSH_SPAWN } brush_mode_t;

/* Which screen the sand app is showing. SAND_UI_MENU is never acted on by
 * sand_ui_step() - the boot menu is microui-driven and stays entirely in
 * app_sand.c - but it lives in this enum anyway so `screen` has one
 * definition rather than two. */
typedef enum { SAND_UI_MENU, SAND_UI_RUNNING, SAND_UI_PALETTE } sand_ui_screen_t;

/* What the caller must do after a step - sand_ui_step() decides, app_sand.c
 * carries it out. More than one bit can be set on the same call: closing the
 * palette can also want the label shown. */
typedef enum {
    SAND_UI_OPEN_PALETTE   = 1u << 0,   /* pause the sim, read gravity, draw_palette() */
    SAND_UI_CLOSE_PALETTE  = 1u << 1,   /* resume the sim, force a full repaint */
    SAND_UI_REDRAW_PALETTE = 1u << 2,   /* selection or mode changed - redraw the panel */
    SAND_UI_SHOW_LABEL     = 1u << 3,   /* arm the mode-label countdown */
} sand_ui_action_t;

typedef struct {
    /* Caller-owned, exactly as sand_t borrows `cells` - this module never
     * sees app_sand.c's tables, only points at them. */
    const cell_t *brushes;      /* brush_count cells, indexed by `brush` */
    uint8_t      *modes;        /* brush_count entries, BRUSH_POUR/BRUSH_SPAWN */
    int           brush_count;

    sand_ui_screen_t screen;
    int      brush;          /* index into brushes[]/modes[] */
    bool     erasing;        /* PWR toggles it, independently of the brush */

    /* Set by open_palette(), consumed by the next input->released
     * handle_palette_input() sees - see sand_ui.c's own comment on what it
     * is guarding against. */
    bool     swallow_release;

    /* The brush and its mode at the moment the panel opened - recorded on
     * open, compared against the current brush/mode on close, so the mode
     * label on the way out confirms a choice only when the choice actually
     * changed while the panel was open. */
    int      opened_brush;
    uint8_t  opened_mode;
} sand_ui_t;

/* One frame's input. Returns a bitmask of sand_ui_action_t for the caller to
 * carry out; this module itself draws nothing and touches no hardware - see
 * this file's own top comment. */
unsigned sand_ui_step(sand_ui_t *ui, const input_t *input);
