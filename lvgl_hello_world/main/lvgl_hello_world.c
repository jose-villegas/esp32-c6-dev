#include "bsp/esp-bsp.h"
#include "esp_log.h"

static const char *TAG = "lvgl_hello_world";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting display");
    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "Failed to start display");
        return;
    }

    bsp_display_lock(0);

    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "Hello World!");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    lv_obj_center(label);

    bsp_display_unlock();

    ESP_LOGI(TAG, "Hello world drawn on display");
}
