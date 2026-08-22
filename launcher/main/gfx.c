#include "gfx.h"

#include <string.h>

#include "driver/spi_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_sh8601.h"
#include "esp_timer.h"
#include "esp_check.h"
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
static esp_lcd_panel_io_handle_t panel_io;
static bool spi_bus_up;
static SemaphoreHandle_t strip_sent;

/* Copied from the Waveshare BSP (Apache-2.0, (c) 2026 Waveshare Team), where
 * it is a private static.
 *
 * We need it because gfx brings the panel up itself rather than calling
 * bsp_display_new(). The BSP offers no way to release the display, and
 * releasing it is the only way to reach the SD card - the two share SPI2 on
 * different pins, so only one can hold the bus. Owning the sequence here is
 * what makes gfx_suspend()/gfx_resume() possible.
 *
 * Note command 0x11 (sleep out) carries a 120 ms settle, which dominates the
 * cost of a full re-initialisation. */
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

/* Current clip rectangle, as inclusive-exclusive bounds. */
static struct { int x0, y0, x1, y1; } clip;

/* One bit per strip: set means "this band changed and must be sent".
 *
 * The panel refreshes from its own GRAM, so anything not sent simply stays on
 * screen. Sending a whole frame costs 9.6 ms of a ~14 ms budget and is pure
 * bus time, so skipping unchanged bands is the largest saving available - see
 * docs/ESP32-C6-AMOLED-Notes.md. STRIP_COUNT is 7, so a byte is plenty. */
static uint8_t strip_dirty;

/* Marks the bands spanned by an ALREADY-CLIPPED row range.
 *
 * Inline and unguarded on purpose. gfx_fill_rect is the workhorse behind text,
 * and gfx_text_scaled calls it once per set font pixel - so this runs thousands
 * of times on a screen of text, and routing that through the public
 * gfx_mark_dirty(), with its re-clipping and its call overhead, cost about 5%
 * of the launcher's framerate. STRIP_HEIGHT is a power of two, so the divisions
 * become shifts. */
static inline void mark_band(int y0, int y1)
{
    if (y0 >= y1) {
        return;
    }
    const int first = y0 / STRIP_HEIGHT;
    const int last  = (y1 - 1) / STRIP_HEIGHT;

    for (int i = first; i <= last; i++) {
        strip_dirty |= (uint8_t)(1u << i);
    }
}

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

/* Brings the panel up on SPI2.
 *
 * `send_init` chooses between a full initialisation and only re-attaching:
 * the SH8601 keeps its registers while powered, so after a suspend the panel
 * is still configured and only the ESP32 side needs rebuilding. Skipping the
 * command sequence avoids its 120 ms settle. */
static esp_err_t panel_bring_up(bool send_init)
{
    const spi_bus_config_t bus = SH8601_PANEL_BUS_QSPI_CONFIG(
        BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2,
        BSP_LCD_DATA3, GFX_WIDTH * STRIP_HEIGHT * sizeof(gfx_color_t));

    esp_err_t err = spi_bus_initialize(BSP_LCD_SPI_NUM, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }
    spi_bus_up = true;

    esp_lcd_panel_io_spi_config_t io_config =
        SH8601_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, on_strip_sent, NULL);

    /* The macro defaults to 40 MHz, which is what the vendor driver validates.
     * gfx_present() measured 17.6 ms at that rate against a theoretical 16.5,
     * so the frame is 94% bus-bound - the clock is the whole cost, and doubling
     * it is the only change that halves it. See GFX_QSPI_HZ in gfx.h. */
    io_config.pclk_hz = GFX_QSPI_HZ;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM,
                                   &io_config, &panel_io);
    if (err != ESP_OK) {
        return err;
    }

    sh8601_vendor_config_t vendor = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,   /* reset is on the IO expander */
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor,
    };
    err = esp_lcd_new_panel_sh8601(panel_io, &panel_config, &panel);
    if (err != ESP_OK) {
        return err;
    }

    if (send_init) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "on");
    }
    return ESP_OK;
}

/* Releases SPI2 so something else can use it. The framebuffer is kept: it is
 * ordinary RAM and has nothing to do with the bus. */
static void panel_tear_down(void)
{
    if (panel != NULL) {
        esp_lcd_panel_del(panel);
        panel = NULL;
    }
    if (panel_io != NULL) {
        esp_lcd_panel_io_del(panel_io);
        panel_io = NULL;
    }
    if (spi_bus_up) {
        spi_bus_free(BSP_LCD_SPI_NUM);
        spi_bus_up = false;
    }
}

