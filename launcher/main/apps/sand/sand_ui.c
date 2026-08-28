#include "sand_ui.h"

/* Opens the palette panel: transitions to SAND_UI_PALETTE and records
 * everything the close needs to compare against - see close_palette()'s own
 * comment. The gravity read and draw_palette() call that used to happen
 * here now happen in app_sand.c; SAND_UI_OPEN_PALETTE is the signal to do
 * them - see sand_ui.h's own top comment on "what stays behind".
 *
 * Arms `swallow_release`, but ONLY when a finger is actually on the screen
 * as the panel opens.
 *
 * Opening the panel is a press of the physical BOOT button, which has
 * nothing to do with whatever a finger already down on the touchscreen is
 * doing - so a pour in progress when BOOT is released is a touch left
 * dangling, and its own release still has to land somewhere. Without
 * swallowing it, that release arrives on the very next frame with `screen`
 * already SAND_UI_PALETTE, and whatever tile the finger happened to lift
 * over is what a click would resolve to - silently changing the brush the
 * player never meant to touch. (Today, feed_input() in ui.c only ever
 * synthesises a click from a fresh input->pressed, so a bare release like
 * this one does not actually reach microui as a click at all - but that is
 * an emergent property of ui.c's touch synthesis, a file with no tests, and
 * not something this guard gets to lean on. See sand_ui_tile_clicked()'s
 * own comment for where the guard actually bites now.)
 *
 * Arming it unconditionally was wrong, and cost the common case to protect
 * the rare one: with no finger down there is no dangling release to eat, so
 * the flag ate the player's first deliberate tap on a tile instead and the
 * panel only started responding on the second (commit eef97e4).
 * `touch_in_progress` is simply input->down at the moment of opening -
 * swallow a release only when there is genuinely one already owed. */
static unsigned open_palette(sand_ui_t *ui, bool touch_in_progress)
{
    ui->screen = SAND_UI_PALETTE;
    ui->swallow_release = touch_in_progress;
    ui->opened_brush = ui->brush;
    ui->opened_mode  = ui->modes[ui->brush];
    return SAND_UI_OPEN_PALETTE;
}

/* Closes the palette panel. The forced repaint of the sand underneath it,
 * and the accumulator resets that keep the pause from cashing in as a burst
 * of catch-up steps the instant it closes, are app_sand.c's job - see
 * SAND_UI_CLOSE_PALETTE.
 *
 * Asks for the mode label - via SAND_UI_SHOW_LABEL - only if the brush or
 * its mode actually changed while the panel was open (see
 * opened_brush/opened_mode's own comment on sand_ui_t). Since cycling no
 * longer gives its own confirmation on the way past each material, this is
 * the only feedback closing the panel gets, and it should say nothing when
 * there is nothing to confirm. */
static unsigned close_palette(sand_ui_t *ui)
{
    unsigned actions = SAND_UI_CLOSE_PALETTE;

    if (ui->brush != ui->opened_brush ||
        ui->modes[ui->brush] != ui->opened_mode) {
        actions |= SAND_UI_SHOW_LABEL;
    }

    ui->screen = SAND_UI_RUNNING;
    return actions;
}

/* Only reachable while SAND_UI_PALETTE - see sand_ui_step() below.
 *
 * Closes on boot.released only - the mirror image of where the panel opens
 * in SAND_UI_RUNNING (see sand_ui_step()'s own comment on that site). It
 * cannot close on the SAME edge that opened it: edges are read-and-cleared
 * once per frame by the caller (buttons_read()) before this function ever
 * runs for the first time, so the .released that opened the panel is
 * already gone by the time a SAND_UI_PALETTE frame gets to see one of its
 * own.
 *
 * boot.held closes nothing here - see the comment on the `if` just below
 * for why that is deliberate and what it means for anyone who holds BOOT
 * instead of tapping it.
 *
 * Selecting or toggling a tile is NOT decided here any more, and neither is
 * reading a touch at all - see sand_ui.h's own "WHO HIT-TESTS AND WHO
 * DECIDES" comment. What is left, once BOOT is out of the way, is
 * `swallow_release`'s own bookkeeping: disarm it the first frame a finger
 * that was already down when the panel opened is genuinely lifted
 * (input->down goes false), so the very next tap - the first one that can
 * possibly reach sand_ui_tile_clicked() - is read as a real selection
 * rather than eaten too. See that function's own comment for the other half
 * of this guard: while armed, it is the one that actually ignores a click. */
