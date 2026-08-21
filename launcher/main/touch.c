#include "touch.h"
#include "touch_fsm.h"

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

static esp_lcd_touch_handle_t panel;

/* All the interpretation lives in touch_fsm, which is hardware-free and
 * covered by host tests. This file is only responsible for getting samples out
 * of the controller and handing them over. */
static touch_fsm_t fsm;

/* Shared between the polling task and the render loop. The chip is
 * single-core, so a spinlock-guarded critical section is both correct and
 * essentially free here. */
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static void poll_once(void)
{
    bool have_point = false;
    int  x = 0, y = 0;

    /* Only talk to the controller when it says it has something. The FT5x06
     * NACKs register reads while idle, and each failed transaction costs a bus
     * timeout - polling blindly at this rate would swamp the system. */
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

    portENTER_CRITICAL(&lock);
    touch_fsm_update(&fsm, have_point, x, y, now_us);
    portEXIT_CRITICAL(&lock);
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
    touch_fsm_init(&fsm);

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
    touch_fsm_take(&fsm, out);
    portEXIT_CRITICAL(&lock);
}
