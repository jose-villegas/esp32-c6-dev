/*=============================================================================
 * The shell: boots the device, runs the frame loop, and switches between the
 * launcher and whichever app is running.
 *
 * There is exactly one frame loop on the device. It lives here, not in the
 * apps, so that switching is instant and no app can wedge the system by
 * failing to yield.
 *
 * Note this task must never return. Once firmware goes idle on this board the
 * chip stops responding to reset signalling and can only be recovered with the
 * BOOT button - see docs/Notes/Flashing-and-Toolchain.md.
 *===========================================================================*/

#include <stdint.h>
#include <string.h>

#include "app.h"
#include "boot/boot_anim.h"
#include "display/display.h"
#include "input/gesture.h"
#include "input/imu.h"
#include "gfx/gfx.h"
#include "boot/post.h"
#include "boot/post_ui.h"
#include "input/buttons.h"
#include "input/touch.h"
#include "ui/ui.h"
#include "ui/ui_launcher.h"
#include "util/screenshot.h"

#if CONFIG_LAUNCHER_SELFTEST
#include "boot/selftest.h"
#endif

#include "bsp/esp-bsp.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "shell";

/* Leaving an app is a swipe up from the bottom edge, the same gesture the
 * board's stock firmware used. It replaced a small back button, which was fine
 * to aim at with a mouse and miserable with a fingertip. The recognition
 * itself lives in gesture.c, where it is covered by host tests.
 *
 * A thin bar near the bottom hinting the gesture exists, in the manner of a
 * phone's home indicator. Cheap, unobtrusive, and it does not steal a row of
 * the app's screen the way a title bar does. */
#define HOME_HINT_WIDTH   120
#define HOME_HINT_HEIGHT  4
#define HOME_HINT_MARGIN  10
#define HOME_HINT_RGB     0x4A5268

/* How often orientation is resampled, not every frame.
 *
 * The render loop reaches several hundred fps once partial updates are
 * doing their job, and imu_read() is an I2C transaction - real bus time, not
 * a memory read. A quarter-turn decision needs nothing like frame-rate
 * resolution, and display.h's hysteresis wants a dwell between samples
 * anyway, not a reading every 2-4 ms it would have to filter through.
 *
 * 10 Hz (100 ms) is plenty: a genuine reorientation is caught within a
 * tenth of a second, far below what a human notices as lag, and it leaves
 * the overwhelming majority of frames untouched. */
#define DISPLAY_SAMPLE_MS 100

/* --- app registry ------------------------------------------------------- */

/* Filled in before app_main() by the constructors APP_REGISTER() emits. No
 * app is named here; see app.h for why. */
static const app_t *apps[APP_MAX];
static int apps_registered;

void app_register(const app_t *app)
{
    if (apps_registered >= APP_MAX) {
        ESP_LOGE(TAG, "More than %d apps registered; '%s' was dropped",
                 APP_MAX, app->name);
        return;
    }
    apps[apps_registered++] = app;
}

const app_t *const *app_list(void) { return apps; }
int app_list_count(void) { return apps_registered; }

/* --- display orientation -------------------------------------------------
 *
 * Owned here, not by any one app - see display.h's top comment for why.
 * main.c samples gravity, feeds display_update(), and on a change pushes the
 * new quarter into ui_set_transform() so the launcher, every boot menu and
 * every app UI rotate together instead of each deciding for itself. */

/* Sensor axes to screen axes - a board-layout fact, found by experiment, not
 * derivable from the datasheet; see app_sand.c's own copy of this mapping
 * for the full explanation of which axis is which and why the negation is
 * there.
 *
 * Duplicated rather than shared: this is now the SECOND reader of the IMU,
 * alongside app_sand.c's, and unifying the two is deliberately out of scope
 * here. Two small, independent readers of a sensor that only ever answers
 * "which way is down" is a fine place to leave that, at least for now;
 * forcing them to share a single reader is a separate piece of work with its
 * own tradeoffs (whose polling rate wins? whose smoothing?) that this task
 * does not need to settle. */
#define DISPLAY_GRAVITY_X(s)  (-(s)->ay)
#define DISPLAY_GRAVITY_Y(s)  ( (s)->ax)

