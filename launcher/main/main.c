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

#include "app.h"
#include "gesture.h"
#include "gfx.h"
#include "post.h"
#include "touch.h"
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

/* Adding an app is: write it, declare it here, add one line to the registry. */
extern const app_t app_cube;

const app_t *const app_registry[] = {
    &app_cube,
};

const int app_count = (int)(sizeof(app_registry) / sizeof(app_registry[0]));

/* --- chrome ------------------------------------------------------------- */

static void draw_home_hint(void)
{
    gfx_fill_rect((GFX_WIDTH - HOME_HINT_WIDTH) / 2,
                  GFX_HEIGHT - HOME_HINT_MARGIN - HOME_HINT_HEIGHT,
                  HOME_HINT_WIDTH, HOME_HINT_HEIGHT,
                  gfx_rgb(HOME_HINT_RGB));
}

/* --- main --------------------------------------------------------------- */

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
     * working" stays worth knowing in the field. */
    post_run_after_display();

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

    ui_launcher_init();

    const app_t *current = NULL;   /* NULL means the launcher is showing */
    input_t input = { 0 };
    int64_t previous_us = esp_timer_get_time();
    int64_t fps_window_start = previous_us;
    uint32_t frames = 0;

    ESP_LOGI(TAG, "Ready, %d app%s registered",
             app_count, app_count == 1 ? "" : "s");

    while (1) {
        const int64_t now_us = esp_timer_get_time();
        uint32_t dt_ms = (uint32_t)((now_us - previous_us) / 1000);
        previous_us = now_us;
        if (dt_ms > 250) {
            dt_ms = 250;   /* clamp, so a stall does not jump animation */
        }

        touch_read(&input);

        if (current == NULL) {
            const int chosen = ui_launcher_frame(&input);
            if (chosen >= 0 && chosen < app_count) {
                current = app_registry[chosen];
                ESP_LOGI(TAG, "Starting %s", current->name);
                current->enter();
            }
        } else if (gesture_is_home_swipe(&input, GFX_HEIGHT)) {
            ESP_LOGI(TAG, "Leaving %s", current->name);
            current->exit();
            current = NULL;
            /* Draw the launcher immediately so the frame presented below is
             * the home screen rather than the app's last one. */
            ui_launcher_frame(&input);
        } else {
            current->frame(dt_ms, &input);
            draw_home_hint();
        }

        gfx_present();

        /* Report throughput periodically rather than per frame - logging is
         * not free, and at 25 fps it would be the loudest thing on the wire. */
        if (++frames % 60 == 0) {
            const int64_t now = esp_timer_get_time();
            ESP_LOGI(TAG, "%.1f fps",
                     60.0 * 1000000.0 / (double)(now - fps_window_start));
            fps_window_start = now;
        }

        /* Yield so the idle task can feed the watchdog. */
        vTaskDelay(1);
    }
}
