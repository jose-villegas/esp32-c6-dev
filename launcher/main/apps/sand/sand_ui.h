/*
 * sand_ui - the falling-sand app's UI state machine: which button/touch
 * edges move the app between RUNNING and its two overlay panels - the
 * material palette (SAND_UI_PALETTE, opened by BOOT) and the brush screen
 * (SAND_UI_BRUSH, opened by PWR) - and what a tap on either panel does.
 *
 * Pure decision logic, no gfx and no touch/IMU driver behind it - not even
 * gfx.h, which drags in bsp/esp-bsp.h - so it links on the host and can be
 * tested there, the same reasoning palette.h gives for keeping its own grid
 * arithmetic hardware-free. See suite_sand_ui.c.
 *
 * WHY THIS MODULE EXISTS
 *
 * Every bug this logic has shipped to hardware has the same shape: an input
 * edge consumed by the state that should not own it (a BOOT hold also
 * cycling the brush, a palette close whose matching release advanced the
 * brush, a pour's release selecting a palette tile). This logic must not
 * live in app_sand.c, the one file in the app the host test runner cannot
 * compile (see run_tests.sh's SOURCES comment on the app_*.c convention) -
 * every one of those bugs shipped while it did. Living here is what lets
 * suite_sand_ui.c pin bugs of this shape down for good.
 *
 * WHAT STAYS BEHIND
 *
 * Nothing here draws anything or touches the IMU. sand_ui_step() returns a
 * bitmask of what the caller should do - open the panel, close it, redraw
 * it, show the mode label - and app_sand.c is the one that calls gfx_*,
 * reads gravity, and runs the simulation. See sand_ui_step()'s own comment
 * in sand_ui.c.
 *
 * WHO HIT-TESTS AND WHO DECIDES
 *
 * Hit-testing belongs to microui, not this module: app_sand.c's
 * draw_palette() lays each tile out as a real mu_button(), and the caller
 * tells this module the RESULT - which tile, if any, microui says was
 * clicked - through sand_ui_tile_clicked() below. Not hit-tested by hand
 * against raw screen coordinates: a transform moves the drawing without
 * moving what a hand-rolled hit test looks at, so that would silently break
 * under UI rotation. What a click on that tile MEANS - select it, or toggle
 * its mode - is still exactly this module's call, for the same testability
 * reason everything else here is: see suite_sand_ui.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../../app.h"
#include "material.h"

/* Which brush mode: pour, or a persistent source - toggled by tapping the
 * already-selected tile in the palette (see handle_palette_input() in
 * sand_ui.c) and read by app_sand.c's handle_pour_input(). */
typedef enum { BRUSH_POUR, BRUSH_SPAWN } brush_mode_t;

/* PAINT / ERASE / BOOM - which of the three brush modes is active,
 * independent of brush_mode_t above: that says how the SELECTED MATERIAL is
 * applied, this says what the finger does at all. Chosen from the brush
 * screen's segmented control (SAND_UI_BRUSH), one of three first-class
 * modes, each with its own remembered radius - see sand_ui_t.radius_px and
 * sand_ui_mode_clicked(). */
typedef enum { SAND_MODE_PAINT, SAND_MODE_ERASE, SAND_MODE_DETONATE } sand_mode_t;
#define SAND_MODE_COUNT 3

/* Which screen the sand app is showing. SAND_UI_MENU is never acted on by
 * sand_ui_step() - the boot menu is microui-driven and stays entirely in
 * app_sand.c - but it lives in this enum anyway so `screen` has one
 * definition rather than two. SAND_UI_BRUSH and SAND_UI_PALETTE are
 * siblings, opened by PWR and BOOT respectively - never both at once. */
typedef enum { SAND_UI_MENU, SAND_UI_RUNNING, SAND_UI_PALETTE, SAND_UI_BRUSH } sand_ui_screen_t;

/* What the caller must do after a step - sand_ui_step() decides, app_sand.c
 * carries it out. More than one bit can be set on the same call: closing the
 * palette can also want the label shown. */
typedef enum {
    SAND_UI_OPEN_PALETTE   = 1u << 0,   /* pause the sim, read gravity, draw_palette() */
    SAND_UI_CLOSE_PALETTE  = 1u << 1,   /* resume the sim, force a full repaint */
    SAND_UI_REDRAW_PALETTE = 1u << 2,   /* selection or mode changed - redraw the panel */
    SAND_UI_SHOW_LABEL     = 1u << 3,   /* arm the mode-label countdown */
    SAND_UI_OPEN_BRUSH     = 1u << 4,   /* draw_brush_screen() (Phase 5c) */
    SAND_UI_CLOSE_BRUSH    = 1u << 5,   /* force a full repaint */
    SAND_UI_REDRAW_BRUSH   = 1u << 6,   /* mode or radius changed - redraw the panel */
} sand_ui_action_t;

