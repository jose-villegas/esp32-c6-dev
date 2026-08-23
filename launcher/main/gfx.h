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
#include "gfx_color.h"

#define GFX_WIDTH   BSP_LCD_H_RES   /* 368 */
#define GFX_HEIGHT  BSP_LCD_V_RES   /* 448 */

/* QSPI clock for the panel.
 *
 * Sending one frame is 322 KiB over four lanes, and gfx_present() measures at
 * 94% of the theoretical rate - so this number, and only this number, sets how
 * long a frame takes to reach the screen:
 *
 *     40 MHz -> 16.5 ms theoretical, 17.6 ms measured
 *     80 MHz ->  8.2 ms theoretical
 *
 * The vendor driver defaults to 40 MHz and that is the figure Waveshare and
 * Espressif validate. 80 MHz is within what the ESP32-C6's general-purpose SPI
 * can produce, but it is beyond what anyone documents for this panel, so it is
 * an overclock: if the display shows tearing, wrong colours or noise, this is
 * the first thing to put back.
 *
 * Re-measured at 80 MHz again after the move to the per-cell grid and
 * gathered runs, on the theory that a settled render path might not hit
 * the same margin the old prototype did. It still does: the same small,
 * corner-shaped pixel artifacts came back, so this is not a transfer-
 * pattern-specific problem - it is this panel's real ceiling on this bus.
 * A synthetic timing test also regressed at 80 MHz for an unrelated
 * reason (test_two_far_corners_cost_less_than_a_full_band): a full band
 * gets proportionally cheaper at a faster clock while the fixed per-
 * transaction cost does not, so the gather-vs-fallback thresholds below
 * are tuned for 40 MHz specifically and would need re-measuring, not just
 * reused, if this ever moves again. */
#define GFX_QSPI_HZ (40 * 1000 * 1000)

/* Glyphs are 8x8 in the font data, drawn at 2x so they are legible on a
 * 368-wide panel. Text metrics elsewhere must agree with these. */
#define GFX_GLYPH_SCALE 2
#define GFX_CHAR_W      (8 * GFX_GLYPH_SCALE)
#define GFX_CHAR_H      (8 * GFX_GLYPH_SCALE)

/* Brings up the panel and allocates the framebuffer.
 * Returns false if either fails; the reason is logged. */
bool gfx_init(void);

/* Convert 0xRRGGBB to the panel's pixel format.
 *
 * GFX_RGB in gfx_color.h does the same thing in a constant expression, which
 * is what lets a colour table live in flash rather than RAM. Both go through
 * one definition, so they cannot drift apart. */
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

/* Same, turned in 90-degree steps: 0 is upright, 1 reads top-to-bottom, 2 is
 * upside down, 3 reads bottom-to-top.
 *
 * (x, y) is where the first glyph's cell begins, and the string runs away from
 * it in whichever direction the rotation implies.
 *
 * Exists because "the top of the screen" stops meaning the top edge once the
 * device is turned - a label placed against the up-edge of a sideways board
 * reads sideways unless it is turned with it. */
void gfx_text_turned(int x, int y, const char *text, gfx_color_t color,
                     int scale, int quarter_turns);

/* Text metrics. Kept here so the UI layer and the renderer cannot disagree. */
int gfx_text_width(const char *text, int len);
int gfx_text_height(void);

/* Restrict subsequent drawing to a rectangle. microui emits clip commands
 * around every container, and honouring them is what stops a scrolled panel
 * painting over the rest of the screen. */
void gfx_set_clip(int x, int y, int w, int h);
void gfx_clear_clip(void);

/* Release SPI2 so something else can use it - in practice, the SD card, which
 * is wired to different pins on the same controller and so cannot share it.
 * The framebuffer survives: it is ordinary RAM, unrelated to the bus.
 *
 * Nothing may be presented between suspend and resume. The panel keeps showing
 * whatever was last sent to it, because it refreshes from its own GRAM. */
bool gfx_suspend(void);

/* Bring the panel back.
 *
 * `full_init` re-sends the initialisation sequence. It is not normally needed:
 * the SH8601 keeps its registers while powered, so only the ESP32 side has to
 * be rebuilt - and the sequence carries a 120 ms settle that dominates the
 * cost of a round trip. Pass true only if the panel has actually lost power. */
bool gfx_resume(bool full_init);

/*---------------------------------------------------------------------------
 * Dirty tracking
 *
 * gfx_present() sends only the horizontal bands that changed. The panel holds
 * the rest in its own GRAM, so anything not sent simply stays on screen - and
 * sending is almost the whole cost of a frame, so this is where the time is.
 *
 * Most code never touches any of this. Every gfx_* drawing call marks what it
 * touched, and gfx_clear() marks the whole screen, so an app that clears and
 * redraws is correct without knowing dirty tracking exists.
 *
 * It matters only for code writing through gfx_framebuffer() directly, which
 * gfx cannot see. Such code MUST mark what it wrote. Forgetting looks like a
 * frozen or partially stale screen, not a crash.
 *-------------------------------------------------------------------------*/

/* Declare that a rectangle of the framebuffer has changed. Tracked as a real
 * box per grid cell, not just which cell - a caller that knows it only
 * touched part of a cell may end up sending less than the whole thing. */
void gfx_mark_dirty(int x, int y, int w, int h);

void gfx_mark_all_dirty(void);

/* Whether any band overlapping this rectangle is already going to be sent.
 *
 * For overlay content that is identical every frame - the shell's home hint -
 * this answers "does it need redrawing?". If nothing below it changed, the
 * pixels are still in the framebuffer and still on the panel, and both the
 * draw and the transfer can be skipped. */
bool gfx_region_dirty(int x, int y, int w, int h);

/* Send the changed bands to the panel and wait for the transfers to land.
 * The wait is mandatory - see the notes on asynchronous DMA in the docs. */
void gfx_present(void);

/* Runtime toggle for the grid dirty-region overlay - see gfx_present()'s
 * mark_rect_border(). Off by default even in a development build: it draws
 * directly over real content, so it should be opted into, not always on.
 *
 * Declared only under CONFIG_LAUNCHER_DEVELOPMENT on purpose: a debug knob
 * a release build can still call, quietly doing nothing, is a debug knob
 * a caller can forget to guard. Calling this from a file that is not
 * itself development-only should fail to compile, not silently no-op -
 * the Diagnostics app that owns the checkbox for this is already excluded
 * from a non-development build entirely (see main/CMakeLists.txt), so it
 * sees these declarations exactly when it is allowed to. */
#if CONFIG_LAUNCHER_DEVELOPMENT
void gfx_set_debug_overlay(bool on);
bool gfx_debug_overlay(void);
#endif
