#include <stdlib.h>
#include <stdint.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"

#include "context.h"
#include "framebuffer.h"
#include "inputassembler.h"

/* Render offscreen at a modest size - the board has no PSRAM, and swrast's
 * framebuffer allocates both a color and a depth buffer (8 bytes/pixel). */
#define RENDER_SIZE 120

static const char *TAG = "triangle_demo";

static inline uint16_t to_rgb565(unsigned char r, unsigned char g, unsigned char b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting display");
    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "Failed to start display");
        return;
    }

    /* context is ~5 KB (31-entry vertex cache, lights, matrices) - too big
     * for the "main" task's stack, so it must be heap-allocated. */
    context *ctx = malloc(sizeof(context));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Failed to allocate rasterizer context");
        return;
    }

    framebuffer fb;

    if (!framebuffer_init(&fb, RENDER_SIZE, RENDER_SIZE)) {
        ESP_LOGE(TAG, "Failed to allocate rasterizer framebuffer");
        free(ctx);
        return;
    }

    context_init(ctx);
    ctx->target = &fb;
    context_set_viewport(ctx, 0, 0, RENDER_SIZE, RENDER_SIZE);

    framebuffer_clear(&fb, 16, 16, 24, 255);

    /* one RGB-interpolated triangle: red top, green bottom-left, blue bottom-right */
    ia_begin(ctx);

    ia_color(ctx, 1.0f, 0.0f, 0.0f, 1.0f);
    ia_vertex(ctx, 0.0f, 0.8f, 0.0f, 1.0f);

    ia_color(ctx, 0.0f, 1.0f, 0.0f, 1.0f);
    ia_vertex(ctx, -0.8f, -0.6f, 0.0f, 1.0f);

    ia_color(ctx, 0.0f, 0.0f, 1.0f, 1.0f);
    ia_vertex(ctx, 0.8f, -0.6f, 0.0f, 1.0f);

    ia_end(ctx);
    free(ctx);

    size_t buf_size = (size_t)RENDER_SIZE * RENDER_SIZE * sizeof(uint16_t);
    uint16_t *canvas_buf = malloc(buf_size);
    if (canvas_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate canvas buffer");
        framebuffer_cleanup(&fb);
        return;
    }

    for (int i = 0; i < RENDER_SIZE * RENDER_SIZE; i++) {
        color4 px = fb.color[i];
        canvas_buf[i] = to_rgb565(px.components[RED], px.components[GREEN],
                                   px.components[BLUE]);
    }

    framebuffer_cleanup(&fb);

    bsp_display_lock(0);

    lv_obj_t *canvas = lv_canvas_create(lv_screen_active());
    lv_canvas_set_buffer(canvas, canvas_buf, RENDER_SIZE, RENDER_SIZE,
                          LV_COLOR_FORMAT_RGB565);
    lv_obj_center(canvas);

    bsp_display_unlock();

    ESP_LOGI(TAG, "Triangle rasterized and drawn");
}