/* The shell's one display_t - see display.h's top comment on why the module
 * itself stays a plain struct-and-functions decision function with no
 * singleton of its own, and display_shell_quarter()'s own comment for why
 * the accessor is declared there but defined here. */
static display_t shell_display;

int display_shell_quarter(void) { return display_quarter(&shell_display); }

/* Which edge the exit gesture (and its hint bar) live on, for the shell's
 * current quarter turn.
 *
 * The rule is content-driven, not a fixed physical reference: the exit
 * gesture lives on whichever PHYSICAL edge the CONTENT's own logical bottom
 * currently maps to - the same edge a button pinned to the bottom of the
 * logical canvas would render against, tracking rotation exactly the way
 * ui_transform_rect() already makes buttons and text do (see ui.c's
 * draw_command() TEXT case). An earlier version of this function instead put
 * the exit edge opposite wherever the USB connector sits, on the reasoning
 * that a cable might occupy that edge - ergonomically plausible, but wrong
 * in practice: tested on the board, Portrait needs the exit gesture at the
 * physical BOTTOM, not the USB-opposite LEFT that rule produced.
 *
 * The table below is not hand-derived - hand-derivation is exactly how the
 * USB-opposite rule went wrong. It comes from mapping a thin strip along the
 * logical canvas's bottom edge, { 0, logical_h - 4, logical_w, 4 }, through
 * ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT) and
 * ui_transform_rect() (the same pair the exhaustive sweep already proved
 * exact) and reading off which physical edge the mapped rect landed against:
 *
 *   0  Portrait               -> exit edge BOTTOM
 *   1  Landscape               -> exit edge LEFT
 *   2  Portrait, upside down   -> exit edge TOP
 *   3  Landscape, upside down  -> exit edge RIGHT
 *
 * Each step advances the edge by one quarter turn (BOTTOM -> LEFT -> TOP ->
 * RIGHT -> BOTTOM), which is what rotating a single fixed edge through four
 * 90-degree content turns should produce, and matches quarter 0 needing no
 * correction at all - identity transform, logical bottom is physical
 * bottom. */
static gesture_edge_t exit_edge_for_quarter(int quarter)
{
    static const gesture_edge_t edge_for_quarter[4] = {
        GESTURE_EDGE_BOTTOM,  /* quarter 0: Portrait */
        GESTURE_EDGE_LEFT,    /* quarter 1: Landscape */
        GESTURE_EDGE_TOP,     /* quarter 2: Portrait upside down */
        GESTURE_EDGE_RIGHT,   /* quarter 3: Landscape upside down */
    };
    return edge_for_quarter[quarter];
}

/* Sorted so the menu order is stable. Without this it follows link order,
 * which changes when a file is added and makes the list jump around. */
static void sort_apps(void)
{
    for (int i = 1; i < apps_registered; i++) {
        const app_t *const key = apps[i];
        int j = i - 1;
        while (j >= 0 && strcmp(apps[j]->name, key->name) > 0) {
            apps[j + 1] = apps[j];
            j--;
        }
        apps[j + 1] = key;
    }
}

/* --- chrome ------------------------------------------------------------- */

static void draw_home_hint(gesture_edge_t edge)
{
    /* TOP/BOTTOM keep the strip horizontal (WIDTH wide, HEIGHT tall),
     * centred across the screen's width. LEFT/RIGHT need it turned - the
     * strip becomes vertical (HEIGHT wide, WIDTH tall - the two constants
     * swap, the same way content's own width/height swap under a quarter
     * turn elsewhere in this shell), centred down the screen's height
     * instead. In every case it sits HOME_HINT_MARGIN in from whichever
     * edge the gesture currently lives on. */
    int x = 0, y = 0, w = 0, h = 0;

    switch (edge) {
    case GESTURE_EDGE_TOP:
        w = HOME_HINT_WIDTH;
        h = HOME_HINT_HEIGHT;
        x = (GFX_WIDTH - w) / 2;
        y = HOME_HINT_MARGIN;
        break;
    case GESTURE_EDGE_BOTTOM:
        w = HOME_HINT_WIDTH;
        h = HOME_HINT_HEIGHT;
        x = (GFX_WIDTH - w) / 2;
        y = GFX_HEIGHT - HOME_HINT_MARGIN - h;
        break;
    case GESTURE_EDGE_LEFT:
        w = HOME_HINT_HEIGHT;
        h = HOME_HINT_WIDTH;
        x = HOME_HINT_MARGIN;
        y = (GFX_HEIGHT - h) / 2;
        break;
    case GESTURE_EDGE_RIGHT:
        w = HOME_HINT_HEIGHT;
        h = HOME_HINT_WIDTH;
        x = GFX_WIDTH - HOME_HINT_MARGIN - w;
        y = (GFX_HEIGHT - h) / 2;
        break;
    }

    /* Identical every frame (for a given edge), so it only needs redrawing
     * where the app has already disturbed it. Drawing unconditionally would
     * mark the band dirty on every frame and force it to be sent - a
     * seventh of the frame's bus time, spent on pixels that did not
     * change. */
    if (!gfx_region_dirty(x, y, w, h)) {
        return;
    }

    gfx_fill_rect(x, y, w, h, gfx_rgb(HOME_HINT_RGB));
}

