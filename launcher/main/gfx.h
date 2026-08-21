/*=============================================================================
 * gfx - framebuffer ownership and drawing primitives.
 *
 * Everything on this device draws into ONE full-screen RGB565 framebuffer that
 * this module owns. The shell and every app share it; nothing else allocates a
 * buffer of its own. At 368x448x2 that single buffer is 322 KiB of the ~424
 * KiB the chip has, so a second one is not affordable.
 *
 * Colours are given as plain 0xRRGGBB so callers never deal with the panel's
 * byte-swapped RGB565 layout - gfx_rgb() handles that conversion.
 *===========================================================================*/
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "bsp/esp-bsp.h"

#define GFX_WIDTH   BSP_LCD_H_RES   /* 368 */
#define GFX_HEIGHT  BSP_LCD_V_RES   /* 448 */

/* Glyphs are 8x8 in the font data, drawn at 2x so they are legible on a
 * 368-wide panel. Text metrics elsewhere must agree with these. */
#define GFX_GLYPH_SCALE 2
#define GFX_CHAR_W      (8 * GFX_GLYPH_SCALE)
#define GFX_CHAR_H      (8 * GFX_GLYPH_SCALE)

/* A packed, panel-ready pixel. Produced by gfx_rgb(), stored in the
 * framebuffer, never inspected by callers. */
typedef uint16_t gfx_color_t;

/* Brings up the panel and allocates the framebuffer.
 * Returns false if either fails; the reason is logged. */
bool gfx_init(void);

/* Convert 0xRRGGBB to the panel's pixel format. */
gfx_color_t gfx_rgb(uint32_t rgb);

/* Direct access, for renderers that write pixels in bulk (the 3D rasterizer
 * writes here directly rather than going through gfx_pixel per fragment). */
gfx_color_t *gfx_framebuffer(void);

void gfx_clear(gfx_color_t color);
void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color);

/* Both clip to the framebuffer, so callers need not bounds-check. */
void gfx_pixel(int x, int y, gfx_color_t color);

/* Draws at GFX_GLYPH_SCALE - the size the UI is laid out around. */
void gfx_text(int x, int y, const char *text, gfx_color_t color);

/* Same, at an explicit glyph scale. Scale 1 gives 8x8 glyphs and 46 columns
 * across the panel, which is what makes a dense report like the POST table fit
 * on screen at all. */
void gfx_text_scaled(int x, int y, const char *text, gfx_color_t color,
                     int scale);

/* Text metrics. Kept here so the UI layer and the renderer cannot disagree. */
int gfx_text_width(const char *text, int len);
int gfx_text_height(void);

/* Restrict subsequent drawing to a rectangle. microui emits clip commands
 * around every container, and honouring them is what stops a scrolled panel
 * painting over the rest of the screen. */
void gfx_set_clip(int x, int y, int w, int h);
void gfx_clear_clip(void);

/* Send the finished frame to the panel and wait for the transfer to land.
 * The wait is mandatory - see the notes on asynchronous DMA in the docs. */
void gfx_present(void);
