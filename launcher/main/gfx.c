#include "gfx.h"
#include "gfx_dirty.h"
#include "intmath.h"

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

/* gfx_dirty.h cannot include gfx.h (it must stay ESP-IDF-free to compile on
 * a host), so it carries its own GFX_DIRTY_WIDTH/HEIGHT literals instead of
 * gfx.h's BSP-derived GFX_WIDTH/HEIGHT. This is what keeps the two from
 * silently drifting apart if the board's resolution ever changes. */
_Static_assert(GFX_WIDTH == GFX_DIRTY_WIDTH && GFX_HEIGHT == GFX_DIRTY_HEIGHT,
              "gfx_dirty.h's screen dimensions must match gfx.h's");

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

/* Scratch space for packing one gathered run's box edge-to-edge before
 * sending it as one draw_bitmap() call - see gather_and_send(). Bounded by
 * GATHER_MAX_PIXELS (gfx_dirty.h) - a run bigger than that is not worth
 * gathering at all, at which point the row is sent whole instead.
 *
 * Allocated with MALLOC_CAP_DMA, same as `fb` below - a plain static array
 * is only guaranteed the alignment its element type needs (2 bytes for
 * uint16_t), not whatever the GDMA engine actually requires. A source buffer
 * the DMA can't read cleanly does not fail loudly; it reads back subtly
 * wrong, which is a much worse failure to chase. */