/* Holds the failing checks on screen long enough to be read, or until the
 * screen is touched. Deliberately blocking: a board with dead hardware should
 * not scroll past its own diagnosis into a launcher that looks normal. */
static void show_post_failures(void)
{
    ESP_LOGE(TAG, "POST failed - showing report");

    gfx_clear(gfx_rgb(0x0A0C14));
    gfx_text(10, 10, "HARDWARE FAULT", gfx_rgb(0xFF5C5C));

    int y = 10 + gfx_text_height() + 10;
    y = post_ui_draw(y, true);

    gfx_text_scaled(10, y + 12, "touch to continue", gfx_rgb(0x8A93A8), 1);
    gfx_present();

    /* Touch is not running yet at this point in boot, so this is a plain
     * timeout. Long enough to read and photograph, short enough that an
     * unattended board still reaches the launcher. */
    vTaskDelay(pdMS_TO_TICKS(8000));
}

/* --- main --------------------------------------------------------------- */

/* Common tail of leaving whichever app is running, however the decision to
 * leave was made - the swipe gesture below, or an opted-out app's own PWR
 * fallback. */
static void leave_app(const app_t **current, input_t *input,
                      gesture_edge_t exit_edge)
{
    ESP_LOGI(TAG, "Leaving %s", (*current)->name);
    (*current)->exit();
    *current = NULL;
    /* The app's output is still in the framebuffer, so the launcher must
     * repaint even though its own description has not changed. */
    ui_invalidate();
    /* Draw it immediately, so the frame presented below is the home screen
     * rather than the app's last one. */
    ui_launcher_frame(input);
    /* ui_invalidate() just forced ui_launcher_frame() to repaint its whole
     * rect from the background colour up, which paints over the hint strip's
     * band along with everything else - draw_home_hint() has to run again
     * this same frame to put it back. Left out, the strip stayed missing
     * indefinitely: draw_home_hint() only draws when gfx_region_dirty()
     * already says its band is dirty for some OTHER reason, which the idle
     * launcher's own unchanging menu never gives it, on this frame or any
     * later one - the exact way an app is left is not something the idle
     * branch in step_app() below sees again to retry. */
    draw_home_hint(exit_edge);
}

/* Whichever of the launcher or the current app owns this frame, and the
 * transitions between them: choosing an app from the launcher, or swiping
 * home to leave one. */
