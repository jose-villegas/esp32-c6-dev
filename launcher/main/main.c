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
 * BOOT button - see docs/ESP32-C6-AMOLED-Notes.md.
 *===========================================================================*/

#include <stdint.h>
#include <string.h>

#include "app.h"
#include "gesture.h"
#include "gfx.h"
#include "post.h"
#include "post_ui.h"
#include "buttons.h"
#include "touch.h"
#include "ui.h"
#include "ui_launcher.h"

#if CONFIG_LAUNCHER_SELFTEST
#include "selftest.h"
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

static void draw_home_hint(void)
{
    const int x = (GFX_WIDTH - HOME_HINT_WIDTH) / 2;
    const int y = GFX_HEIGHT - HOME_HINT_MARGIN - HOME_HINT_HEIGHT;

    /* Identical every frame, so it only needs redrawing where the app has
     * already disturbed it. Drawing unconditionally would mark the bottom band
     * dirty on every frame and force it to be sent - a seventh of the frame's
     * bus time, spent on pixels that did not change. */
    if (!gfx_region_dirty(x, y, HOME_HINT_WIDTH, HOME_HINT_HEIGHT)) {
        return;
    }

    gfx_fill_rect(x, y, HOME_HINT_WIDTH, HOME_HINT_HEIGHT,
                  gfx_rgb(HOME_HINT_RGB));
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

/* Whichever of the launcher or the current app owns this frame, and the
 * transitions between them: choosing an app from the launcher, or swiping
 * home to leave one. */
static void step_app(const app_t **current, input_t *input, uint32_t dt_ms)
{
    if (*current == NULL) {
        const int chosen = ui_launcher_frame(input);
        if (chosen >= 0 && chosen < apps_registered) {
            *current = apps[chosen];
            ESP_LOGI(TAG, "Starting %s", (*current)->name);
            (*current)->enter();
        }
        return;
    }

    if (gesture_is_home_swipe(input, GFX_HEIGHT)) {
        ESP_LOGI(TAG, "Leaving %s", (*current)->name);
        (*current)->exit();
        *current = NULL;
        /* The app's output is still in the framebuffer, so the launcher must
         * repaint even though its own description has not changed. */
        ui_invalidate();
        /* Draw it immediately, so the frame presented below is the home
         * screen rather than the app's last one. */
        ui_launcher_frame(input);
        return;
    }

    (*current)->frame(dt_ms, input);
    draw_home_hint();
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

#if CONFIG_LAUNCHER_SELFTEST
    /* Diagnostics build only - a default build compiles none of this. The
     * suites draw to the framebuffer and drive the panel, so they run before
     * the shell paints anything of its own. A failure is reported rather than
     * fatal: the harness reads the result from the console, and a board that
     * still boots is easier to investigate than one that does not. */
    if (selftest_run() != 0) {
        ESP_LOGE(TAG, "self test reported failures");
    }
#endif

    touch_start();
    buttons_start();

    ui_launcher_init();

    const app_t *current = NULL;   /* NULL means the launcher is showing */
    input_t input = { 0 };
    int64_t previous_us = esp_timer_get_time();
    int64_t fps_window_start = previous_us;
    uint32_t frames = 0;

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

        step_app(&current, &input, dt_ms);

        gfx_present();
        report_fps(now_us, &fps_window_start, &frames);

        /* Yield so the idle task can feed the watchdog. */
        vTaskDelay(1);
    }
}