bool gfx_suspend(void)
{
    panel_tear_down();
    return true;
}

bool gfx_resume(bool full_init)
{
    /* A full re-initialisation reloads the panel's registers and can leave its
     * GRAM in an unknown state, so nothing may be assumed to still be on
     * screen. Cheap insurance: one full frame after a resume. */
    if (full_init) {
        gfx_mark_all_dirty();
    }
    return panel_bring_up(full_init) == ESP_OK;
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

    /* Detect the board first: it initialises I2C and pulses the display and
     * touch reset lines through the IO expander, which the panel needs before
     * it will accept anything. */
    if (bsp_board_detect() == BSP_BOARD_VARIANT_UNKNOWN) {
        ESP_LOGE(TAG, "Could not identify the board");
        return false;
    }

    if (panel_bring_up(true) != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the display");
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
    gfx_mark_all_dirty();

    ESP_LOGI(TAG, "%dx%d framebuffer, %u bytes, %u bytes of heap still free",
             GFX_WIDTH, GFX_HEIGHT, (unsigned)bytes,
             (unsigned)esp_get_free_heap_size());
    return true;
}

gfx_color_t *gfx_framebuffer(void)
{
    /* Deliberately does NOT mark anything dirty. gfx cannot know what a caller
     * intends to write, and guessing "everything" would silently discard the
     * saving for the two apps that draw this way. Raw writers call
     * gfx_mark_dirty() themselves; see the warning on it. */
    return fb;
}

/*---------------------------------------------------------------------------
 * Dirty tracking
 *-------------------------------------------------------------------------*/

void gfx_mark_all_dirty(void)
{
    strip_dirty = (uint8_t)((1u << STRIP_COUNT) - 1u);
}

void gfx_mark_dirty(int x, int y, int w, int h)
{
    (void)x;
    (void)w;   /* strips are full width, so only the rows matter */

    int y0 = y;
    int y1 = y + h;

    if (y0 < 0) y0 = 0;
    if (y1 > GFX_HEIGHT) y1 = GFX_HEIGHT;
    if (y0 >= y1) {
        return;
    }

    mark_band(y0, y1);
}

bool gfx_region_dirty(int x, int y, int w, int h)
{
    (void)x;
    (void)w;

    int y0 = y;
    int y1 = y + h;

    if (y0 < 0) y0 = 0;
    if (y1 > GFX_HEIGHT) y1 = GFX_HEIGHT;
    if (y0 >= y1) {
        return false;
    }

    const int first = y0 / STRIP_HEIGHT;
    const int last  = (y1 - 1) / STRIP_HEIGHT;

    for (int i = first; i <= last; i++) {
        if (strip_dirty & (1u << i)) {
            return true;
        }
    }
    return false;
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

    /* Marking the whole screen here is what keeps every existing app working
     * unchanged: the cube, the launcher and the POST report all clear before
     * drawing, so they mark everything without knowing dirty tracking exists. */
    gfx_mark_all_dirty();
}

void gfx_pixel(int x, int y, gfx_color_t color)
{
    if (x < clip.x0 || x >= clip.x1 || y < clip.y0 || y >= clip.y1) {
        return;
    }
    fb[y * GFX_WIDTH + x] = color;
    strip_dirty |= (uint8_t)(1u << (y / STRIP_HEIGHT));
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

    mark_band(y0, y1);   /* already clipped above */
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
    int queued = 0;

    for (int i = 0; i < STRIP_COUNT; i++) {
        if ((strip_dirty & (1u << i)) == 0) {
            continue;   /* unchanged - the panel is still showing it */
        }
        const int y = i * STRIP_HEIGHT;
        esp_lcd_panel_draw_bitmap(panel,
                                  0, y,
                                  GFX_WIDTH, y + STRIP_HEIGHT,
                                  fb + (size_t)y * GFX_WIDTH);
        queued++;
    }

    strip_dirty = 0;

    /* draw_bitmap only QUEUES a DMA transfer that reads out of the
     * framebuffer. Returning before they drain would let the next frame start
     * overwriting memory still being shifted out to the panel. Wait for
     * exactly as many as were queued - waiting for STRIP_COUNT would block
     * for ever the moment any strip was skipped. */
    for (int i = 0; i < queued; i++) {
        xSemaphoreTake(strip_sent, portMAX_DELAY);
    }
}