static gfx_color_t *gather_buf;

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
    /* Counting, not binary: a whole frame's rows are queued before any is
     * awaited, so several finish first. A binary semaphore would saturate at
     * one, discard the rest, and deadlock on the next wait.
     *
     * Sized for STRIP_COUNT * GRID_COLS, not STRIP_COUNT: a row can send up
     * to one independent gather per column - see send_one_row(). Undersizing
     * this does not corrupt anything - xSemaphoreGiveFromISR above the max
     * simply fails - but gfx_present()'s wait loop would then block forever
     * on gives that never happened. */
    strip_sent = xSemaphoreCreateCounting(
        STRIP_COUNT * GRID_COLS + 2, 0);
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

    const size_t gather_bytes = (size_t)GATHER_MAX_PIXELS * sizeof(gfx_color_t);
    gather_buf = heap_caps_malloc(gather_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (gather_buf == NULL) {
        ESP_LOGE(TAG, "Could not allocate %u byte gather buffer",
                 (unsigned)gather_bytes);
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

/* The actual tracking - state, geometry and the grid/leaf logic - lives in
 * gfx_dirty.h as a header-only module (see its own file comment for why:
 * mark_band() must stay inlinable into this translation unit). These three
 * are thin wrappers so gfx.h's public API keeps its existing names and
 * every other file in the project stays unaware the split exists. */
void gfx_mark_all_dirty(void) { dirty_mark_all(); }
void gfx_mark_dirty(int x, int y, int w, int h) { dirty_mark(x, y, w, h); }
bool gfx_region_dirty(int x, int y, int w, int h)
{
    (void)x; (void)w;
    return dirty_region_dirty(y, h);
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

/* Shorten a line to the part inside the clip rect, or reject it outright.
 * Returns false if none of it is on screen.
 *
 * WHY THIS IS NOT JUST THE PER-PIXEL TEST
 *
 * The Bresenham loop below already skips pixels outside the clip rect, which
 * is correct but costs a step per pixel of the line's length whether or not
 * any of them land. That was fine while the only caller was a plotted curve
 * whose points were all on screen. It stopped being fine with a floor grid
 * that runs until it leaves the panel: those lines are mostly off it, and
 * some are entirely off it.
 *
 * Clipping moves where Bresenham's error term starts, so a clipped line can
 * differ by a pixel from the same line drawn unclipped. That is why the
 * caller takes a fast path when both ends are already inside: the common case
 * stays exactly as it was, and nothing that fits on screen is touched by any
 * of this.
 */
static bool clip_line(int *x0, int *y0, int *x1, int *y1)
{
    int c0 = outcode(*x0, *y0);
    int c1 = outcode(*x1, *y1);

    /* Bounded rather than while(1): every pass either accepts, rejects, or
     * moves one endpoint onto an edge, so four is already more than it can
     * need. A loop that cannot terminate is not a risk worth taking on a
     * device with a watchdog and no console. */
    for (int pass = 0; pass < 8; pass++) {
        if ((c0 | c1) == 0) {
            return true;              /* both ends inside */
        }
        if ((c0 & c1) != 0) {
            return false;             /* both beyond the same edge */
        }

        const int out = c0 ? c0 : c1;
        int x, y;

        /* The far edges are exclusive, so this clips to the last pixel
         * inside rather than to the boundary itself. */
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

/* Bresenham, in the form that treats both axes alike so no case analysis is
 * needed for steep versus shallow lines.
 *
 * `blend` is what the two public wrappers below differ by, and it is passed
 * as a flag rather than duplicating the walk: the loop is the part worth
 * getting right once, and the alternative is two copies that drift.
 *
 * The dirty box is worked out up front, from the line's own bounding box
 * intersected with the clip rect, and marked ONCE - rather than per pixel the
 * way gfx_pixel() would. A diagonal line's bounding box is mostly empty, so
 * this claims more than it writes; that only ever costs bus time, whereas
 * claiming too little leaves stale pixels on the panel. */
static void draw_line(int x0, int y0, int x1, int y1, gfx_color_t color,
                      bool blend, bool open_start)
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

    const int dx = im_abs(x1 - x0);
    const int dy = -im_abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    bool first = true;

    for (;;) {
        if (!(first && open_start) &&
            x0 >= clip.x0 && x0 < clip.x1 && y0 >= clip.y0 && y0 < clip.y1) {
            gfx_color_t *const dst = &fb[(size_t)y0 * GFX_WIDTH + x0];
            *dst = blend ? gfx_color_add(*dst, color) : color;
        }
        first = false;
        if (x0 == x1 && y0 == y1) {
            break;
        }
        /* One error term, two independent tests: whichever axis the line is
         * long on steps every time, the other steps when the error says so,
         * and a 45-degree line steps both. */
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }

    if (bx0 < bx1 && by0 < by1) {
        dirty_mark(bx0, by0, bx1 - bx0, by1 - by0);
    }
}

void gfx_line(int x0, int y0, int x1, int y1, gfx_color_t color)
{
    draw_line(x0, y0, x1, y1, color, false, false);
}

void gfx_line_add(int x0, int y0, int x1, int y1, gfx_color_t color)
{
    draw_line(x0, y0, x1, y1, color, true, false);
}

void gfx_line_add_open(int x0, int y0, int x1, int y1, gfx_color_t color)
{
    draw_line(x0, y0, x1, y1, color, true, true);
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
 * One font-aware path (gfx_text_font(), gfx_font_width()) that everything
 * else here delegates to, passing gfx_default_font() - see gfx_font.h for
 * what a gfx_font_t is and why it exists: honouring microui's mu_Font is a
 * later task, in files this one steers clear of, and that task needs a font
 * to point AT. gfx_default_font() wraps font8x8_basic.h's data, which is
 * public-domain 8x8 bitmap data: one byte per row, and within a row bit 0 is
 * the LEFTMOST pixel - see gfx_font_8x8's own comment in gfx_font.h.
 *-------------------------------------------------------------------------*/

const gfx_font_t *gfx_default_font(void)
{
    return &gfx_font_8x8;
}

int gfx_font_width(const gfx_font_t *font, const char *text, int len,
                   int scale)
{
    return gfx_font_text_width(font, text, len, scale);
}

int gfx_text_width(const char *text, int len)
{
    return gfx_font_width(gfx_default_font(), text, len, GFX_GLYPH_SCALE);
}

int gfx_text_height(void)
{
    return gfx_font_height(gfx_default_font(), GFX_GLYPH_SCALE);
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

/* Where one font pixel at (row, col) within its own cell lands once rotated
 * by `turn` quarter-turns. The cell keeps its origin at (x, y) whichever way
 * it is turned - only the string's advance direction differs between
 * rotations, not this. Drawn as a scale x scale square.
 *
 * Generalised from the old fixed-8x8 version to font->cell_w/cell_h, but
 * still only ever exercised at cell_w == cell_h == 8 - the only font that
 * exists. A future non-square font's rotation is unverified until one shows
 * up to test it against. */
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

/* Draws one glyph of `font`, or nothing at all when there is nothing safe to
 * draw:
 *
 *   - `ch` outside [font->first, font->first + font->count) - the same gate
 *     gfx_text_turned() always had (it skipped anything >= 128, and this
 *     font's first/count is exactly [0, 128)), generalised to whatever range
 *     `font` actually covers.
 *
 *   - font->bpp != 1. The coverage atlas this descriptor makes room for
 *     needs blending to draw - each atlas byte would be a coverage level,
 *     not a 1-bit mask - and gfx has no blending anywhere yet (draw_command()
 *     in ui.c skips transparent rects for the same reason, per its own
 *     comment). Wiring that up is a separate task; an unrendered glyph is a
 *     far smaller problem than a garbled one in the meantime. */
static void draw_glyph_font(const gfx_font_t *font, int x, int y,
                            unsigned char ch, gfx_color_t color, int scale,
                            int turn)
{
    if (font->bpp != 1) {
        return;
    }
    if (ch < font->first || (unsigned)(ch - font->first) >= font->count) {
        return;
    }

    const uint8_t *glyph = font->atlas + (size_t)(ch - font->first) * font->cell_h;

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
}

void gfx_text_font(int x, int y, const char *text, gfx_color_t color,
                   int scale, int quarter_turns, const gfx_font_t *font)
{
    if (scale < 1) {
        scale = 1;
    }

    const int turn = ((quarter_turns % 4) + 4) % 4;

    /* Which way the string advances from one glyph to the next. Applied to
     * whatever gfx_font_advance() says this glyph's step is - for today's
     * monospace font that is always cell_w * scale, matching the old fixed
     * `cell` exactly, including that it advanced even past a codepoint it
     * declined to draw. */
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
    gfx_text_font(x, y, text, color, scale, quarter_turns, gfx_default_font());
}

/*---------------------------------------------------------------------------
 * Present
 *-------------------------------------------------------------------------*/

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Runtime state for the overlay below. Declared only in this block, same as
 * the getter/setter that touch it - see the "why not always compiled"
 * comment on their declaration in gfx.h. */
static bool debug_overlay_on;

void gfx_set_debug_overlay(bool on) { debug_overlay_on = on; }
bool gfx_debug_overlay(void) { return debug_overlay_on; }

/* Same story as debug_overlay_on above - see gfx_set_leaf_overlay()'s own
 * comment in gfx.h for what this actually draws. */
static bool leaf_overlay_on;

void gfx_set_leaf_overlay(bool on) { leaf_overlay_on = on; }
bool gfx_debug_leaf_overlay(void) { return leaf_overlay_on; }

/* Outlines whichever rectangle is about to be sent, one row and one column
 * of pixels deep - shows exactly which segments of the strip/gather split
 * are triggering an update on a real interaction, and which path each one
 * took, rather than only in a synthetic test. Dev builds only: this writes
 * directly into what gets sent, which is the point, but has no business in
 * a shipped image.
 *
 * `stride` is the buffer's own row length, not the rectangle's width - for
 * gather_buf the two are the same (tightly packed to `w`), for `fb` they
 * are not (always GFX_WIDTH, regardless of how much of that row changed). */
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

/* Draws a thin line at every LEAF_W/LEAF_H boundary strictly inside
 * [x0,x0+w) x [y0,y0+h) of `buf` (stride `stride`) - the fixed leaf
 * subdivision (gfx_dirty.h) underneath whatever box actually got sent,
 * without touching the box's own outer border (mark_rect_border() already
 * owns that pixel). `x0`/`y0` are the box's absolute panel position,
 * needed to find where the fixed boundaries - always at multiples of
 * LEAF_W/LEAF_H - fall relative to this buffer's own local origin.
 *
 * Only ever called from gather_and_send(), never send_full_row(): a full-
 * row fallback only happens when a run was not leaf-eligible or its best
 * split still exceeded the gather budget, so there is nothing this could
 * show there that would not be misleading - the whole point is to show
 * where refinement *could* subdivide, and cyan means it already could
 * not. */
static void mark_leaf_grid(gfx_color_t *buf, int stride, int w, int h,
                           int x0, int y0, gfx_color_t colour)
{
    for (int lx = ((x0 / LEAF_W) + 1) * LEAF_W; lx < x0 + w; lx += LEAF_W) {
        const int col = lx - x0;
        for (int row = 0; row < h; row++) {
            buf[(size_t)row * stride + col] = colour;
        }
    }
    for (int ly = ((y0 / LEAF_H) + 1) * LEAF_H; ly < y0 + h; ly += LEAF_H) {
        const int row = ly - y0;
        for (int col = 0; col < w; col++) {
            buf[(size_t)row * stride + col] = colour;
        }
    }
}

/* Copies out, or back in, exactly the pixels mark_rect_border() touches for
 * a COL_WIDTH x STRIP_HEIGHT cell - into/from a buffer sized BORDER_PIXELS.
 * Used to make the cyan border on a full-width send temporary: unlike the
 * gathered paths, which draw their border into the disposable gather_buf,
 * a full-width send has no scratch copy - it sends fb directly, so drawing
 * the border there would permanently corrupt fb for whoever reads it next:
 * a later gather over this exact cell, or this row simply never being
 * redrawn again. Same pixel order both ways, so save/restore are exact
 * inverses of each other. */
#define BORDER_PIXELS (2 * (COL_WIDTH + STRIP_HEIGHT))

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
#endif

/* Packs [x0,x1) x [y0,y1) into gather_buf and sends it as one draw_bitmap()
 * call.
 *
 * gather_buf is shared and about to be overwritten, so every transfer
 * queued so far - not just the most recent one - must actually have
 * finished reading out of it first. strip_sent is a plain counter with no
 * identity attached to which transfer signalled it, so taking it once here
 * is not the same as waiting for THIS gather specifically: whichever
 * transfer happens to finish first satisfies whichever Take() runs first,
 * and an earlier still-batched full-width send finishing first would let
 * this proceed while a previous gather's own transfer was still in flight.
 * Draining exactly `*queued` of them first empties the queue, so the one
 * Take() after this gather's own draw_bitmap() is unambiguously waiting
 * for it - SPI transactions on one device complete in the order they were
 * queued, so nothing else can be outstanding at that point. Safe to call
 * more than once per row for the same reason: each call leaves the queue
 * empty again before returning. */
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
        /* A leaf-refined split is already the tight unit sent - unlike a
         * cell-run gather, it does not necessarily span whole cells, so
         * bordering per cell below would draw outside what was actually
         * sent. One border around the whole packed box instead. */
        mark_rect_border(gather_buf, w, w, h, border);
    } else if (debug_overlay_on) {
        /* One border per cell in the run, at that cell's own tight bounds -
         * never around the merged box as a whole. cell_x0/x1/y0/y1 are
         * already clipped to their own cell's column and row (see
         * union_cell_x/union_cell_y), so a border built from them can never
         * land on the fixed line shared with a neighbouring cell the way a
         * single border around the merged box could - it only ever draws
         * inside the cell it belongs to. */
        for (int col = run_start; col < run_end; col++) {
            const int idx = row * GRID_COLS + col;
            gfx_color_t *at = gather_buf +
                             (size_t)(cell_y0[idx] - y0) * w +
                             (cell_x0[idx] - x0);
            mark_rect_border(at, w, cell_x1[idx] - cell_x0[idx],
                             cell_y1[idx] - cell_y0[idx], border);
        }
    }
    if (debug_overlay_on && leaf_overlay_on) {
        mark_leaf_grid(gather_buf, w, w, h, x0, y0, gfx_rgb(0x00FF00));
    }
#else
    (void)row; (void)run_start; (void)run_end; (void)refined; (void)border;
#endif
    esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, gather_buf);
    xSemaphoreTake(strip_sent, portMAX_DELAY);
}

/* Sends row's whole band, full width - too many cells dirty to be worth
 * gathering them independently. Queues without waiting, batched with
 * whichever other rows do the same; gfx_present() drains them all together
 * at the end. */
static void send_full_row(int row, int *queued)
{
    const int y = row * STRIP_HEIGHT;

#if CONFIG_LAUNCHER_DEVELOPMENT
    /* Cyan: outlines each cell individually, even though they are all
     * going out together in one full-width transfer - shows the grid
     * structure consistently regardless of which path a row took.
     *
     * Has to wait for its own transfer immediately, rather than batching
     * like the plain path below, so the saved pixels can be put back
     * before anything else - a later gather, a later frame - reads fb
     * again. Acceptable cost while actively debugging, not otherwise. */
    if (debug_overlay_on) {
        static gfx_color_t saved[GRID_COLS][BORDER_PIXELS];
        for (int col = 0; col < GRID_COLS; col++) {
            gfx_color_t *cell = fb + (size_t)y * GFX_WIDTH + col * COL_WIDTH;
            save_border(cell, GFX_WIDTH, COL_WIDTH, STRIP_HEIGHT, saved[col]);
            mark_rect_border(cell, GFX_WIDTH, COL_WIDTH, STRIP_HEIGHT,
                             gfx_rgb(0x00FFFF));
        }
        esp_lcd_panel_draw_bitmap(panel, 0, y, GFX_WIDTH, y + STRIP_HEIGHT,
                                  fb + (size_t)y * GFX_WIDTH);
        xSemaphoreTake(strip_sent, portMAX_DELAY);
        for (int col = 0; col < GRID_COLS; col++) {
            gfx_color_t *cell = fb + (size_t)y * GFX_WIDTH + col * COL_WIDTH;
            restore_border(cell, GFX_WIDTH, COL_WIDTH, STRIP_HEIGHT,
                           saved[col]);
        }
        return;
    }
#endif

    esp_lcd_panel_draw_bitmap(panel, 0, y, GFX_WIDTH, y + STRIP_HEIGHT,
                              fb + (size_t)y * GFX_WIDTH);
    (*queued)++;
}

/* collect_runs_from_mask(), collect_dirty_runs(), run_is_leaf_eligible(),
 * leaf_mask_for_run(), refine_run(), plan_run() and run_box() all now live
 * in gfx_dirty.h, alongside the state they operate on - see its file
 * comment for why the split is header-only rather than a separate .c. */

/* Sends one dirty run - either as up to LEAF_REFINE_MAX_RUNS leaf-refined
 * pieces, tighter than the run's own coarse box, or as that coarse box
 * whole when refinement found nothing safe or worth splitting on. Yellow
 * either way - both are still "this got gathered and sent", just at
 * different granularity; see gather_and_send()'s overlay comment for how
 * the border itself differs. */
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

/* Sends row's dirty cells as one gather per contiguous run of dirty
 * columns - adjacent cells merge into a single transaction, since the
 * per-transaction cost dwarfs what a merged box carries extra, but a
 * genuine gap of clean columns between two separate features keeps them
 * as independent sends rather than one box spanning the untouched middle.
 * Falls back to the row whole if any run (or any leaf-refined split of
 * one) turns out too big to be worth gathering, rather than mixing a
 * partial gather with a full-width send - which would resend that part
 * twice. */
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
                send_full_row(row, queued);
                return;
            }
        }
    }

    for (int r = 0; r < n; r++) {
        send_run(row, run_start[r], run_end[r], box_x0[r], box_x1[r],
                box_y0[r], box_y1[r], split_n[r], split_x0[r], split_x1[r],
                queued);
    }
}

void gfx_present(void)
{
    int queued = 0;

    for (int row = 0; row < STRIP_COUNT; row++) {
        if (!dirty_row_is_dirty(row)) {
            continue;   /* unchanged - the panel is still showing it */
        }

        send_one_row(row, &queued);
        dirty_row_sent(row);
    }

    dirty_frame_sent();

    /* draw_bitmap only QUEUES a DMA transfer that reads out of the
     * framebuffer. Returning before they drain would let the next frame start
     * overwriting memory still being shifted out to the panel. Wait for
     * exactly as many full-width sends were queued - a gathered send was
     * already waited on above, so it must not be counted again here. */
    for (int i = 0; i < queued; i++) {
        xSemaphoreTake(strip_sent, portMAX_DELAY);
    }
}