static void step_app(const app_t **current, input_t *input, uint32_t dt_ms)
{
    /* Which edge the gesture lives on tracks the board's current
     * orientation - see exit_edge_for_quarter()'s own comment - so it is
     * recomputed each call rather than cached, the same as display_shell_quarter()
     * itself is cheap enough to call freely (it only reads a struct field).
     * Needed by both branches below: the strip is chrome the shell shows
     * wherever it's drawing, not a hint that only exists once there is an
     * app open to swipe away from - a board sitting on the launcher should
     * still show the same edge, so the affordance reads consistently no
     * matter what's on screen. */
    const gesture_edge_t exit_edge = exit_edge_for_quarter(display_shell_quarter());

    if (*current == NULL) {
        const int chosen = ui_launcher_frame(input);
        if (chosen >= 0 && chosen < apps_registered) {
            *current = apps[chosen];
            ESP_LOGI(TAG, "Starting %s", (*current)->name);
            (*current)->enter();
        } else {
            draw_home_hint(exit_edge);
        }
        return;
    }

    /* Opt-in per app - see app_t's own comment on home_gesture. An app that
     * leaves it unset gets no swipe detection and no hint strip; it must
     * provide its own way back to the launcher. */
    if ((*current)->home_gesture &&
        gesture_is_home_swipe(input, exit_edge, GFX_WIDTH, GFX_HEIGHT)) {
        leave_app(current, input, exit_edge);
        return;
    }

    /* A HELD PWR is a temporary, shell-level fallback for an app that opted
     * out of the swipe gesture - today, only app_sand.c, whose own comment
     * on home_gesture promises "a deliberate control of its own" still to
     * come. Held, not a plain press: app_sand.c's own handle_brush_input()
     * already reads input->power.pressed to cycle brush mode (erase,
     * explosion, ...), and a short press is how the PMU reports that same
     * cycling gesture everywhere else in this shell too - stealing it here
     * would silence that cycling the instant this fallback existed. held
     * fires from the PMU's own separate long-press interrupt (see buttons.h),
     * so the two are independent presses, not the same edge read twice.
     * Checked before frame() runs, the same as the swipe check above, so
     * app_sand.c never sees the hold that just exited it. */
    if (!(*current)->home_gesture && input->power.held) {
        leave_app(current, input, exit_edge);
        return;
    }

    (*current)->frame(dt_ms, input);
    if ((*current)->home_gesture) {
        draw_home_hint(exit_edge);
    }
}

/* Report throughput on a TIMER, not every N frames: an idle launcher repaints
 * nothing and runs at the tick ceiling, so counting frames alone lets the
 * report interval swing with load - and a log line costs several
 * milliseconds of UART, enough to throttle the very thing it is measuring. */
static void report_fps(int64_t now_us, int64_t *window_start, uint32_t *frames)
{
    (*frames)++;
    const int64_t since = now_us - *window_start;
    if (since >= 1500000) {
        ESP_LOGI(TAG, "%.1f fps", (double)*frames * 1000000.0 / (double)since);
        *frames = 0;
        *window_start = now_us;
    }
}

