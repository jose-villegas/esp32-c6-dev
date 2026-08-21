#include <stdlib.h>
#include <stdint.h>

#include "bsp/esp-bsp.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "context.h"
#include "framebuffer.h"
#include "inputassembler.h"

/* BSP_LCD_H_RES / BSP_LCD_V_RES are the panel's real max resolution,
 * fetched from the board support package rather than hardcoded. */
#define H_RES BSP_LCD_H_RES
#define V_RES BSP_LCD_V_RES

/* Render + stream in horizontal strips straight to the panel's own GRAM
 * (bypassing LVGL entirely). The board has no PSRAM, and a full-resolution
 * swrast framebuffer (color + depth, 8 bytes/pixel) would need ~1.3 MB -
 * far more than the ~357 KB of internal RAM available. Tiling keeps peak
 * memory to one strip's worth while still covering the whole screen.
 * 448 / 64 = 7 strips exactly, no partial tile. */
#define TILE_H 64

static const char *TAG = "triangle_demo";

/* esp_lvgl_port configures this panel with swap_bytes=true - the QSPI
 * controller expects byte-swapped RGB565. LVGL normally handles that for
 * us; going through the raw panel API, we have to swap it ourselves. */
static inline uint16_t to_rgb565_swapped(unsigned char r, unsigned char g, unsigned char b)
{
    uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((v >> 8) | (v << 8));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Display resolution: %d x %d", H_RES, V_RES);

    esp_lcd_panel_handle_t panel = NULL;
    const bsp_display_config_t disp_cfg = {
        .max_transfer_sz = H_RES * TILE_H * sizeof(uint16_t),
    };
    if (bsp_display_new(&disp_cfg, &panel, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init display");
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
    if (!framebuffer_init(&fb, H_RES, TILE_H)) {
        ESP_LOGE(TAG, "Failed to allocate rasterizer tile framebuffer");
        free(ctx);
        return;
    }

    size_t tile_bytes = (size_t)H_RES * TILE_H * sizeof(uint16_t);
    uint16_t *tile_buf = heap_caps_malloc(tile_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (tile_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate DMA tile buffer");
        framebuffer_cleanup(&fb);
        free(ctx);
        return;
    }

    context_init(ctx);
    ctx->target = &fb;

    for (int y0 = 0; y0 < V_RES; y0 += TILE_H) {
        /* Rasterize the SAME full-screen-NDC triangle every pass, but shift
         * the viewport up by y0 so this strip's local row 0 lines up with
         * screen row y0. draw_area clamps to the tile buffer's own extents,
         * so only pixels actually inside [y0, y0+TILE_H) get written. */
        context_set_viewport(ctx, 0, -y0, H_RES, V_RES);

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

        for (int i = 0; i < H_RES * TILE_H; i++) {
            color4 px = fb.color[i];
            tile_buf[i] = to_rgb565_swapped(px.components[RED], px.components[GREEN],
                                             px.components[BLUE]);
        }

        esp_lcd_panel_draw_bitmap(panel, 0, y0, H_RES, y0 + TILE_H, tile_buf);
    }

    framebuffer_cleanup(&fb);
    heap_caps_free(tile_buf);
    free(ctx);

    ESP_LOGI(TAG, "Full-resolution triangle rasterized and drawn");
}