static unsigned handle_palette_input(sand_ui_t *ui, const input_t *input)
{
    /* On the RELEASE, never on the press. Closing on boot.pressed split a
     * single physical press across two screens: the panel went away on the
     * press edge, and the matching release edge arrived several frames
     * later with screen already back to SAND_UI_RUNNING, where
     * handle_brush_input() consumed it and cycled the brush forward one -
     * commit faad9bb, the bug this still guards against even though
     * cycling itself is gone. Acting on the release keeps both edges of a
     * press inside the state that started it.
     *
     * input->boot.held is deliberately NOT handled here - the panel has
     * exactly one way to close, a plain tap of BOOT, mirroring the one way
     * in. Worth writing down because it looks like an omission otherwise:
     * button_fsm suppresses the .released of a press that turns into a
     * .held (see button_fsm.h's contract), so a user who HOLDS BOOT instead
     * of tapping it gets no edge this function acts on at all while the
     * panel is open. Holding does not close the panel; it does nothing,
     * silently, by design - not a missed case, and not a bug to go chasing
     * if someone reports it. */
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

/* What a click on palette tile `index` means - see this function's own doc
 * comment in sand_ui.h for the full contract this applies, and the "WHO
 * HIT-TESTS AND WHO DECIDES" note there for why the hit-test that produces
 * `index` is no longer this module's job. draw_palette() in app_sand.c is
 * the only caller, from inside its own per-tile mu_button() loop - which is
 * also why there is no "index hits nothing" branch here the way the old
 * palette_hit()-based version had one: an index this function is ever
 * handed already named a real tile. */
unsigned sand_ui_tile_clicked(sand_ui_t *ui, int index)
{
    /* Swallow the first click after the panel opens with a finger already
     * down - see `swallow_release`'s own comment on sand_ui_t, and
     * handle_palette_input()'s own comment for the other half of this
     * guard (disarming it on the finger's actual lift). This is the same
     * family of bug faad9bb fixed for BOOT: an edge that outlives the state
     * that produced it, read by whatever state happens to be current
     * instead of the one it actually belongs to. */
    if (ui->swallow_release) {
        return 0;
    }

    if (index == ui->brush) {
        /* Tapping the ALREADY-selected tile toggles its mode instead of
         * re-selecting it - selection state has nothing left to change, so
         * a second tap on the same tile has to mean something else. Only
         * if the material is eligible to be a source at all: an
         * ineligible tile has no mode to toggle into, so this does
         * nothing at all rather than silently flip a bit nothing ever
         * reads (see material_can_emit()). */
        if (!material_can_emit(ui->brushes[ui->brush])) {
            return 0;
        }
        ui->modes[ui->brush] = (ui->modes[ui->brush] == BRUSH_POUR)
                                    ? BRUSH_SPAWN : BRUSH_POUR;
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

/* BOOT is not read here at all - its release edge opens the palette from
 * sand_ui_step() itself, before this function is ever called for that
 * frame, and selecting a material happens in handle_palette_input() rather
 * than here. What is left is PWR: a plain press cycles PAINT -> ERASE ->
 * DETONATE -> PAINT, because none of the three needs a hold's
 * BUTTON_HOLD_US delay - a dedicated button's press is the right cost for a
 * control pressed this often. See sand_mode_t's own comment in sand_ui.h
 * for why DETONATE rides along on this same cycle rather than getting a
 * control of its own, and app_sand.c's comment above `brushes[]` for the
 * fuller reasoning on why BOOT and PWR ended up with the jobs they have. */
static unsigned handle_brush_input(sand_ui_t *ui, const input_t *input)
{
    if (input->power.pressed) {
        ui->mode = (sand_mode_t)((ui->mode + 1) % SAND_MODE_COUNT);
        return SAND_UI_SHOW_LABEL;
    }
    return 0;
}

/* One frame's worth of input, dispatched by screen.
 *
 * input->boot.released opens the panel from here, on the release edge
 * rather than the press - the mirror image of handle_palette_input()'s own
 * close-on-released, and for the identical reason (see that function's
 * comment, and commit faad9bb, the bug both of these guard against).
 * Opening on .pressed would leave the matching .released to arrive one
 * frame later with `screen` already SAND_UI_PALETTE, where it could resolve
 * to a click on whatever tile happens to be under the finger instead - see
 * `swallow_release`'s own comment on sand_ui_t for the guard that exists
 * because of exactly this. Acting on .released keeps both edges of the
 * press that opened the panel inside SAND_UI_RUNNING, with nothing left
 * outstanding by the time SAND_UI_PALETTE takes over.
 *
 * input->boot.held is deliberately NOT handled here, or anywhere else in
 * this module - the panel has exactly one way in, a plain tap. Worth
 * writing down because it looks like an omission otherwise: button_fsm
 * suppresses the .released of a press that turns into a .held (see
 * button_fsm.h's contract), so a user who HOLDS BOOT instead of tapping it
 * gets no edge this module acts on at all for that press. Holding does not
 * open the panel; it does nothing, silently, by design - not a missed
 * case. `held` itself is still produced and plumbed all the way through
 * button_fsm and buttons_read(); this module simply has no consumer for it
 * right now. */
unsigned sand_ui_step(sand_ui_t *ui, const input_t *input)
{
    if (ui->screen == SAND_UI_MENU) {
        return 0;
    }

    if (ui->screen == SAND_UI_PALETTE) {
        return handle_palette_input(ui, input);
    }

    if (input->boot.released) {
        return open_palette(ui, input->down);
    }

    return handle_brush_input(ui, input);
}