void app_main(void)
{
    /* Before the display: the SD card shares SPI2 with the panel, so this is
     * the one moment it can be tested without tearing anything down. */
    post_run_before_display();

    if (!gfx_init()) {
        ESP_LOGE(TAG, "Graphics failed to start; nothing more to do");
        /* Park rather than return - returning from app_main leaves the chip
         * idle and unflashable. */
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    /* The rest of the health check, now that the display is up and can be
     * reported on. Ships in every build, release included: "is this board
     * working" stays worth knowing in the field.
     *
     * Silent when healthy - a device should boot, not announce that it is
     * fine. Only a failure gets the screen, because that is the case where
     * nobody may have a serial cable attached and the information is
     * actionable. The full report is always available from the Diagnostics
     * app. */
    if (!post_run_after_display()) {
        show_post_failures();
    }

#if CONFIG_LAUNCHER_SELFTEST && CONFIG_LAUNCHER_SELFTEST_AUTORUN
    /* Diagnostics build only - a default build compiles none of this. The
     * suites draw to the framebuffer and drive the panel, so they run before
     * the shell paints anything of its own. A failure is reported rather than
     * fatal: the harness reads the result from the console, and a board that
     * still boots is easier to investigate than one that does not.
     *
     * Nested behind a second flag, not just CONFIG_LAUNCHER_SELFTEST: this
     * used to run unconditionally whenever the suites were compiled in,
     * which made every diagnostics build pay the full suite's wall-clock
     * cost - hundreds of tests, including device-only perf-budget tests
     * that run full-grid simulations - on every boot, regardless of whether
     * the visit had anything to do with testing. LAUNCHER_SELFTEST_AUTORUN
     * is what an automated harness like tools/report_test_results.sh turns
     * on; ordinary interactive --diag use leaves it off and triggers the
     * suite on demand from the Diagnostics app instead. This stays a
     * compile-time #if, not a runtime check, because it is a boot-time cost
     * question: the point is to not even pay for the decision at boot. */
    if (selftest_run() != 0) {
        ESP_LOGE(TAG, "self test reported failures");
    }
#endif

    /* The startup animation. After the health checks, so a board with a fault
     * says so before it does anything decorative, and before touch starts,
     * because there is nothing yet for a tap to reach. */
    boot_anim_run();

    touch_start();
    buttons_start();
    screenshot_start();

    /* Not fatal if this fails - the display sampling below just finds
     * imu_ready() false forever after and the shell stays upright, the same
     * graceful fallback app_sand.c's own imu_init() failure gets. */
    if (!imu_init()) {
        ESP_LOGW(TAG, "No IMU - display orientation stays upright");
    }
    display_init(&shell_display);

    /* display_init() leaves quarter at a neutral 0 on purpose - see its own
     * comment. DISPLAY_DEFAULT_QUARTER is what THIS shell actually boots
     * into, a fact about this one board, and main.c is where that belongs.
     * Set directly rather than through display_update(): quarter is a
     * plain field on a struct main.c already owns, and there is no prior
     * gravity reading to synthesise here - this is the state before the
     * first sample, not a transition from one orientation to another. */
    shell_display.quarter = DISPLAY_DEFAULT_QUARTER;

    ui_launcher_init();

    /* ui_init() (inside ui_launcher_init() above) resets the transform to
     * identity - it has to, so a stale transform from a previous run of a
     * host test or a future warm-restart path can never leak in - which
     * means the DISPLAY_DEFAULT_QUARTER just set above is not actually in
     * force yet. Apply it now, once, before the frame loop below ever
     * builds a UI frame or maps a touch through it, so the very first
     * thing drawn is already in the board's normal held orientation
     * instead of starting upright and visibly turning into place once the
     * loop's own periodic sample confirms what was already decided. */
    ui_set_transform(ui_transform_quarter_turn(
        display_quarter(&shell_display), GFX_WIDTH, GFX_HEIGHT));

    const app_t *current = NULL;   /* NULL means the launcher is showing */
    input_t input = { 0 };
    int64_t previous_us = esp_timer_get_time();
    int64_t fps_window_start = previous_us;
    uint32_t frames = 0;
    int64_t next_display_sample_us = previous_us;

    sort_apps();
    ESP_LOGI(TAG, "Ready, %d app%s registered",
             apps_registered, apps_registered == 1 ? "" : "s");

    while (1) {
        const int64_t now_us = esp_timer_get_time();
        uint32_t dt_ms = (uint32_t)((now_us - previous_us) / 1000);
        previous_us = now_us;
        if (dt_ms > 250) {
            dt_ms = 250;   /* clamp, so a stall does not jump animation */
        }

        touch_read(&input);
        buttons_read(&input.boot, &input.power);

        /* Resampled at DISPLAY_SAMPLE_MS, not every frame - see that
         * constant's own comment. Ahead of step_app(), so a transform change
         * this iteration is already in force before anything below builds a
         * UI frame or maps this frame's touch through it - draw_palette() in
         * app_sand.c used to have to get this same ordering right locally
         * for exactly the same reason; now it is main.c's job once, for
         * everything. */
        if (now_us >= next_display_sample_us) {
            next_display_sample_us = now_us + (int64_t)DISPLAY_SAMPLE_MS * 1000;

            imu_sample_t sample;
            if (imu_ready() && imu_read(&sample)) {
                const int gx = DISPLAY_GRAVITY_X(&sample);
                const int gy = DISPLAY_GRAVITY_Y(&sample);
                if (display_update(&shell_display, gx, gy)) {
                    ui_set_transform(ui_transform_quarter_turn(
                        display_quarter(&shell_display), GFX_WIDTH, GFX_HEIGHT));
                }
            }
        }

        step_app(&current, &input, dt_ms);

        /* A capture requested from the host over the console's UART - see
         * util/screenshot.c's own top comment for why that arrives as a
         * flag rather than a direct call from the listener task. Checked
         * after the frame is drawn but before it is sent, so what is
         * streamed off is exactly what gfx_present() below is about to put
         * on screen, regardless of which app (or the launcher) just drew
         * it. */
        if (screenshot_take_request()) {
            screenshot_dump();
        }

        gfx_present();
        report_fps(now_us, &fps_window_start, &frames);

        /* Yield so the idle task can feed the watchdog. */
        vTaskDelay(1);
    }
}
