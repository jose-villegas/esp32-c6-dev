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
#include "gfx.h"
#include "ui_launcher.h"

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_lcd_touch.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "shell";

/* Chrome drawn over a running app. */
#define BAR_HEIGHT     40
#define BAR_RGB        0x161A28
#define BAR_TEXT_RGB   0xE6EAF2
#define ACCENT_RGB     0x3DDC97

/* The back control occupies the left end of the bar. Sized generously - a
 * fingertip is far wider than the glyph. */
#define BACK_WIDTH     72

/* --- app registry ------------------------------------------------------- */

/* Adding an app is: write it, declare it here, add one line to the registry. */
extern const app_t app_cube;

const app_t *const app_registry[] = {
    &app_cube,
};

const int app_count = (int)(sizeof(app_registry) / sizeof(app_registry[0]));

/* --- touch -------------------------------------------------------------- */

static esp_lcd_touch_handle_t touch;
static uint32_t touch_errors;

static void read_input(input_t *input, const input_t *previous)
{
    esp_lcd_touch_point_data_t point = { 0 };
    uint8_t count = 0;
    bool down = false;

    /* Only talk to the controller when it says it has something to report.
     *
     * The FT5x06 NACKs register reads while idle, so polling it every frame
     * produces a failed I2C transaction per frame - and each failure costs a
     * bus timeout, which stalls the whole loop. The INT line (active low)
     * exists precisely to signal "data ready"; gating on it is both the
     * designed behaviour and what keeps the frame loop fast. */
    const bool data_ready = (touch != NULL) &&
                            (gpio_get_level(BSP_LCD_TOUCH_INT) == 0);

    if (data_ready) {
        /* Two calls by design: read_data() pulls a fresh sample from the
         * controller over I2C, get_data() returns what that sample held. */
        if (esp_lcd_touch_read_data(touch) == ESP_OK) {
            down = esp_lcd_touch_get_data(touch, &point, &count, 1) == ESP_OK &&
                   count > 0;
        } else {
            touch_errors++;
        }
    }

    input->down = down;
    /* Keep the last known position on release: the coordinates are no longer
     * valid once the finger is gone, but UI code needs to know where the
     * release happened. */
    input->x = down ? (int)point.x : previous->x;
    input->y = down ? (int)point.y : previous->y;

    input->pressed  =  down && !previous->down;
    input->released = !down &&  previous->down;
}

/* --- chrome ------------------------------------------------------------- */

static void draw_app_bar(const char *name)
{
    gfx_fill_rect(0, 0, GFX_WIDTH, BAR_HEIGHT, gfx_rgb(BAR_RGB));

    /* A left-pointing chevron, drawn as a stack of blocks - cheaper than
     * carrying an icon font for one glyph. */
    const int cx = 26, cy = BAR_HEIGHT / 2;
    for (int i = 0; i < 8; i++) {
        gfx_fill_rect(cx + i, cy - i - 1, 3, 2, gfx_rgb(ACCENT_RGB));
        gfx_fill_rect(cx + i, cy + i - 1, 3, 2, gfx_rgb(ACCENT_RGB));
    }

    gfx_text(BACK_WIDTH, (BAR_HEIGHT - gfx_text_height()) / 2, name,
             gfx_rgb(BAR_TEXT_RGB));
}

static bool back_was_tapped(const input_t *input)
{
    return input->released &&
           input->x < BACK_WIDTH &&
           input->y < BAR_HEIGHT;
}

/* --- main --------------------------------------------------------------- */

void app_main(void)
{
    if (!gfx_init()) {
        ESP_LOGE(TAG, "Graphics failed to start; nothing more to do");
        /* Park rather than return - returning from app_main leaves the chip
         * idle and unflashable. */
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    if (bsp_touch_new(NULL, &touch) != ESP_OK) {
        ESP_LOGW(TAG, "Touch unavailable; the launcher will not be usable");
        touch = NULL;
    }

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

        const input_t previous = input;
        read_input(&input, &previous);

        if (current == NULL) {
            const int chosen = ui_launcher_frame(&input);
            if (chosen >= 0 && chosen < app_count) {
                current = app_registry[chosen];
                ESP_LOGI(TAG, "Starting %s", current->name);
                current->enter();
            }
        } else if (back_was_tapped(&input)) {
            ESP_LOGI(TAG, "Leaving %s", current->name);
            current->exit();
            current = NULL;
            /* Draw the launcher immediately so the frame we present below is
             * the home screen rather than the app's last one. */
            ui_launcher_frame(&input);
        } else {
            current->frame(dt_ms, &input);
            draw_app_bar(current->name);
        }

        gfx_present();

        /* Report throughput periodically. Touch errors are counted rather
         * than logged per occurrence, so a misbehaving controller cannot
         * flood the console (or slow the loop by logging). */
        if (++frames % 60 == 0) {
            const int64_t now = esp_timer_get_time();
            ESP_LOGI(TAG, "%.1f fps | touch errors %u",
                     60.0 * 1000000.0 / (double)(now - fps_window_start),
                     (unsigned)touch_errors);
            fps_window_start = now;
        }

        /* Yield so the idle task can feed the watchdog. */
        vTaskDelay(1);
    }
}
