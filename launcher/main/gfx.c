#include "gfx.h"

#include <string.h>

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "font8x8_basic.h"

/* The frame is sent in full-width bands. Full width matters: it makes each
 * band a contiguous run inside the framebuffer, so the DMA reads it in place
 * with no copy. 448 / 64 = 7 bands exactly. */
#define STRIP_HEIGHT 64
#define STRIP_COUNT  (GFX_HEIGHT / STRIP_HEIGHT)

static const char *TAG = "gfx";

static gfx_color_t *fb;
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t strip_sent;

/* Current clip rectangle, as inclusive-exclusive bounds. */
static struct { int x0, y0, x1, y1; } clip;

/*---------------------------------------------------------------------------
 * Panel plumbing
 *-------------------------------------------------------------------------*/

static bool IRAM_ATTR on_strip_sent(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *event,
                                    void *user_context)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(strip_sent, &woken);
    return woken == pdTRUE;
}

bool gfx_init(void)
{
    /* Counting, not binary: a whole frame's strips are queued before any is
     * awaited, so several finish first. A binary semaphore would saturate at
     * one, discard the rest, and deadlock on the next wait. */
    strip_sent = xSemaphoreCreateCounting(STRIP_COUNT + 2, 0);
    if (strip_sent == NULL) {
        ESP_LOGE(TAG, "Could not create the strip-transfer semaphore");
        return false;
    }

    /* bsp_display_new(), not bsp_display_start(): the latter also starts LVGL,
     * which would cost tens of KiB we would rather spend on the framebuffer. */
    esp_lcd_panel_io_handle_t io = NULL;
    const bsp_display_config_t config = {
        .max_transfer_sz = GFX_WIDTH * STRIP_HEIGHT * sizeof(gfx_color_t),
    };
    if (bsp_display_new(&config, &panel, &io) != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the display");
        return false;
    }

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = on_strip_sent,
    };
    if (esp_lcd_panel_io_register_event_callbacks(io, &callbacks, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Could not register the transfer-complete callback");
        return false;
    }

    const size_t bytes = (size_t)GFX_WIDTH * GFX_HEIGHT * sizeof(gfx_color_t);
    fb = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (fb == NULL) {
        ESP_LOGE(TAG, "Could not allocate %u byte framebuffer "
                      "(largest free DMA block is %u bytes)",
                 (unsigned)bytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        return false;
    }

    gfx_clear_clip();

    ESP_LOGI(TAG, "%dx%d framebuffer, %u bytes, %u bytes of heap still free",
             GFX_WIDTH, GFX_HEIGHT, (unsigned)bytes,
             (unsigned)esp_get_free_heap_size());
    return true;
}

gfx_color_t *gfx_framebuffer(void)
{
    return fb;
}

/*---------------------------------------------------------------------------
 * Colour
 *-------------------------------------------------------------------------*/

/* Pack 0xRRGGBB into the panel's format: RGB565, byte-swapped. The swap is
 * required because this QSPI controller expects the high and low bytes in the
 * opposite order to the chip's native layout - LVGL's port layer normally does
 * it for you, but we drive the panel directly. */
gfx_color_t gfx_rgb(uint32_t rgb)
{
    const uint8_t r = (rgb >> 16) & 0xFF;
    const uint8_t g = (rgb >> 8)  & 0xFF;
    const uint8_t b =  rgb        & 0xFF;

    const uint16_t packed = (uint16_t)(((r & 0xF8) << 8) |
                                       ((g & 0xFC) << 3) |
                                       ( b        >> 3));
    return (gfx_color_t)((packed >> 8) | (packed << 8));
}

/*---------------------------------------------------------------------------
 * Clipping
 *-------------------------------------------------------------------------*/

void gfx_set_clip(int x, int y, int w, int h)
{
    int x1 = x + w;
    int y1 = y + h;

    clip.x0 = x  < 0 ? 0 : x;
    clip.y0 = y  < 0 ? 0 : y;
    clip.x1 = x1 > GFX_WIDTH  ? GFX_WIDTH  : x1;
    clip.y1 = y1 > GFX_HEIGHT ? GFX_HEIGHT : y1;
}

void gfx_clear_clip(void)
{
    clip.x0 = 0;
    clip.y0 = 0;
    clip.x1 = GFX_WIDTH;
    clip.y1 = GFX_HEIGHT;
}

/*---------------------------------------------------------------------------
 * Primitives
 *-------------------------------------------------------------------------*/

/* Ignores the clip rect by design - this is the whole-screen wipe that starts
 * a frame, and it writes two pixels per 32-bit store. */
void gfx_clear(gfx_color_t color)
{
    const uint32_t pair = ((uint32_t)color << 16) | color;
    uint32_t *words = (uint32_t *)fb;
    const int count = (GFX_WIDTH * GFX_HEIGHT) / 2;

    for (int i = 0; i < count; i++) {
        words[i] = pair;
    }
}

void gfx_pixel(int x, int y, gfx_color_t color)
{
    if (x < clip.x0 || x >= clip.x1 || y < clip.y0 || y >= clip.y1) {
        return;
    }
    fb[y * GFX_WIDTH + x] = color;
}

void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color)
{
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;

    if (x0 < clip.x0) x0 = clip.x0;
    if (y0 < clip.y0) y0 = clip.y0;
    if (x1 > clip.x1) x1 = clip.x1;
    if (y1 > clip.y1) y1 = clip.y1;

    for (int row = y0; row < y1; row++) {
        gfx_color_t *dst = fb + (size_t)row * GFX_WIDTH + x0;
        for (int col = x0; col < x1; col++) {
            *dst++ = color;
        }
    }
}

