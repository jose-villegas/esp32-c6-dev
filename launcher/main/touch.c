#include "touch.h"

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

/* How long to wait, with the controller reporting nothing, before deciding the
 * finger is gone.
 *
 * The FT5x06's INT line is not a reliable "finger is down" level - it signals
 * "there is fresh data", and it can drop briefly mid-touch. Treating every
 * deassertion as a release makes a held finger flicker, which produces
 * spurious press/release pairs. Requiring a quiet period instead debounces
 * that, at the cost of a few milliseconds of extra latency on release. */
#define RELEASE_QUIET_MS 60

static esp_lcd_touch_handle_t panel;

/* Shared between the polling task and the render loop. The chip is
 * single-core, so a spinlock-guarded critical section is both correct and
 * essentially free here. */
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static struct {
    bool down;
    int  x, y;
    int  press_x, press_y;   /* where the current/last touch began */
    bool pressed;            /* latched until read */
    bool released;           /* latched until read */
} shared;

static void poll_once(void)
{
    static int64_t last_seen_us;
    static bool    was_down;

    bool  have_point = false;
    int   x = 0, y = 0;

    /* Only talk to the controller when it says it has something. The FT5x06
     * NACKs register reads while idle, and each failed transaction costs a bus
     * timeout - polling it blindly at 100 Hz would swamp the system. */
    if (panel != NULL && gpio_get_level(BSP_LCD_TOUCH_INT) == 0) {
        if (esp_lcd_touch_read_data(panel) == ESP_OK) {
            esp_lcd_touch_point_data_t point = { 0 };
            uint8_t count = 0;
            if (esp_lcd_touch_get_data(panel, &point, &count, 1) == ESP_OK &&
                count > 0) {
                have_point = true;
                x = point.x;
                y = point.y;
            }
        }
    }

    const int64_t now_us = esp_timer_get_time();
    bool down = was_down;

    if (have_point) {
        down = true;
        last_seen_us = now_us;
    } else if (was_down &&
               (now_us - last_seen_us) > (RELEASE_QUIET_MS * 1000)) {
        down = false;
    }

    portENTER_CRITICAL(&lock);
    if (down && !was_down) {
        shared.pressed  = true;
        shared.press_x  = x;
        shared.press_y  = y;
    } else if (!down && was_down) {
        shared.released = true;
    }
    if (have_point) {
        shared.x = x;
        shared.y = y;
    }
    shared.down = down;
    portEXIT_CRITICAL(&lock);

    was_down = down;
}

static void touch_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(1000 / TOUCH_POLL_HZ);
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        poll_once();
        vTaskDelayUntil(&last_wake, period > 0 ? period : 1);
    }
}

void touch_start(void)
{
    if (bsp_touch_new(NULL, &panel) != ESP_OK) {
        ESP_LOGW(TAG, "Touch controller unavailable; input will not work");
        panel = NULL;
    }

    /* Above the render loop's priority so a long blit cannot delay sampling,
     * which is the entire point of running it separately. */
    xTaskCreate(touch_task, "touch", 3072, NULL, 6, NULL);
}

void touch_read(input_t *out)
{
    portENTER_CRITICAL(&lock);
    out->down     = shared.down;
    out->pressed  = shared.pressed;
    out->released = shared.released;
    out->x        = shared.x;
    out->y        = shared.y;
    out->press_x  = shared.press_x;
    out->press_y  = shared.press_y;

    /* Consume the edges so each is reported to exactly one frame. */
    shared.pressed  = false;
    shared.released = false;
    portEXIT_CRITICAL(&lock);
}
