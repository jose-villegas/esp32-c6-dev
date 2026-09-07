#include "gfx/gfx.h"
#include "gfx/gfx_dirty.h"
#include "gfx/gfx_font_roles.h"
#include "util/intmath.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
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
#endif

/* Carries GFX_DIRTY_WIDTH/HEIGHT for ESP-IDF independence and aligns with
 * gfx.h's BSP values. */
_Static_assert(GFX_WIDTH == GFX_DIRTY_WIDTH && GFX_HEIGHT == GFX_DIRTY_HEIGHT,
              "gfx_dirty.h's screen dimensions must match gfx.h's");

#ifdef ESP_PLATFORM
static const char *TAG = "gfx";
#endif

static gfx_color_t *fb;

#ifdef ESP_PLATFORM
static esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t panel_io;
static bool spi_bus_up;
static SemaphoreHandle_t strip_sent;

/* Copied from the Waveshare BSP (Apache-2.0, (c) 2026 Waveshare Team),
 * where it is a private static - needed here because gfx brings the
 * panel up itself rather than calling bsp_display_new(): the BSP offers
 * no way to release the display, and releasing it is the only way to
 * reach the SD card (shared SPI2, only one bus owner at a time), which
 * is what makes gfx_suspend()/gfx_resume() possible. Command 0x11 (sleep
 * out) carries a 120 ms settle, dominating a full re-init's cost. */
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
#endif

/* Current clip rectangle, as inclusive-exclusive bounds. */
static struct { int x0, y0, x1, y1; } clip;

#ifdef ESP_PLATFORM
/* Scratch space for gather_and_send(), bounded by GATHER_MAX_PIXELS,
 * allocated with MALLOC_CAP_DMA. Misalignment causes DMA errors. */
static gfx_color_t *gather_buf;
#endif

/*---------------------------------------------------------------------------
 * Panel plumbing - device-only. A host build never brings a panel up or
 * presents to one; see gfx_init()/gfx_suspend()/gfx_resume()/gfx_present()
 * below for the host side of each.
 *-------------------------------------------------------------------------*/

#ifdef ESP_PLATFORM
static bool IRAM_ATTR on_strip_sent(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *event,
                                    void *user_context)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(strip_sent, &woken);
    return woken == pdTRUE;
}

/* SPI2 panel. `send_init` chooses full init or re-attach, skipping command
 * sequence to avoid 120 ms wait. */
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

    /* Defaults to 40 MHz, 17.6 ms frame, 94% bus-bound. See GFX_QSPI_HZ. */
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

/* Releases SPI2 for other use. Framebuffer is ordinary RAM, not bus-related. */
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
#endif   /* ESP_PLATFORM - panel plumbing */

bool gfx_suspend(void)
{
#ifdef ESP_PLATFORM
    panel_tear_down();
#endif
    return true;
}

bool gfx_resume(bool full_init)
{
#ifdef ESP_PLATFORM
    /* GRAM unknown after re-init; assume screen cleared. One frame after
     * resume. */
    if (full_init) {
        gfx_mark_all_dirty();
    }
    return panel_bring_up(full_init) == ESP_OK;
#else
    (void)full_init;
    return true;
#endif
}