/*---------------------------------------------------------------------------
 * Text
 *
 * font8x8_basic is public-domain 8x8 bitmap data: one byte per row, and
 * within a row bit 0 is the LEFTMOST pixel.
 *-------------------------------------------------------------------------*/

int gfx_text_width(const char *text, int len)
{
    if (len < 0) {
        len = (int)strlen(text);
    }
    return len * GFX_CHAR_W;
}

int gfx_text_height(void)
{
    return GFX_CHAR_H;
}

void gfx_text(int x, int y, const char *text, gfx_color_t color)
{
    gfx_text_scaled(x, y, text, color, GFX_GLYPH_SCALE);
}

void gfx_text_scaled(int x, int y, const char *text, gfx_color_t color,
                     int scale)
{
    if (scale < 1) {
        scale = 1;
    }

    for (const char *p = text; *p != '\0'; p++, x += 8 * scale) {
        const unsigned char ch = (unsigned char)*p;
        if (ch >= 128) {
            continue;   /* font covers ASCII only */
        }
        const char *glyph = font8x8_basic[ch];

        for (int row = 0; row < 8; row++) {
            const unsigned char bits = (unsigned char)glyph[row];
            if (bits == 0) {
                continue;
            }
            for (int col = 0; col < 8; col++) {
                if (!(bits & (1 << col))) {
                    continue;
                }
                /* One font pixel becomes a scale x scale square. */
                gfx_fill_rect(x + col * scale, y + row * scale,
                              scale, scale, color);
            }
        }
    }
}

/*---------------------------------------------------------------------------
 * Present
 *-------------------------------------------------------------------------*/

void gfx_present(void)
{
    for (int y = 0; y < GFX_HEIGHT; y += STRIP_HEIGHT) {
        esp_lcd_panel_draw_bitmap(panel,
                                  0, y,
                                  GFX_WIDTH, y + STRIP_HEIGHT,
                                  fb + (size_t)y * GFX_WIDTH);
    }

    /* draw_bitmap only QUEUES a DMA transfer that reads out of the
     * framebuffer. Returning before they drain would let the next frame start
     * overwriting memory still being shifted out to the panel. */
    for (int i = 0; i < STRIP_COUNT; i++) {
        xSemaphoreTake(strip_sent, portMAX_DELAY);
    }
}
