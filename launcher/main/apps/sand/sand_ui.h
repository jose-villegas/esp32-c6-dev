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
 *
 * WHO HIT-TESTS AND WHO DECIDES
 *
 * Which tile a tap landed on used to be this module's own job too, via
 * palette_hit() on raw screen coordinates - and that split (drawn as
 * microui commands, hit-tested by hand) was the one thing standing between
 * this panel and rotation, since a transform would move the drawing without
 * moving where palette_hit() looked. Hit-testing now belongs to microui
 * itself: app_sand.c's draw_palette() lays each tile out as a real
 * mu_button(), and the caller tells this module the RESULT - which tile, if
 * any, microui says was clicked - through sand_ui_tile_clicked() below.
 * What a click on that tile MEANS - select it, or toggle its mode - is
 * still exactly this module's call, for the same testability reason
 * everything else here is: see suite_sand_ui.c.
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

/* PAINT / ERASE / DETONATE - PWR cycles it, independently of `brush` and of
 * brush_mode_t above. This is a DIFFERENT axis from brush_mode_t: that one
 * says how the SELECTED MATERIAL gets applied (poured, or left as a
 * standing source); this one says what the finger does at all, and a
 * material is only one of its three answers.
 *
 * DETONATE is TEMPORARY EVALUATION SCAFFOLDING for
 * docs/Sand/Explosion-Plan.md - a way to fire sand_explode() with a finger
 * before any material or trigger owns it, so the mechanic can be judged on
 * its own. It rides on the same cycle as ERASE rather than sitting in
 * brush_mode_t or brushes[] because it is not a material: nothing paints an
 * explosion, so it has no cell to remember and no tile of its own in the
 * palette. It can be deleted outright the day the plan's questions are
 * answered, without touching sand.c/sand.h at all - see
 * app_sand.c's DETONATE branch of handle_pour_input(). */
typedef enum { SAND_MODE_PAINT, SAND_MODE_ERASE, SAND_MODE_DETONATE } sand_mode_t;
#define SAND_MODE_COUNT 3

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
    int         brush;       /* index into brushes[]/modes[] */
    sand_mode_t mode;        /* PAINT/ERASE/DETONATE - PWR cycles it */

    /* Set by open_palette() when a finger is already down as the panel
     * opens. Cleared by sand_ui_step() itself, the first SAND_UI_PALETTE
     * frame it sees input->down go false - a genuine lift, not any
     * particular click - rather than by whichever click happens to arrive
     * next: a click is no longer something this module ever sees directly
     * (see this file's own top comment on "WHO HIT-TESTS AND WHO DECIDES"),
     * so the guard can no longer key off consuming one. While armed,
     * sand_ui_tile_clicked() ignores whatever tile it is told was clicked.
     * See both functions' own comments in sand_ui.c for what this guards
     * against. */
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
 * this file's own top comment.
 *
 * No longer hit-tests a palette tap itself - see "WHO HIT-TESTS AND WHO
 * DECIDES" above. While SAND_UI_PALETTE, this reads only input->boot.released
 * (closes the panel) and input->down (disarms `swallow_release` on a genuine
 * lift); selecting or toggling a tile happens through sand_ui_tile_clicked()
 * below instead, called by the caller once microui says which tile, if any,
 * was actually clicked this frame. */
unsigned sand_ui_step(sand_ui_t *ui, const input_t *input);

/* What a click on palette tile `index` means, once the caller's own hit-test
 * - a real mu_button() per tile now, see draw_palette() in app_sand.c - has
 * already decided a click landed on it. Applies exactly the rule this used
 * to apply itself while reading a raw touch release:
 *
 *   - the already-selected tile toggles its mode between BRUSH_POUR and
 *     BRUSH_SPAWN, but only when material_can_emit() says the brush is
 *     eligible to be a source at all - an ineligible tile has no mode to
 *     toggle into, so this does nothing rather than flip a bit nothing
 *     ever reads;
 *   - any OTHER tile is selected instead: `mode` resets to SAND_MODE_PAINT
 *     (choosing a material means you want to place it, not erase or blow
 *     up whatever is already there), and that tile's own remembered
 *     BRUSH_POUR/BRUSH_SPAWN mode is left exactly as it was;
 *   - and while `swallow_release` is armed, this does nothing at all and
 *     returns 0 - see that field's own comment on sand_ui_t.
 *
 * `index` is caller-guaranteed to be a real tile: draw_palette() only ever
 * calls this from inside its own per-tile loop, at that tile's own index, so
 * there is no "index hits nothing" case left for this function to handle -
 * that former case is dispositioned entirely by microui's hit-test not
 * calling this at all, the same way an unclicked mu_button() never runs the
 * caller's `if` body. */
unsigned sand_ui_tile_clicked(sand_ui_t *ui, int index);
