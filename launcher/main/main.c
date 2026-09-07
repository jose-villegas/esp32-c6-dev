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

#if CONFIG_LAUNCHER_DEVELOPMENT
#include "util/screenshot.h"
#endif

#if CONFIG_LAUNCHER_SELFTEST
#include "boot/selftest.h"
#include "suites.h"
#endif

#include "bsp/esp-bsp.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "shell";

#if CONFIG_LAUNCHER_DEVELOPMENT
#include "esp_heap_caps.h"

/* Free heap alone never predicts whether the next big allocation fits:
 * the framebuffer and the sand grid each need ONE CONTIGUOUS block, and
 * free space can sit outside the largest one with nothing saying where
 * it went. Printing both numbers at each boot phase says which phase
 * loses it. */
static void heap_mark(const char *where)
{
    ESP_LOGI(TAG, "HEAPMARK %-18s free %6u largest %6u", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}
#else
#define heap_mark(where) ((void)0)
#endif

#define HOME_HINT_WIDTH   120
#define HOME_HINT_HEIGHT  4
#define HOME_HINT_MARGIN  10
#define HOME_HINT_RGB     0x4A5268

/* 10 Hz: sufficient for reorientation without lag. */
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

/* Board layout fact; see app_sand.c. Duplicated for clarity. Sharing not
 * covered. */
#define DISPLAY_GRAVITY_X(s)  (-(s)->ay)
#define DISPLAY_GRAVITY_Y(s)  ( (s)->ax)

static display_t shell_display;

int display_shell_quarter(void) { return display_quarter(&shell_display); }

/* Content-driven, not a fixed physical reference: the exit gesture lives
 * on whichever PHYSICAL edge the content's logical bottom maps to,
 * tracking rotation the same way ui_transform_rect() makes buttons/text
 * do. An earlier USB-opposite-edge rule tested wrong on device (Portrait
 * needs BOTTOM, not the LEFT that rule produced) - not hand-derived any
 * more: this table maps a strip along the logical canvas's bottom edge
 * through the same transform pipeline the exhaustive sweep already
 * proved exact. */
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

    if (!gfx_region_dirty(x, y, w, h)) {
        return;
    }

    gfx_fill_rect(x, y, w, h, gfx_rgb(HOME_HINT_RGB));
}

/* Holds failing checks until touch. Prevents dead hardware diagnosis from
 * scrolling to launcher. */
static void show_post_failures(void)
{
    ESP_LOGE(TAG, "POST failed - showing report");

    gfx_clear(gfx_rgb(0x0A0C14));
    gfx_text(10, 10, "HARDWARE FAULT", gfx_rgb(0xFF5C5C));

    int y = 10 + gfx_text_height() + 10;
    y = post_ui_draw(y, true);

    gfx_text_scaled(10, y + 12, "touch to continue", gfx_rgb(0x8A93A8), 1);
    gfx_present();

    /* Long timeout for manual action, short for unattended use. */
    vTaskDelay(pdMS_TO_TICKS(8000));
}

/* --- main --------------------------------------------------------------- */

static void leave_app(const app_t **current, input_t *input,
                      gesture_edge_t exit_edge)
{
    ESP_LOGI(TAG, "Leaving %s", (*current)->name);
    (*current)->exit();
    *current = NULL;
    /* Launcher must repaint due to framebuffer output. */
    ui_invalidate();
    /* Draw it immediately, so the frame presented below is the home screen
     * rather than the app's last one. */
    ui_launcher_frame(input);
    /* ui_launcher_frame() just repainted its whole rect over the hint
     * strip's band, so this has to run again to put it back - dirty
     * tracking alone will not retry it, since nothing else marks that
     * band dirty on a later frame. */
    draw_home_hint(exit_edge);
}

