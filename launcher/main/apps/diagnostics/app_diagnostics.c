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

#include <stdio.h>

#include "../../app.h"
#include "../../display/display.h"
#include "../../gfx/gfx.h"
#include "../../input/imu.h"
#include "../../boot/post_ui.h"
#include "../../boot/selftest.h"
#include "../../ui/ui.h"

#define PAGE_COUNT     2
#define COL_BACKGROUND 0x0A0C14

static int page;

/* Persisted like `page` above - a developer toggle that resets to off every
 * visit would defeat the point of leaving the board on this screen while
 * physically turning it through its holds to read the numbers off. */
static int show_orientation;

/* Last selftest_run() result, persisted across frames like show_orientation
 * above rather than reset in diagnostics_enter(): re-running the checks on
 * every visit already happens via post_rerun() for the (cheap) POST report,
 * but the self test suite is a separate, heavier action the user explicitly
 * asks for by tapping the button below - it must not silently re-run just
 * because the page was revisited. -1 means "never tapped yet", so it reads
 * differently from a run that tapped and found zero failures. */
static int selftest_failures = -1;

/* Set by a tap of the button below; consumed at the top of
 * diagnostics_frame(), never run from inside mu_button()'s own if-block -
 * see the comment there for why. */
static bool selftest_pending;

/* Sensor axes to screen axes - the SAME board-layout fact main.c's
 * DISPLAY_GRAVITY_X/Y and app_sand.c's GRAVITY_SCREEN_X/Y already carry,
 * copied rather than shared for the same reason main.c's own copy gives:
 * these are small, independent readers of a sensor that only ever answers
 * "which way is down", and forcing a shared one is a separate piece of
 * work this toggle does not need. This copy exists so the numbers shown
 * here are the exact gx/gy display_update() actually decides orientation
 * from - not a plausible-looking approximation of them. */
#define ORIENTATION_GRAVITY_X(s)  (-(s)->ay)
#define ORIENTATION_GRAVITY_Y(s)  ( (s)->ax)

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

    /* ui_width()/ui_height(), not GFX_WIDTH/GFX_HEIGHT - see ui.h: the
     * logical canvas swaps dimensions under a quarter-turn transform. */
    if (ui_begin_screen(ctx, "Developer Toggles",
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

        mu_layout_row(ctx, 1, (int[]){ -1 }, UI_ROW_HEIGHT);
        mu_checkbox(ctx, "show orientation", &show_orientation);

        /* Read once per frame, only while the toggle is on - imu_read() is
         * an I2C transaction, and there is no reason to pay for it on
         * every frame of a page most visits never enable this on. Shows
         * three things on purpose, not just the quarter: the raw
         * accelerometer counts (what the sensor actually measured), the
         * derived gx/gy (what display_update() actually decides from -
         * see ORIENTATION_GRAVITY_X/Y above), and the shell's current
         * quarter (what that decision came out to) - so a hold can be
         * pinned down as "this physical orientation reads these numbers
         * and the shell currently calls it quarter N" in one line,
         * without doing the ORIENTATION_GRAVITY_X/Y arithmetic by hand. */
        if (show_orientation) {
            char line[64];
            mu_layout_row(ctx, 1, (int[]){ -1 }, gfx_text_height() + 4);

            if (imu_ready()) {
                imu_sample_t sample;
                if (imu_read(&sample)) {
                    snprintf(line, sizeof line, "accel ax=%d ay=%d az=%d",
                             sample.ax, sample.ay, sample.az);
                    mu_text(ctx, line);
                    mu_layout_row(ctx, 1, (int[]){ -1 }, gfx_text_height() + 4);
                    snprintf(line, sizeof line, "gravity gx=%d gy=%d",
                             ORIENTATION_GRAVITY_X(&sample),
                             ORIENTATION_GRAVITY_Y(&sample));
                    mu_text(ctx, line);
                } else {
                    mu_text(ctx, "IMU read failed");
                }
            } else {
                mu_text(ctx, "no IMU");
            }

            mu_layout_row(ctx, 1, (int[]){ -1 }, gfx_text_height() + 4);
            snprintf(line, sizeof line, "shell quarter=%d",
                     display_shell_quarter());
            mu_text(ctx, line);
        }

        /* An ACTION, not a persistent toggle like the checkboxes above -
         * this runs once when tapped rather than reflecting a state the
         * page tracks continuously. Only FLAGGED here, not run: selftest_run()
         * itself happens at the top of diagnostics_frame(), outside this
         * page's own ui_begin()/ui_end() bracket - see the comment there for
         * why it cannot run from inside this if-block. */
        mu_layout_row(ctx, 1, (int[]){ -1 }, UI_ROW_HEIGHT);
        if (mu_button(ctx, "run self test suite")) {
            selftest_pending = true;
        }

        mu_layout_row(ctx, 1, (int[]){ -1 }, gfx_text_height() + 4);
        char selftest_line[48];
        if (selftest_failures < 0) {
            snprintf(selftest_line, sizeof selftest_line, "self test: not run yet");
        } else if (selftest_failures == 0) {
            snprintf(selftest_line, sizeof selftest_line, "self test: all passed");
        } else {
            snprintf(selftest_line, sizeof selftest_line, "self test: %d failure(s)",
                     selftest_failures);
        }
        mu_text(ctx, selftest_line);

        mu_layout_row(ctx, 1, (int[]){ -1 }, gfx_text_height() + 8);
        mu_text(ctx, "BOOT for the POST report");

        mu_end_window(ctx);
    }

    ui_end(COL_BACKGROUND);
}