bool gfx_init(void)
{
#ifdef ESP_PLATFORM
    /* Sized for STRIP_COUNT * GRID_COLS: see send_one_row(). Undersizing
     * blocks gfx_present() forever. */
    strip_sent = xSemaphoreCreateCounting(
        STRIP_COUNT * GRID_COLS + 2, 0);
    if (strip_sent == NULL) {
        ESP_LOGE(TAG, "Could not create the strip-transfer semaphore");
        return false;
    }

    /* Detect board: initialises I2C, pulses display and touch reset lines. */
    if (bsp_board_detect() == BSP_BOARD_VARIANT_UNKNOWN) {
        ESP_LOGE(TAG, "Could not identify the board");
        return false;
    }

    if (panel_bring_up(true) != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the display");
        return false;
    }

#if CONFIG_LAUNCHER_DEVELOPMENT
    /* Framebuffer state post SD probe & panel bring-up; paired with HEAPMARK
     * in main.c. See heap_mark() comment and bd esp32c6-8h2. */
    ESP_LOGI(TAG, "HEAPMARK %-18s free %6u largest %6u", "before framebuffer",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
#endif

    const size_t bytes = (size_t)GFX_WIDTH * GFX_HEIGHT * sizeof(gfx_color_t);
    fb = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (fb == NULL) {
        ESP_LOGE(TAG, "Could not allocate %u byte framebuffer "
                      "(largest free DMA block is %u bytes)",
                 (unsigned)bytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        return false;
    }

    const size_t gather_bytes = (size_t)GATHER_MAX_PIXELS * sizeof(gfx_color_t);
    gather_buf = heap_caps_malloc(gather_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (gather_buf == NULL) {
        ESP_LOGE(TAG, "Could not allocate %u byte gather buffer",
                 (unsigned)gather_bytes);
        return false;
    }

    gfx_clear_clip();
    gfx_mark_all_dirty();

    ESP_LOGI(TAG, "%dx%d framebuffer at %p, %u bytes; heap free %u, "
                  "largest DMA block %u",
             GFX_WIDTH, GFX_HEIGHT, (void *)fb, (unsigned)bytes,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    return true;
#else
    const size_t bytes = (size_t)GFX_WIDTH * GFX_HEIGHT * sizeof(gfx_color_t);
    fb = malloc(bytes);
    if (fb == NULL) {
        return false;
    }
    gfx_clear_clip();
    gfx_mark_all_dirty();
    return true;
#endif
}

gfx_color_t *gfx_framebuffer(void)
{
    /* gfx can't guess intent. "Everything" wastes resources. Raw writers use
     * gfx_mark_dirty(). Be cautious. */
    return fb;
}

/*---------------------------------------------------------------------------
 * Dirty tracking
 *-------------------------------------------------------------------------*/

static bool partial_clear_on;
static bool interlace_on;
static int frame_parity;
static bool prev_bbox_valid;
static int prev_bbox_x0, prev_bbox_y0, prev_bbox_x1, prev_bbox_y1;
static bool drawn_bbox_valid;
static int drawn_bbox_x0, drawn_bbox_y0, drawn_bbox_x1, drawn_bbox_y1;

void gfx_set_partial_clear(bool on)
{
    if (!on) {
        prev_bbox_valid = false;
    }
    partial_clear_on = on;
}

bool gfx_partial_clear_enabled(void)
{
    return partial_clear_on;
}

void gfx_set_interlace(bool on)
{
    interlace_on = on;
}

bool gfx_interlace_enabled(void)
{
    return interlace_on;
}

void gfx_invalidate(void)
{
    prev_bbox_valid = false;
}

/* gfx_dirty.h header-only for inlining mark_band(); thin wrappers for gfx.h
 * API. */
void gfx_mark_all_dirty(void)
{
    dirty_mark_all();
    drawn_bbox_valid = false;
    prev_bbox_valid = false;
}

void gfx_mark_dirty(int x, int y, int w, int h)
{
    dirty_mark(x, y, w, h);

    if (w <= 0 || h <= 0) {
        return;
    }
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > GFX_WIDTH) x1 = GFX_WIDTH;
    if (y1 > GFX_HEIGHT) y1 = GFX_HEIGHT;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    if (drawn_bbox_valid) {
        if (x0 < drawn_bbox_x0) drawn_bbox_x0 = x0;
        if (y0 < drawn_bbox_y0) drawn_bbox_y0 = y0;
        if (x1 > drawn_bbox_x1) drawn_bbox_x1 = x1;
        if (y1 > drawn_bbox_y1) drawn_bbox_y1 = y1;
    } else {
        drawn_bbox_x0 = x0;
        drawn_bbox_y0 = y0;
        drawn_bbox_x1 = x1;
        drawn_bbox_y1 = y1;
        drawn_bbox_valid = true;
    }
}

bool gfx_region_dirty(int x, int y, int w, int h)
{
    (void)x; (void)w;
    return dirty_region_dirty(y, h);
}

/*---------------------------------------------------------------------------
 * Colour
 *-------------------------------------------------------------------------*/

/* Pack 0xRRGGBB to RGB565, byte-swapped. QSPI needs high and low bytes
 * swapped, but LVGL's port doesn't handle it. */
gfx_color_t gfx_rgb(uint32_t rgb)
{
    return GFX_RGB(rgb);
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

/* Ignores clip rect; clears whole-screen or bounding box; marks box dirty. */
void gfx_clear(gfx_color_t color)
{
    if (partial_clear_on && prev_bbox_valid) {
        for (int y = prev_bbox_y0; y < prev_bbox_y1; y++) {
            gfx_color_t *dst = fb + (size_t)y * GFX_WIDTH + prev_bbox_x0;
            for (int x = prev_bbox_x0; x < prev_bbox_x1; x++) {
                *dst++ = color;
            }
        }
        dirty_mark(prev_bbox_x0, prev_bbox_y0,
                   prev_bbox_x1 - prev_bbox_x0,
                   prev_bbox_y1 - prev_bbox_y0);
        drawn_bbox_valid = false;
        return;
    }

    const uint32_t pair = ((uint32_t)color << 16) | color;
    uint32_t *words = (uint32_t *)fb;
    const int count = (GFX_WIDTH * GFX_HEIGHT) / 2;

    for (int i = 0; i < count; i++) {
        words[i] = pair;
    }

    gfx_mark_all_dirty();
}

void gfx_pixel(int x, int y, gfx_color_t color)
{
    if (x < clip.x0 || x >= clip.x1 || y < clip.y0 || y >= clip.y1) {
        return;
    }
    fb[y * GFX_WIDTH + x] = color;
    mark_band(y, y + 1);
}

/* Cohen-Sutherland outcodes: one bit per edge the point lies outside of. */
enum { OUT_LEFT = 1, OUT_RIGHT = 2, OUT_TOP = 4, OUT_BOTTOM = 8 };

static int outcode(int x, int y)
{
    int code = 0;
    if (x < clip.x0)       { code |= OUT_LEFT; }
    else if (x >= clip.x1) { code |= OUT_RIGHT; }
    if (y < clip.y0)       { code |= OUT_TOP; }
    else if (y >= clip.y1) { code |= OUT_BOTTOM; }
    return code;
}

/* Clipping affects error term, differs by pixel. Caller takes fast path if
 * both ends inside. */
static bool clip_line(int *x0, int *y0, int *x1, int *y1)
{
    int c0 = outcode(*x0, *y0);
    int c1 = outcode(*x1, *y1);

    for (int pass = 0; pass < 8; pass++) {
        if ((c0 | c1) == 0) {
            return true;              /* both ends inside */
        }
        if ((c0 & c1) != 0) {
            return false;             /* both beyond the same edge */
        }

        const int out = c0 ? c0 : c1;
        int x, y;

        /* Clips to last pixel inside, not boundary. */
        if (out & OUT_BOTTOM) {
            y = clip.y1 - 1;
            x = *x0 + (int)(((int64_t)(*x1 - *x0) * (y - *y0)) / (*y1 - *y0));
        } else if (out & OUT_TOP) {
            y = clip.y0;
            x = *x0 + (int)(((int64_t)(*x1 - *x0) * (y - *y0)) / (*y1 - *y0));
        } else if (out & OUT_RIGHT) {
            x = clip.x1 - 1;
            y = *y0 + (int)(((int64_t)(*y1 - *y0) * (x - *x0)) / (*x1 - *x0));
        } else {
            x = clip.x0;
            y = *y0 + (int)(((int64_t)(*y1 - *y0) * (x - *x0)) / (*x1 - *x0));
        }

        if (out == c0) {
            *x0 = x; *y0 = y; c0 = outcode(x, y);
        } else {
            *x1 = x; *y1 = y; c1 = outcode(x, y);
        }
    }
    return false;
}

/* One pixel of a line. */
static void plot(int x, int y, gfx_color_t color, unsigned flags)
{
    if (x < clip.x0 || x >= clip.x1 || y < clip.y0 || y >= clip.y1) {
        return;
    }
    gfx_color_t *const dst = &fb[(size_t)y * GFX_WIDTH + x];

    *dst = (flags & GFX_LINE_ADD) ? gfx_color_add(*dst, color) : color;
}

/* Bresenham, treats both axes alike, no case analysis. */
static void walk(int x0, int y0, int x1, int y1, gfx_color_t color,
                 unsigned flags)
{
    const int dx = im_abs(x1 - x0);
    const int dy = -im_abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    bool first = true;

    for (;;) {
        if (!(first && (flags & GFX_LINE_OPEN))) {
            plot(x0, y0, color, flags);
        }
        first = false;

        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Box intersects clip, marked once. Overestimating costs bus time;
 * underestimating leaves stale. */
static void draw_line(int x0, int y0, int x1, int y1, gfx_color_t color,
                      unsigned flags)
{
    /* Only pay for clipping when some of the line is actually outside. */
    if (outcode(x0, y0) | outcode(x1, y1)) {
        if (!clip_line(&x0, &y0, &x1, &y1)) {
            return;
        }
    }

    int bx0 = im_min(x0, x1), bx1 = im_max(x0, x1) + 1;
    int by0 = im_min(y0, y1), by1 = im_max(y0, y1) + 1;

    if (bx0 < clip.x0) bx0 = clip.x0;
    if (by0 < clip.y0) by0 = clip.y0;
    if (bx1 > clip.x1) bx1 = clip.x1;
    if (by1 > clip.y1) by1 = clip.y1;

    walk(x0, y0, x1, y1, color, flags);

    if (bx0 < bx1 && by0 < by1) {
        dirty_mark(bx0, by0, bx1 - bx0, by1 - by0);
    }
}

void gfx_line(int x0, int y0, int x1, int y1, gfx_color_t color)
{
    draw_line(x0, y0, x1, y1, color, 0);
}

void gfx_line_ex(int x0, int y0, int x1, int y1, gfx_color_t color,
                 unsigned flags)
{
    draw_line(x0, y0, x1, y1, color, flags);
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
 * Dithered fake transparency
 *
 * gfx_fill_rect_blend() (further down this file) is a REAL per-pixel blend,
 * but it pays for a framebuffer read - affordable at glyph scale, not
 * across a whole frame (see its own comment for why). Dithering is the
 * other way to fake transparency, and the classic one, for exactly the
 * cases a real blend is too expensive for: ordered (Bayer) dithering picks
 * WHICH pixels to draw, not how to blend the ones it does. No extra
 * framebuffer read, no float math, just a per-pixel threshold test against
 * a small fixed table - practically free next to a real blend, which is
 * exactly why 8-bit consoles used it for shadows and water decades before
 * this chip's own class of hardware could afford anything better.
 *---------------------------------------------------------------------------*/

/* gfx_fill_rect() uses `alpha` (0-255) for coverage, avoiding framebuffer
 * reads. gfx_dither_covers() in gfx_color.h. Returns if 0. */
void gfx_fill_rect_dither(int x, int y, int w, int h, gfx_color_t color,
                          uint8_t alpha)
{
    if (alpha == 0) {
        return;
    }

    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;

    if (x0 < clip.x0) x0 = clip.x0;
    if (y0 < clip.y0) y0 = clip.y0;
    if (x1 > clip.x1) x1 = clip.x1;
    if (y1 > clip.y1) y1 = clip.y1;

    for (int row = y0; row < y1; row++) {
        gfx_color_t *dst = fb + (size_t)row * GFX_WIDTH;
        for (int col = x0; col < x1; col++) {
            if (gfx_dither_covers(col, row, alpha)) {
                dst[col] = color;
            }
        }
    }

    mark_band(y0, y1);
}

/* Per-pixel blend, reads framebuffer. Efficient for glyphs, not full-frame.
 * Alpha 0 no-op, 255 matches gfx_fill_rect(). */
void gfx_fill_rect_blend(int x, int y, int w, int h, gfx_color_t color,
                         uint8_t alpha)
{
    if (alpha == 0) {
        return;
    }

    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;

    if (x0 < clip.x0) x0 = clip.x0;
    if (y0 < clip.y0) y0 = clip.y0;
    if (x1 > clip.x1) x1 = clip.x1;
    if (y1 > clip.y1) y1 = clip.y1;

    for (int row = y0; row < y1; row++) {
        gfx_color_t *dst = fb + (size_t)row * GFX_WIDTH;
        for (int col = x0; col < x1; col++) {
            dst[col] = gfx_color_mix(dst[col], color, alpha);
        }
    }

    mark_band(y0, y1);
}

/* Cheap by construction, not by luck: alpha is one value for the whole
 * call, and the Bayer pattern repeats every 4 pixels, so the per-pixel
 * decision collapses to four booleans per row - a fully-covered row is a
 * plain memcpy, an untouched row costs nothing. Phase-locked to absolute
 * panel coordinates like every other dithered draw in gfx.h, so
 * overlapping dithered shapes stay in register with each other. First
 * user: the boot animation's photograph crossfade (boot_anim.c's
 * draw_image()). */
void gfx_blit_dither(int x, int y, int w, int h, const gfx_color_t *src,
                     int src_stride, uint8_t alpha)
{
    if (alpha == 0) {
        return;
    }

    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;

    if (x0 < clip.x0) x0 = clip.x0;
    if (y0 < clip.y0) y0 = clip.y0;
    if (x1 > clip.x1) x1 = clip.x1;
    if (y1 > clip.y1) y1 = clip.y1;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    const int level = gfx_dither_level(alpha);

    for (int row = y0; row < y1; row++) {
        const uint8_t *cells = gfx_dither4x4[row & 3];
        const bool p[4] = { level > cells[0], level > cells[1],
                            level > cells[2], level > cells[3] };

        if (!p[0] && !p[1] && !p[2] && !p[3]) {
            continue;
        }

        gfx_color_t *dst = fb + (size_t)row * GFX_WIDTH;
        const gfx_color_t *s =
            src + (size_t)(row - y) * (size_t)src_stride + (x0 - x);

        if (p[0] && p[1] && p[2] && p[3]) {
            memcpy(dst + x0, s, (size_t)(x1 - x0) * sizeof *dst);
            continue;
        }

        int col = x0;
        gfx_color_t *dp = dst + col;
        const gfx_color_t *sp = s;
        for (; col < x1 && (col & 3) != 0; col++, dp++, sp++) {
            if (p[col & 3]) {
                *dp = *sp;
            }
        }
        for (; col + 4 <= x1; col += 4, dp += 4, sp += 4) {
            if (p[0]) dp[0] = sp[0];
            if (p[1]) dp[1] = sp[1];
            if (p[2]) dp[2] = sp[2];
            if (p[3]) dp[3] = sp[3];
        }
        for (; col < x1; col++, dp++, sp++) {
            if (p[col & 3]) {
                *dp = *sp;
            }
        }
    }

    mark_band(y0, y1);
}

/*---------------------------------------------------------------------------
 * Text
 *
 * One font-aware path (gfx_text_font(), gfx_font_width()) that everything
 * else here delegates to, passing gfx_font_ui() - see gfx_font.h for what a
 * gfx_font_t is and why it exists: honouring microui's mu_Font is a later
 * task, in files this one steers clear of, and that task needs a font to
 * point AT. gfx_font_ui() (gfx_font_roles.h) wraps font8x8_basic.h's data,
 * which is public-domain 8x8 bitmap data: one byte per row, and within a
 * row bit 0 is the LEFTMOST pixel - see gfx_font_8x8's own comment in
 * gfx_font.h.
 *
 * This used to be its own function here, gfx_default_font(), returning
 * &gfx_font_8x8 directly - the one entry the scheme had before more than
 * one role existed. It is retired in favour of gfx_font_ui(): "gfx's own
 * built-in default" and "the font the UI role names" were always the same
 * question asked twice, and every caller (ui.c, boot_anim.c, this file)
 * wanted the same answer either way, so there is no longer a reason to
 * make a caller pick which of two names to spell. */

int gfx_font_width(const gfx_font_t *font, const char *text, int len,
                   int scale)
{
    return gfx_font_text_width(font, text, len, scale);
}

int gfx_text_width(const char *text, int len)
{
    return gfx_font_width(gfx_font_ui(), text, len, GFX_GLYPH_SCALE);
}

int gfx_text_height(void)
{
    return gfx_font_height(gfx_font_ui(), GFX_GLYPH_SCALE);
}

void gfx_text(int x, int y, const char *text, gfx_color_t color)
{
    gfx_text_scaled(x, y, text, color, GFX_GLYPH_SCALE);
}

void gfx_text_scaled(int x, int y, const char *text, gfx_color_t color,
                     int scale)
{
    gfx_text_turned(x, y, text, color, scale, 0);
}

/* Generalised to variable cell size. 1bpp path only for 8x8. Used by 8bpp
 * path too. */
static void draw_rotated_font_pixel(const gfx_font_t *font, int x, int y,
                                    int row, int col, int scale, int turn,
                                    gfx_color_t color)
{
    int px, py;
    switch (turn) {
    case 1:  px = font->cell_h - 1 - row; py = col;                   break;
    case 2:  px = font->cell_w - 1 - col; py = font->cell_h - 1 - row; break;
    case 3:  px = row;                    py = font->cell_w - 1 - col; break;
    default: px = col;                    py = row;                   break;
    }
    gfx_fill_rect(x + px * scale, y + py * scale, scale, scale, color);
}

/* 8bpp atlas; 0-255 coverage; blends via gfx_fill_rect_blend() instead of
 * solid. */
static void draw_rotated_font_pixel_blend(const gfx_font_t *font, int x,
                                          int y, int row, int col, int scale,
                                          int turn, gfx_color_t color,
                                          uint8_t coverage)
{
    int px, py;
    switch (turn) {
    case 1:  px = font->cell_h - 1 - row; py = col;                   break;
    case 2:  px = font->cell_w - 1 - col; py = font->cell_h - 1 - row; break;
    case 3:  px = row;                    py = font->cell_w - 1 - col; break;
    default: px = col;                    py = row;                   break;
    }
    gfx_fill_rect_blend(x + px * scale, y + py * scale, scale, scale, color,
                        coverage);
}

/* Draws `font` glyph or nothing if `ch` is out of range or `font->bpp`
 * unsupported. Use separate loops for layouts. */
static void draw_glyph_font(const gfx_font_t *font, int x, int y,
                            unsigned char ch, gfx_color_t color, int scale,
                            int turn)
{
    if (ch < font->first || (unsigned)(ch - font->first) >= font->count) {
        return;
    }

    if (font->bpp == 1) {
        const uint8_t *glyph =
            font->atlas + (size_t)(ch - font->first) * font->cell_h;

        for (int row = 0; row < font->cell_h; row++) {
            const uint8_t bits = glyph[row];
            if (bits == 0) {
                continue;
            }
            for (int col = 0; col < font->cell_w; col++) {
                if (bits & (1 << col)) {
                    draw_rotated_font_pixel(font, x, y, row, col, scale, turn, color);
                }
            }
        }
        return;
    }

    if (font->bpp == 8) {
        const size_t cell_pixels = (size_t)font->cell_w * font->cell_h;
        const uint8_t *glyph =
            font->atlas + (size_t)(ch - font->first) * cell_pixels;

        for (int row = 0; row < font->cell_h; row++) {
            const uint8_t *glyph_row = glyph + (size_t)row * font->cell_w;
            for (int col = 0; col < font->cell_w; col++) {
                const uint8_t coverage = glyph_row[col];
                if (coverage == 0) {
                    continue;
                }
                draw_rotated_font_pixel_blend(font, x, y, row, col, scale,
                                              turn, color, coverage);
            }
        }
        return;
    }
}

void gfx_text_font(int x, int y, const char *text, gfx_color_t color,
                   int scale, int quarter_turns, const gfx_font_t *font)
{
    if (scale < 1) {
        scale = 1;
    }

    const int turn = ((quarter_turns % 4) + 4) % 4;

    static const int step[4][2] = {
        {  1,  0 },   /* upright:        left to right */
        {  0,  1 },   /* quarter turn:   top to bottom */
        { -1,  0 },   /* upside down:    right to left */
        {  0, -1 },   /* three quarters: bottom to top */
    };

    for (const char *p = text; *p != '\0'; p++) {
        const unsigned char ch = (unsigned char)*p;
        draw_glyph_font(font, x, y, ch, color, scale, turn);
        const int adv = gfx_font_advance(font, ch, scale);
        x += step[turn][0] * adv;
        y += step[turn][1] * adv;
    }
}

void gfx_text_turned(int x, int y, const char *text, gfx_color_t color,
                     int scale, int quarter_turns)
{
    gfx_text_font(x, y, text, color, scale, quarter_turns, gfx_font_ui());
}

/*---------------------------------------------------------------------------
 * Dithered text
 *
 * A second, complete copy of draw_rotated_font_pixel()/draw_glyph_font()/
 * gfx_text_font() rather than one shared core threaded with an alpha
 * parameter - on purpose: gfx_text_font() is the single font-aware path
 * every other text call in this file, and every caller of it in the whole
 * tree, already goes through, and it is worth that path staying exactly
 * the code it was before dithering existed, provably unable to regress
 * from this addition, rather than trusting a compiler to fold an
 * `alpha == 255` check back out of it at every call site forever. The
 * duplication is small (three short functions) and it buys that
 * guarantee outright instead of by inspection.
 *---------------------------------------------------------------------------*/

static void draw_rotated_font_pixel_dither(const gfx_font_t *font, int x,
                                           int y, int row, int col,
                                           int scale, int turn,
                                           gfx_color_t color, uint8_t alpha)
{
    int px, py;
    switch (turn) {
    case 1:  px = font->cell_h - 1 - row; py = col;                   break;
    case 2:  px = font->cell_w - 1 - col; py = font->cell_h - 1 - row; break;
    case 3:  px = row;                    py = font->cell_w - 1 - col; break;
    default: px = col;                    py = row;                   break;
    }
    gfx_fill_rect_dither(x + px * scale, y + py * scale, scale, scale,
                         color, alpha);
}

static void draw_glyph_font_dither(const gfx_font_t *font, int x, int y,
                                   unsigned char ch, gfx_color_t color,
                                   int scale, int turn, uint8_t alpha)
{
    if (ch < font->first || (unsigned)(ch - font->first) >= font->count) {
        return;
    }

    if (font->bpp == 1) {
        const uint8_t *glyph =
            font->atlas + (size_t)(ch - font->first) * font->cell_h;

        for (int row = 0; row < font->cell_h; row++) {
            const uint8_t bits = glyph[row];
            if (bits == 0) {
                continue;
            }
            for (int col = 0; col < font->cell_w; col++) {
                if (bits & (1 << col)) {
                    draw_rotated_font_pixel_dither(font, x, y, row, col, scale,
                                                   turn, color, alpha);
                }
            }
        }
        return;
    }

    if (font->bpp == 8) {
        const size_t cell_pixels = (size_t)font->cell_w * font->cell_h;
        const uint8_t *glyph =
            font->atlas + (size_t)(ch - font->first) * cell_pixels;

        for (int row = 0; row < font->cell_h; row++) {
            const uint8_t *glyph_row = glyph + (size_t)row * font->cell_w;
            for (int col = 0; col < font->cell_w; col++) {
                const uint8_t coverage = glyph_row[col];
                if (coverage == 0) {
                    continue;
                }
                const uint8_t folded = coverage < alpha ? coverage : alpha;
                draw_rotated_font_pixel_dither(font, x, y, row, col, scale,
                                               turn, color, folded);
            }
        }
        return;
    }
}

/* gfx_text_font() with dithered glyphs for translucent effect. Used in
 * boot_anim.c for title shadow. */
void gfx_text_font_dither(int x, int y, const char *text, gfx_color_t color,
                          int scale, int quarter_turns,
                          const gfx_font_t *font, uint8_t alpha)
{
    if (scale < 1) {
        scale = 1;
    }

    const int turn = ((quarter_turns % 4) + 4) % 4;

    static const int step[4][2] = {
        {  1,  0 },
        {  0,  1 },
        { -1,  0 },
        {  0, -1 },
    };

    for (const char *p = text; *p != '\0'; p++) {
        const unsigned char ch = (unsigned char)*p;
        draw_glyph_font_dither(font, x, y, ch, color, scale, turn, alpha);
        const int adv = gfx_font_advance(font, ch, scale);
        x += step[turn][0] * adv;
        y += step[turn][1] * adv;
    }
}

/*---------------------------------------------------------------------------
 * Present
 *-------------------------------------------------------------------------*/

#if CONFIG_LAUNCHER_DEVELOPMENT
/* See gfx.h for "why not always compiled". Used by gfx_set_debug_overlay()
 * below. */
static bool debug_overlay_on;
bool gfx_debug_overlay(void) { return debug_overlay_on; }

static bool leaf_overlay_on;
bool gfx_debug_leaf_overlay(void) { return leaf_overlay_on; }

static inline bool overlay_any_on(void) { return debug_overlay_on || leaf_overlay_on; }

/* See send_partial_band() for third path. Exists for device test. Not reset
 * by gfx_present(). */
static int dev_strips_sent_full;
static int dev_strips_sent_gathered;
static int dev_strips_sent_partial;

void gfx_reset_strip_send_counts(void)
{
    dev_strips_sent_full = 0;
    dev_strips_sent_gathered = 0;
    dev_strips_sent_partial = 0;
}

void gfx_get_strip_send_counts(int *full_bands, int *gathered,
                               int *partial_bands)
{
    if (full_bands)     { *full_bands     = dev_strips_sent_full; }
    if (gathered)       { *gathered       = dev_strips_sent_gathered; }
    if (partial_bands)  { *partial_bands  = dev_strips_sent_partial; }
}

static void mark_rect_border(gfx_color_t *buf, int stride, int w, int h,
                             gfx_color_t colour)
{
    for (int col = 0; col < w; col++) {
        buf[col] = colour;
        buf[(size_t)(h - 1) * stride + col] = colour;
    }
    for (int row = 0; row < h; row++) {
        buf[(size_t)row * stride] = colour;
        buf[(size_t)row * stride + (w - 1)] = colour;
    }
}

/* Used for full-width send border. No scratch copy - direct to fb. Inverse
 * save/restore. */
#define BORDER_PIXELS (2 * (COL_WIDTH + STRIP_HEIGHT))

#define LEAF_BORDER_PIXELS (2 * (LEAF_W + LEAF_H))

static void save_border(const gfx_color_t *buf, int stride, int w, int h,
                        gfx_color_t *out)
{
    int i = 0;
    for (int col = 0; col < w; col++) {
        out[i++] = buf[col];
        out[i++] = buf[(size_t)(h - 1) * stride + col];
    }
    for (int row = 0; row < h; row++) {
        out[i++] = buf[(size_t)row * stride];
        out[i++] = buf[(size_t)row * stride + (w - 1)];
    }
}

static void restore_border(gfx_color_t *buf, int stride, int w, int h,
                           const gfx_color_t *saved)
{
    int i = 0;
    for (int col = 0; col < w; col++) {
        buf[col] = saved[i++];
        buf[(size_t)(h - 1) * stride + col] = saved[i++];
    }
    for (int row = 0; row < h; row++) {
        buf[(size_t)row * stride] = saved[i++];
        buf[(size_t)row * stride + (w - 1)] = saved[i++];
    }
}

/* 6240 px combined, borrowed from gather_buf's front rather than
 * malloc'd separately: two separate buffers once cost sand its needed
 * contiguous allocation, and this debug overlay must never be why sand
 * cannot start. gather_buf is provably idle here: gather_and_send()
 * always waits for its own transfer to finish before returning, and the
 * frame loop is single-threaded. The _Static_assert ties this to
 * GATHER_MAX_PIXELS so a size change that breaks the fit is a compile
 * error, not a silent DMA overflow. */
#define OVERLAY_CELL_SAVE_PIXELS (GRID_COLS * BORDER_PIXELS)
#define OVERLAY_LEAF_SAVE_PIXELS (LEAF_RECTS_PER_ROW_MAX * LEAF_BORDER_PIXELS)
_Static_assert(OVERLAY_CELL_SAVE_PIXELS + OVERLAY_LEAF_SAVE_PIXELS <=
              GATHER_MAX_PIXELS,
              "overlay save/restore scratch must fit inside gather_buf");

static inline gfx_color_t (*overlay_cell_save(void))[BORDER_PIXELS]
{
    return (gfx_color_t (*)[BORDER_PIXELS])gather_buf;
}

static inline gfx_color_t (*overlay_leaf_save(void))[LEAF_BORDER_PIXELS]
{
    return (gfx_color_t (*)[LEAF_BORDER_PIXELS])
        (gather_buf + OVERLAY_CELL_SAVE_PIXELS);
}

/* Shared by send_full_row() and gather_and_send(), never live at once:
 * the frame loop is single-threaded. Unlike the cell/leaf scratch above,
 * this cannot borrow gather_buf - which IS the destination leaf borders
 * draw into using this list's rects, so it must survive alongside
 * gather_buf, not overlap it. Malloc'd on enable/disable (1024 bytes)
 * rather than static: too big for app_main()'s stack, and a permanent
 * .bss reservation fares no better given this repo's history of
 * static-growth OOMs. */
static dirty_leaf_rect_t *leaf_rect_scratch;

void gfx_set_debug_overlay(bool on)
{
    debug_overlay_on = on;
}

void gfx_set_leaf_overlay(bool on)
{
    if (on) {
        if (leaf_rect_scratch == NULL) {
            leaf_rect_scratch = malloc(sizeof(*leaf_rect_scratch) *
                                       LEAF_RECTS_PER_ROW_MAX);
            if (leaf_rect_scratch == NULL) {
                ESP_LOGE(TAG, "overlay: could not allocate %u-byte leaf "
                              "rect scratch - staying off",
                         (unsigned)(sizeof(*leaf_rect_scratch) *
                                    LEAF_RECTS_PER_ROW_MAX));
                return;
            }
        }
        leaf_overlay_on = true;
    } else {
        leaf_overlay_on = false;
        free(leaf_rect_scratch);
        leaf_rect_scratch = NULL;
    }
}
#endif

/* Device-only path for QSPI panel send. Host build is no-op. */
#ifdef ESP_PLATFORM

/* gather_buf is shared and about to be overwritten, so every queued
 * transfer, not just the most recent, must drain first. strip_sent is a
 * plain counter with no transfer identity: taking it once is not the
 * same as waiting for THIS gather, since whichever transfer finishes
 * first satisfies whichever Take() runs. Draining exactly *queued first
 * empties the queue, so the one Take() after this draw_bitmap()
 * unambiguously waits for it - SPI transactions on one device complete
 * in queued order. */
static void gather_and_send(int x0, int y0, int x1, int y1, int row,
                            int run_start, int run_end, bool refined,
                            int *queued, gfx_color_t border)
{
    const int w = x1 - x0;
    const int h = y1 - y0;

    for (int j = 0; j < *queued; j++) {
        xSemaphoreTake(strip_sent, portMAX_DELAY);
    }
    *queued = 0;

    for (int r = 0; r < h; r++) {
        memcpy(gather_buf + (size_t)r * w,
              fb + (size_t)(y0 + r) * GFX_WIDTH + x0,
              (size_t)w * sizeof(gfx_color_t));
    }
#if CONFIG_LAUNCHER_DEVELOPMENT
    if (debug_overlay_on && refined) {
        /* One border around the whole packed box. */
        mark_rect_border(gather_buf, w, w, h, border);
    } else if (debug_overlay_on) {
        for (int col = run_start; col < run_end; col++) {
            const int idx = row * GRID_COLS + col;
            gfx_color_t *at = gather_buf +
                             (size_t)(cell_y0[idx] - y0) * w +
                             (cell_x0[idx] - x0);
            mark_rect_border(at, w, cell_x1[idx] - cell_x0[idx],
                             cell_y1[idx] - cell_y0[idx], border);
        }
    }
    if (leaf_overlay_on) {
        const int n = dirty_leaf_rects(row, x0, y0, x1, y1, leaf_rect_scratch,
                                       LEAF_RECTS_PER_ROW_MAX);
        for (int i = 0; i < n; i++) {
            const dirty_leaf_rect_t *r = &leaf_rect_scratch[i];
            gfx_color_t *at =
                gather_buf + (size_t)(r->y0 - y0) * w + (r->x0 - x0);
            mark_rect_border(at, w, r->x1 - r->x0, r->y1 - r->y0,
                             gfx_rgb(0x00FF00));
        }
    }
#else
    (void)row; (void)run_start; (void)run_end; (void)refined; (void)border;
#endif
    esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, gather_buf);
    xSemaphoreTake(strip_sent, portMAX_DELAY);
}

static void send_full_row(int row, int *queued)
{
    const int y = row * STRIP_HEIGHT;

#if CONFIG_LAUNCHER_DEVELOPMENT
    /* leaf_rect_scratch never NULL: gfx_set_leaf_overlay() allocates */
    int leaf_n = 0;
    if (leaf_overlay_on) {
        leaf_n = dirty_leaf_rects(row, 0, y, GFX_WIDTH, y + STRIP_HEIGHT,
                                  leaf_rect_scratch, LEAF_RECTS_PER_ROW_MAX);
    }

    /* Cyan borders, green leaves, or both. Skip if leaf_overlay_on and no
     * dirty leaves. Transfer immediately for debugging. */
    if (debug_overlay_on || leaf_n > 0) {
        /* Save phase: prevent overwriting shared pixels. */
        if (debug_overlay_on) {
            for (int col = 0; col < GRID_COLS; col++) {
                gfx_color_t *cell =
                    fb + (size_t)y * GFX_WIDTH + col * COL_WIDTH;
                save_border(cell, GFX_WIDTH, COL_WIDTH, STRIP_HEIGHT,
                           overlay_cell_save()[col]);
            }
        }
        for (int i = 0; i < leaf_n; i++) {
            const dirty_leaf_rect_t *r = &leaf_rect_scratch[i];
            gfx_color_t *at = fb + (size_t)r->y0 * GFX_WIDTH + r->x0;
            save_border(at, GFX_WIDTH, r->x1 - r->x0, r->y1 - r->y0,
                       overlay_leaf_save()[i]);
        }

        if (debug_overlay_on) {
            for (int col = 0; col < GRID_COLS; col++) {
                gfx_color_t *cell =
                    fb + (size_t)y * GFX_WIDTH + col * COL_WIDTH;
                mark_rect_border(cell, GFX_WIDTH, COL_WIDTH, STRIP_HEIGHT,
                                gfx_rgb(0x00FFFF));
            }
        }
        for (int i = 0; i < leaf_n; i++) {
            const dirty_leaf_rect_t *r = &leaf_rect_scratch[i];
            gfx_color_t *at = fb + (size_t)r->y0 * GFX_WIDTH + r->x0;
            mark_rect_border(at, GFX_WIDTH, r->x1 - r->x0, r->y1 - r->y0,
                            gfx_rgb(0x00FF00));
        }

        esp_lcd_panel_draw_bitmap(panel, 0, y, GFX_WIDTH, y + STRIP_HEIGHT,
                                  fb + (size_t)y * GFX_WIDTH);
        xSemaphoreTake(strip_sent, portMAX_DELAY);

        /* Restore in the reverse order of saving. */
        for (int i = leaf_n - 1; i >= 0; i--) {
            const dirty_leaf_rect_t *r = &leaf_rect_scratch[i];
            gfx_color_t *at = fb + (size_t)r->y0 * GFX_WIDTH + r->x0;
            restore_border(at, GFX_WIDTH, r->x1 - r->x0, r->y1 - r->y0,
                          overlay_leaf_save()[i]);
        }
        if (debug_overlay_on) {
            for (int col = GRID_COLS - 1; col >= 0; col--) {
                gfx_color_t *cell =
                    fb + (size_t)y * GFX_WIDTH + col * COL_WIDTH;
                restore_border(cell, GFX_WIDTH, COL_WIDTH, STRIP_HEIGHT,
                              overlay_cell_save()[col]);
            }
        }
        return;
    }
#endif

    esp_lcd_panel_draw_bitmap(panel, 0, y, GFX_WIDTH, y + STRIP_HEIGHT,
                              fb + (size_t)y * GFX_WIDTH);
    (*queued)++;
}

/* Third, cheapest send path: a full-width box is already contiguous in
 * `fb`, so it sends exactly the panel's bytes with no packing. Possible
 * only because the box carries a real sub-strip Y extent, from
 * cell_y0/cell_y1 tracking (gfx_dirty.h), not the coarse strip grid.
 * Measured ~10% fewer pixels per frame on falling-sand/lava scenes, 0%
 * where strips are genuinely full-height. Declines whenever either
 * overlay layer is on: their save/restore machinery assumes
 * send_full_row()'s full STRIP_HEIGHT box. */
static bool send_partial_band(int y0, int y1, int *queued)
{
#if CONFIG_LAUNCHER_DEVELOPMENT
    if (overlay_any_on()) {
        return false;
    }
#endif
    esp_lcd_panel_draw_bitmap(panel, 0, y0, GFX_WIDTH, y1,
                              fb + (size_t)y0 * GFX_WIDTH);
    (*queued)++;
    return true;
}

/* See gfx_dirty.h file comment */

/* Yellow; see gather_and_send()'s comment */
static void send_run(int row, int run_start, int run_end, int box_x0,
                     int box_x1, int box_y0, int box_y1, int split_n,
                     const int *split_x0, const int *split_x1, int *queued)
{
    if (split_n == 0) {
        gather_and_send(box_x0, box_y0, box_x1, box_y1, row, run_start,
                        run_end, false, queued, gfx_rgb(0xFFFF00));
        return;
    }

    for (int i = 0; i < split_n; i++) {
        gather_and_send(split_x0[i], box_y0, split_x1[i], box_y1, row,
                        run_start, run_end, true, queued, gfx_rgb(0xFFFF00));
    }
}

/* Cells merge into transactions; gaps remain. Falls back to full row for
 * large parts. */
static void send_one_row(int row, int *queued)
{
    int run_start[GRID_COLS], run_end[GRID_COLS];
    int box_x0[GRID_COLS], box_x1[GRID_COLS];
    int box_y0[GRID_COLS], box_y1[GRID_COLS];
    int split_n[GRID_COLS];
    int split_x0[GRID_COLS][LEAF_REFINE_MAX_RUNS];
    int split_x1[GRID_COLS][LEAF_REFINE_MAX_RUNS];
    const int n = collect_dirty_runs(row, run_start, run_end);

    for (int r = 0; r < n; r++) {
        run_box(row, run_start[r], run_end[r], &box_x0[r], &box_x1[r],
               &box_y0[r], &box_y1[r]);
        split_n[r] = plan_run(row, run_start[r], run_end[r], box_y0[r],
                              box_y1[r], split_x0[r], split_x1[r]);

        if (split_n[r] == 0) {
            const size_t area = (size_t)(box_x1[r] - box_x0[r]) *
                                (size_t)(box_y1[r] - box_y0[r]);
            if (area > GATHER_MAX_PIXELS) {
                /* See send_partial_band(). Full-width box means row's only
                 * run - safe to return. */
                if (box_x0[r] == 0 && box_x1[r] == GFX_WIDTH &&
                    box_y1[r] - box_y0[r] < STRIP_HEIGHT &&
                    send_partial_band(box_y0[r], box_y1[r], queued)) {
#if CONFIG_LAUNCHER_DEVELOPMENT
                    dev_strips_sent_partial++;
#endif
                    return;
                }
#if CONFIG_LAUNCHER_DEVELOPMENT
                dev_strips_sent_full++;
#endif
                send_full_row(row, queued);
                return;
            }
        }
    }

#if CONFIG_LAUNCHER_DEVELOPMENT
    if (n > 0) {
        dev_strips_sent_gathered++;
    }
#endif
    for (int r = 0; r < n; r++) {
        send_run(row, run_start[r], run_end[r], box_x0[r], box_x1[r],
                box_y0[r], box_y1[r], split_n[r], split_x0[r], split_x1[r],
                queued);
    }
}

void gfx_present(void)
{
    int queued = 0;
    if (interlace_on) {
        frame_parity = !frame_parity;
    }

    uint32_t remaining_cell_dirty = 0;

    for (int row = 0; row < STRIP_COUNT; row++) {
        if (!dirty_row_is_dirty(row)) {
            continue;   /* unchanged - the panel is still showing it */
        }

        if (interlace_on && (row % 2) != frame_parity) {
            remaining_cell_dirty |= cell_dirty &
                (((1u << GRID_COLS) - 1u) << (row * GRID_COLS));
            continue;
        }

        send_one_row(row, &queued);
        dirty_row_sent(row);
    }

    dirty_frame_sent();
    if (interlace_on) {
        cell_dirty = remaining_cell_dirty;
    }

    if (partial_clear_on && drawn_bbox_valid) {
        prev_bbox_x0 = drawn_bbox_x0;
        prev_bbox_y0 = drawn_bbox_y0;
        prev_bbox_x1 = drawn_bbox_x1;
        prev_bbox_y1 = drawn_bbox_y1;
        prev_bbox_valid = true;
    } else if (!partial_clear_on) {
        prev_bbox_valid = false;
    }
    drawn_bbox_valid = false;

    /* Wait for queued full-width sends to drain. */
    for (int i = 0; i < queued; i++) {
        xSemaphoreTake(strip_sent, portMAX_DELAY);
    }
}

#else   /* !ESP_PLATFORM */

void gfx_present(void)
{
    /* No panel on a host build - see this section's own top comment. */
}

#endif   /* ESP_PLATFORM - the presentation pipeline */

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Bypasses gfx_present() for raw QSPI. SPI driver splits into chunks, one
 * strip_sent means full frame. */
void gfx_present_raw_full_frame_for_test(void)
{
    esp_lcd_panel_draw_bitmap(panel, 0, 0, GFX_WIDTH, GFX_HEIGHT, fb);
    xSemaphoreTake(strip_sent, portMAX_DELAY);
}
#endif