static void step_app(const app_t **current, input_t *input, uint32_t dt_ms)
{
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

    /* See app_t.home_gesture. Unset apps get no swipe detection or hint
     * strip. */
    if ((*current)->home_gesture &&
        gesture_is_home_swipe(input, exit_edge, GFX_WIDTH, GFX_HEIGHT)) {
        leave_app(current, input, exit_edge);
        return;
    }

    /* HELD, not a plain press: app_sand.c's own handle_brush_input()
     * already reads a short press to cycle brush mode, and stealing it
     * here would silence that everywhere else in this shell too. `held`
     * fires from the PMU's own separate long-press interrupt (buttons.h),
     * so the two are independent presses, not the same edge read twice.
     * Checked before frame() runs, so the app never sees the hold that
     * just exited it. */
    if (!(*current)->home_gesture && input->power.held) {
        leave_app(current, input, exit_edge);
        return;
    }

    (*current)->frame(dt_ms, input);
    if ((*current)->home_gesture) {
        draw_home_hint(exit_edge);
    }
}

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Report throughput on TIMER, not frames. CONFIG_LAUNCHER_DEVELOPMENT only */
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
#endif

void app_main(void)
{
    heap_mark("boot");

    /* Test SD card during panel use. */
    post_run_before_display();
    heap_mark("after sd probe");

    if (!gfx_init()) {
        ESP_LOGE(TAG, "Graphics failed to start; nothing more to do");
        /* Park rather than return - returning from app_main leaves the chip
         * idle and unflashable. */
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    heap_mark("after gfx_init");
#if CONFIG_LAUNCHER_DEVELOPMENT
    heap_caps_dump(MALLOC_CAP_DMA);
#endif

    if (!post_run_after_display()) {
        show_post_failures();
    }
    heap_mark("after post");

#if CONFIG_LAUNCHER_SELFTEST && CONFIG_LAUNCHER_SELFTEST_AUTORUN
    if (selftest_run() != 0) {
        ESP_LOGE(TAG, "self test reported failures");
    }
#endif

    boot_anim_run();
    heap_mark("after boot anim");

    touch_start();
    buttons_start();
#if CONFIG_LAUNCHER_DEVELOPMENT
    screenshot_start();
#endif

    if (!imu_init()) {
        ESP_LOGW(TAG, "No IMU - display orientation stays upright");
    }
    display_init(&shell_display);

    shell_display.quarter = DISPLAY_DEFAULT_QUARTER;

    ui_launcher_init();
    heap_mark("shell ready");

    /* ui_init() above already reset the transform to identity (it has to,
     * so a stale one from a previous host test can never leak in), so
     * DISPLAY_DEFAULT_QUARTER is not actually in force yet - apply it
     * once here before the first frame is built, or the board would
     * start upright and visibly turn into place. */
    ui_set_transform(ui_transform_quarter_turn(
        display_quarter(&shell_display), GFX_WIDTH, GFX_HEIGHT));

    const app_t *current = NULL;   /* NULL means the launcher is showing */
    input_t input = { 0 };
    int64_t previous_us = esp_timer_get_time();
#if CONFIG_LAUNCHER_DEVELOPMENT
    int64_t fps_window_start = previous_us;
    uint32_t frames = 0;
#endif
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

#if CONFIG_LAUNCHER_SELFTEST
        /* See util/screenshot.c for framebuffer contention explanation. */
        char runsuite_name[64];
        if (screenshot_take_runsuite_request(runsuite_name, sizeof runsuite_name)) {
            if (!suites_run_one(runsuite_name)) {
                ESP_LOGE(TAG, "no suite named '%s' is registered", runsuite_name);
            }
        }
#endif

        touch_read(&input);
        buttons_read(&input.boot, &input.power);

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

#if CONFIG_LAUNCHER_DEVELOPMENT
        if (screenshot_take_request()) {
            screenshot_dump(&input, current);
        }
#endif

        gfx_present();
#if CONFIG_LAUNCHER_DEVELOPMENT
        report_fps(now_us, &fps_window_start, &frames);
#endif

        /* Yield so the idle task can feed the watchdog. */
        vTaskDelay(1);
    }
}