static void diagnostics_frame(uint32_t dt_ms, const input_t *input)
{
    (void)dt_ms;

    /* Consumed here, before this frame's own ui_begin()/ui_end() bracket
     * opens - never from inside mu_button()'s own if-block in
     * draw_toggles_page(). selftest_run() runs suite_ui.c, a device-only
     * suite whose fixture() calls ui_init() once per test - i.e. mu_init()
     * on the exact same ui_context() singleton every window in this shell
     * draws through, this toggle page included - and whose tests then set
     * ui_set_transform() to a fixed sequence of quarter-turns that end on
     * whatever the LAST test happened to leave it at, not the board's real
     * physical orientation. Calling selftest_run() synchronously from
     * mu_button()'s own if-block used to do exactly that in the middle of
     * this very frame's ui_begin()/ui_end() bracket: the suite's own
     * mu_init() would reset the container pool and canvas hashes the
     * mu_end_window()/ui_end() calls further down were still relying on,
     * and ui_set_transform() would leave the shell's touch-to-logical
     * mapping wrong until the board was physically turned enough to trigger
     * main.c's own periodic resync - which, held steady, might never
     * happen, reading exactly like "the screen stopped responding to taps".
     * Running it here instead, before any mu_* call this frame, means the
     * suite's own ui_init() calls land on a context nothing is mid-use of,
     * and the explicit ui_set_transform() right after immediately restores
     * the board's actual orientation before draw_toggles_page() ever opens
     * its own frame. One frame of latency between the tap landing and the
     * suite actually starting - the same deferral gfx_resume()'s "one full
     * frame after a resume" already uses, and imperceptible next to the
     * suite's own run time. */
    if (selftest_pending) {
        selftest_pending = false;
        selftest_failures = selftest_run();
        ui_set_transform(ui_transform_quarter_turn(
            display_shell_quarter(), GFX_WIDTH, GFX_HEIGHT));
    }

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
    .name         = "Diagnostics",
    .summary      = "Hardware self-test report",
    .enter        = diagnostics_enter,
    .frame        = diagnostics_frame,
    .exit         = diagnostics_exit,
    .home_gesture = true,
};

APP_REGISTER(app_diagnostics);
