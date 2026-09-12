#include "sand_ui.h"

/* Opens the palette panel: transitions to SAND_UI_PALETTE and records
 * everything close_palette() needs to compare against. Arms
 * `swallow_release`, but ONLY when a finger is on the screen as the
 * panel opens: opening is a BOOT press, unrelated to a finger already
 * down, so a pour in progress when BOOT is released leaves a touch
 * dangling - its release arrives next frame with `screen` already
 * SAND_UI_PALETTE, resolving to a click on whatever tile the finger
 * happens to lift over. */
static unsigned
open_palette(sand_ui_t* ui, bool touch_in_progress) {
    ui->screen = SAND_UI_PALETTE;
    /* Arming it unconditionally was wrong, and cost the common case to
     * protect the rare one: with no finger down there is no dangling
     * release to eat, so the flag ate the player's first deliberate tap
     * on a tile instead and the panel only started responding on the
     * second (commit eef97e4). `touch_in_progress` is simply input->down
     * at the moment of opening - swallow a release only when there is
     * genuinely one already owed. */
    ui->swallow_release = touch_in_progress;
    ui->opened_brush = ui->brush;
    ui->opened_mode = ui->modes[ui->brush];
    return SAND_UI_OPEN_PALETTE;
}

/* Closes the palette panel. The forced repaint of the sand underneath it,
 * and the accumulator resets that keep the pause from cashing in as a
 * burst of catch-up steps, are app_sand.c's job - see
 * SAND_UI_CLOSE_PALETTE. Asks for the mode label via SAND_UI_SHOW_LABEL
 * only if the brush or its mode actually changed while open - the only
 * feedback closing gets, so it says nothing when there is nothing to
 * confirm. */
static unsigned
close_palette(sand_ui_t* ui) {
    unsigned actions = SAND_UI_CLOSE_PALETTE;

    if (ui->brush != ui->opened_brush || ui->modes[ui->brush] != ui->opened_mode) {
        actions |= SAND_UI_SHOW_LABEL;
    }

    ui->screen = SAND_UI_RUNNING;
    return actions;
}

/* Only reachable while SAND_UI_PALETTE. Closes on boot.released only,
 * mirroring where the panel opens in SAND_UI_RUNNING: it cannot close on
 * the SAME edge that opened it, since edges are read-and-cleared once
 * per frame before this function ever runs for the first time. Selecting
 * or toggling a tile is NOT decided here - see sand_ui.h's "WHO
 * HIT-TESTS AND WHO DECIDES". What is left is swallow_release's own
 * bookkeeping: disarm it the first frame a finger already down when
 * opened is lifted. */
static unsigned
handle_palette_input(sand_ui_t* ui, const input_t* input) {
    /* On the RELEASE, never on the press. Closing on boot.pressed split a
     * single physical press across two screens: the panel closed on the
     * press edge, and the matching release arrived a frame later with
     * screen back to SAND_UI_RUNNING, where handle_brush_input() consumed
     * it and cycled the brush - commit faad9bb, still guarded against
     * though cycling is gone. input->boot.held is deliberately NOT
     * handled: button_fsm suppresses the .released of a press turned
     * .held, so holding does nothing here. */
    if (input->boot.released) {
        return close_palette(ui);
    }

    /* The dangling touch's own lift, not any particular click - see this
     * function's own top comment. Checked every SAND_UI_PALETTE frame,
     * not just once, because the finger can take more than one frame to
     * actually come up. */
    if (ui->swallow_release && !input->down) {
        ui->swallow_release = false;
    }

    return 0;
}

/* What a click on palette tile `index` means - see this function's own
 * doc comment in sand_ui.h for the full contract, and the "WHO
 * HIT-TESTS AND WHO DECIDES" note there for why the hit-test producing
 * `index` belongs to microui, not this module. draw_palette() in
 * app_sand.c is the only caller, from inside its own per-tile mu_button()
 * loop, so there is no "index hits nothing" branch here: an index this
 * function is ever handed already named a real tile. */
unsigned
sand_ui_tile_clicked(sand_ui_t* ui, int index) {
    /* Swallow the first click after the panel opens with a finger already
     * down - see `swallow_release`'s own comment on sand_ui_t, and
     * handle_palette_input()'s own comment for the other half of this
     * guard (disarming it on the finger's actual lift). This is the same
     * family of bug faad9bb fixed for BOOT: an edge that outlives the
     * state that produced it, read by whatever state happens to be
     * current instead of the one it actually belongs to. */
    if (ui->swallow_release) {
        return 0;
    }

    if (index == ui->brush) {
        /* Tapping the ALREADY-selected tile toggles its mode instead of
         * re-selecting it - selection state has nothing left to change,
         * so a second tap has to mean something else. Only if the
         * material is eligible to be a source at all: an ineligible tile
         * has no mode to toggle into, so this does nothing rather than
         * silently flip a bit nothing ever reads (see
         * material_can_emit()). */
        if (!material_can_emit(ui->brushes[ui->brush])) {
            return 0;
        }
        ui->modes[ui->brush] = (ui->modes[ui->brush] == BRUSH_POUR) ? BRUSH_SPAWN : BRUSH_POUR;
    } else {
        /* A different tile: select it. Its own remembered mode is left
         * exactly as it was - only `mode` resets on selection, the same as
         * always (ERASE and DETONATE alike: choosing a material means you
         * want to place it, not erase or blow up whatever is already
         * there). */
        ui->brush = index;
        ui->mode = SAND_MODE_PAINT;
    }

    return SAND_UI_REDRAW_PALETTE;
}

