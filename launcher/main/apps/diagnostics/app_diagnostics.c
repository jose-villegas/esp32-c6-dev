/*=============================================================================
 * app_diagnostics - shows the POST report on demand, plus a second page of
 * developer-only toggles.
 *
 * The POST itself is silent when everything passes, which is the right default
 * for a device that should just boot. This is how you look anyway, without
 * attaching a serial cable - a board in the field usually has nothing on its
 * console.
 *
 * Entering it RE-RUNS the checks, so the report is live rather than a record of
 * what boot found - the point of opening it is usually to see whether something
 * is failing now.
 *
 * Every check is repeated, including the SD card. That one is interesting: the
 * card and the display share SPI2 on different pins, so the re-run releases the
 * panel, mounts the card, and takes the bus back. Nothing is visible on screen
 * while that happens - the panel goes on refreshing its last frame from its own
 * GRAM - and the report prints how long the round trip took.
 *
 * BOOT pages between the report and the toggle screen, rather than the
 * toggle screen adding a control to the report itself - the report is
 * already a dense list with no obvious room, and a second page costs
 * nothing the report's own layout has to account for.
 *
 * The toggle screen is built with microui like any other app UI (see
 * ui_launcher.c) - a real checkbox, tappable, drawn and dirty-tracked the
 * same way the launcher's own menu is - rather than hand-rolling a
 * one-off control that reads a button directly. Any future developer
 * toggle belongs on this same page as another mu_checkbox() row, not as
 * its own bespoke screen.
 *===========================================================================*/

#include "../../app.h"
#include "../../gfx.h"
#include "../../post_ui.h"
#include "../../ui.h"

#define PAGE_COUNT     2
#define COL_BACKGROUND 0x0A0C14

static int page;

static void diagnostics_enter(void)
{
    /* Always open on the report - the page you came here for by default,
     * and the same screen every time regardless of where a previous visit
     * left off. */
    page = 0;

    /* Re-run on entry rather than per frame: the checks probe I2C and cycle the
     * audio rail, which is fine once but has no business happening 40 times a
     * second. */
    post_rerun();
}

static void draw_toggles_page(const input_t *input)
{
    mu_Context *ctx = ui_context();
    ui_begin(input);

    if (mu_begin_window_ex(ctx, "Developer Toggles",
                           mu_rect(0, 0, GFX_WIDTH, GFX_HEIGHT),
                           MU_OPT_NOTITLE | MU_OPT_NORESIZE |
                           MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {

        mu_layout_row(ctx, 1, (int[]){ -1 }, gfx_text_height() + 8);
        mu_text(ctx, "DEVELOPER TOGGLES");

        mu_layout_row(ctx, 1, (int[]){ -1 }, UI_ROW_HEIGHT);
        int overlay_on = gfx_debug_overlay();
        mu_checkbox(ctx, "gfx dirty-region overlay", &overlay_on);
        gfx_set_debug_overlay(overlay_on);

        mu_layout_row(ctx, 1, (int[]){ -1 }, UI_ROW_HEIGHT);
        int leaf_on = gfx_debug_leaf_overlay();
        mu_checkbox(ctx, "gfx leaf-grid overlay", &leaf_on);
        gfx_set_leaf_overlay(leaf_on);

        mu_layout_row(ctx, 1, (int[]){ -1 }, gfx_text_height() + 8);
        mu_text(ctx, "BOOT for the POST report");

        mu_end_window(ctx);
    }

    ui_end(COL_BACKGROUND);
}

static void diagnostics_frame(uint32_t dt_ms, const input_t *input)
{
    (void)dt_ms;

    if (input->boot.pressed) {
        page = (page + 1) % PAGE_COUNT;
        if (page == 1) {
            /* Switching in from the report page, drawn entirely outside
             * microui - ui_end() compares this page's own command list
             * against its last paint and would otherwise see no change
             * and skip repainting, leaving the report's pixels on screen
             * underneath. See ui_invalidate()'s own comment. */
            ui_invalidate();
        }
    }

    /* Redrawn every frame rather than cached: the shell owns the framebuffer
     * and the previous app may have left anything in it. */
    if (page == 0) {
        post_ui_draw_report("POWER-ON SELF TEST");
    } else {
        draw_toggles_page(input);
    }
}

static void diagnostics_exit(void) { }

const app_t app_diagnostics = {
    .name    = "Diagnostics",
    .summary = "Hardware self-test report",
    .enter   = diagnostics_enter,
    .frame   = diagnostics_frame,
    .exit    = diagnostics_exit,
};

APP_REGISTER(app_diagnostics);