/* Brush radius bounds in px, shared by every mode's slider - see
 * sand_ui_set_radius(). Min keeps a radius from rounding away to nothing at
 * VERY LOW quality's 8px cells (app_sand.c's coarsest); max sits well above
 * DETONATE's 50px default while still leaving most of the 368px-wide canvas
 * free of a single tap's circle. */
#define SAND_UI_RADIUS_MIN 2
#define SAND_UI_RADIUS_MAX 64

typedef struct {
    /* Caller-owned, exactly as sand_t borrows `cells` - this module never
     * sees app_sand.c's tables, only points at them. */
    const cell_t *brushes;      /* brush_count cells, indexed by `brush` */
    uint8_t      *modes;        /* brush_count entries, BRUSH_POUR/BRUSH_SPAWN */
    int           brush_count;

    sand_ui_screen_t screen;
    int         brush;       /* index into brushes[]/modes[] */
    sand_mode_t mode;        /* PAINT/ERASE/DETONATE - set from the brush screen */

    /* Per-mode brush radius in px, indexed by sand_mode_t, clamped to
     * [SAND_UI_RADIUS_MIN, SAND_UI_RADIUS_MAX] by sand_ui_set_radius().
     * Owned here rather than borrowed like brushes/modes above, since
     * nothing outside this module needs to share it - app_sand.c only
     * seeds the three defaults at startup. */
    uint8_t  radius_px[SAND_MODE_COUNT];

    /* Set by open_palette()/open_brush() when a finger is already down as
     * the panel opens. Cleared on the first frame input->down goes false -
     * a genuine lift, not any particular click, since a click is no longer
     * seen directly here (see "WHO HIT-TESTS AND WHO DECIDES" above). While
     * armed, sand_ui_tile_clicked() and sand_ui_mode_clicked() ignore
     * whatever they are told was clicked. */
    bool     swallow_release;

    /* The brush and its mode at the moment the palette opened - recorded on
     * open, compared against the current brush/mode on close, so the mode
     * label on the way out confirms a choice only when the choice actually
     * changed while the panel was open. */
    int      opened_brush;
    uint8_t  opened_mode;

    /* The SAND_UI_BRUSH counterpart to opened_brush/opened_mode above:
     * mode and its radius at the moment the brush screen opened, compared
     * on close for the same reason. */
    sand_mode_t opened_sand_mode;
    uint8_t     opened_radius;
} sand_ui_t;

/* One frame's input. Returns a sand_ui_action_t bitmask; this module draws
 * nothing and touches no hardware, and does not hit-test a tap itself -
 * see "WHO HIT-TESTS AND WHO DECIDES" above. PWR opens SAND_UI_BRUSH from
 * RUNNING and closes it from BRUSH; BOOT and PWR never open both panels
 * at once. */
unsigned sand_ui_step(sand_ui_t *ui, const input_t *input);

/* What a click on palette tile `index` means, once the caller's hit-test
 * (a real mu_button() per tile) decided a click landed on it. The
 * already-selected tile toggles BRUSH_POUR/BRUSH_SPAWN, only when
 * material_can_emit() says it's eligible - ineligible does nothing
 * rather than flip a bit nothing reads. Any OTHER tile is selected
 * instead: `mode` resets to SAND_MODE_PAINT, its remembered pour/spawn
 * mode left as-is. While `swallow_release` is armed, returns 0. `index`
 * is caller-guaranteed real. */
unsigned sand_ui_tile_clicked(sand_ui_t *ui, int index);

/* What a tap on brush-mode segment `index` means, once the caller's
 * hit-test (a real mu_button() per segment) decided a tap landed on it -
 * same "caller hit-tests, this module decides" split as
 * sand_ui_tile_clicked() above. `index` lines up with sand_mode_t
 * directly (see brush_screen_segment_t's own comment on the shared
 * order). Selects that mode; the already-selected segment is a no-op.
 * While `swallow_release` is armed, returns 0. `index` is
 * caller-guaranteed real. */
unsigned sand_ui_mode_clicked(sand_ui_t *ui, int index);

/* Sets the CURRENT mode's brush radius, clamped to
 * [SAND_UI_RADIUS_MIN, SAND_UI_RADIUS_MAX]. Every other mode's remembered
 * radius is untouched - see sand_ui_t.radius_px. */
void sand_ui_set_radius(sand_ui_t *ui, uint8_t radius_px);

/* The current mode's brush radius. */
uint8_t sand_ui_radius(const sand_ui_t *ui);