/* Opens the brush screen: transitions to SAND_UI_BRUSH and records what
 * close_brush() needs to compare against. Same swallow_release discipline
 * as open_palette() and for the same reason - a finger already on the
 * glass must not resolve into a segment tap - even though PWR itself has
 * no dangling release of its own to worry about (buttons.h). */
static unsigned
open_brush(sand_ui_t* ui, bool touch_in_progress) {
    ui->screen = SAND_UI_BRUSH;
    ui->swallow_release = touch_in_progress;
    ui->opened_sand_mode = ui->mode;
    ui->opened_radius = ui->radius_px[ui->mode];
    return SAND_UI_OPEN_BRUSH;
}

/* Closes the brush screen, the SAND_UI_BRUSH counterpart to
 * close_palette() - same reasoning: SAND_UI_SHOW_LABEL fires only if the
 * mode or its radius actually changed while open. */
static unsigned
close_brush(sand_ui_t* ui) {
    unsigned actions = SAND_UI_CLOSE_BRUSH;

    if (ui->mode != ui->opened_sand_mode || ui->radius_px[ui->mode] != ui->opened_radius) {
        actions |= SAND_UI_SHOW_LABEL;
    }

    ui->screen = SAND_UI_RUNNING;
    return actions;
}

/* Only reachable while SAND_UI_BRUSH. PWR has no release edge (buttons.h),
 * so open and close both read `.pressed` - safe because sand_ui_step()
 * dispatches on the screen value from the START of the frame, so the
 * press that opened SAND_UI_BRUSH this call can never also reach this
 * check in the same call. BOOT is deliberately not read - it belongs to
 * the palette. Segment/slider taps are decided elsewhere; what is left
 * is swallow_release's own bookkeeping, mirroring
 * handle_palette_input(). */
static unsigned
handle_brush_screen_input(sand_ui_t* ui, const input_t* input) {
    if (input->power.pressed) {
        return close_brush(ui);
    }

    if (ui->swallow_release && !input->down) {
        ui->swallow_release = false;
    }

    return 0;
}

/* What a tap on brush-mode segment `index` means - see this function's own
 * doc comment in sand_ui.h for the full contract, and "WHO HIT-TESTS AND
 * WHO DECIDES" for why the hit-test producing `index` is not this
 * module's job. `index` lines up with sand_mode_t directly - see
 * brush_screen_segment_t's own comment on the ordering the two share. */
unsigned
sand_ui_mode_clicked(sand_ui_t* ui, int index) {
    if (ui->swallow_release) {
        return 0;
    }

    if ((sand_mode_t)index == ui->mode) {
        return 0;
    }

    ui->mode = (sand_mode_t)index;
    return SAND_UI_REDRAW_BRUSH;
}

/* Clamps to [SAND_UI_RADIUS_MIN, SAND_UI_RADIUS_MAX] and stores into the
 * CURRENT mode's slot only - see sand_ui_t.radius_px. */
void
sand_ui_set_radius(sand_ui_t* ui, uint8_t radius_px) {
    if (radius_px < SAND_UI_RADIUS_MIN) {
        radius_px = SAND_UI_RADIUS_MIN;
    } else if (radius_px > SAND_UI_RADIUS_MAX) {
        radius_px = SAND_UI_RADIUS_MAX;
    }

    ui->radius_px[ui->mode] = radius_px;
}

uint8_t
sand_ui_radius(const sand_ui_t* ui) {
    return ui->radius_px[ui->mode];
}

/* Only reachable while SAND_UI_RUNNING. BOOT opens the palette; PWR opens
 * the brush screen; both read the edge, never `.held`, for the same
 * reason open_palette()'s own call site does. */
static unsigned
handle_running_input(sand_ui_t* ui, const input_t* input) {
    if (input->boot.released) {
        return open_palette(ui, input->down);
    }

    if (input->power.pressed) {
        return open_brush(ui, input->down);
    }

    return 0;
}

/* One frame's worth of input, dispatched by screen - see this function's
 * own comment in sand_ui.h. Reading `ui->screen` exactly once, before any
 * branch can change it, is what keeps the press that opens a panel from
 * also being read by that panel's own close check in the same call. */
unsigned
sand_ui_step(sand_ui_t* ui, const input_t* input) {
    if (ui->screen == SAND_UI_MENU) {
        return 0;
    }

    if (ui->screen == SAND_UI_PALETTE) {
        return handle_palette_input(ui, input);
    }

    if (ui->screen == SAND_UI_BRUSH) {
        return handle_brush_screen_input(ui, input);
    }

    return handle_running_input(ui, input);
}
