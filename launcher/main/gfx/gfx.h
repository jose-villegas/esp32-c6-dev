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

#ifdef ESP_PLATFORM
#include "bsp/esp-bsp.h"
#endif
#include "gfx/gfx_color.h"
#include "gfx/gfx_font.h"

/* ESP_PLATFORM is defined by ESP-IDF's own toolchain file - never by this
 * project - which is what makes it the natural, zero-plumbing switch
 * between the device build and a host one: a plain `gcc` invocation (the
 * same one test/run_tests.sh already uses) simply never defines it. The
 * host figures are literals rather than pulled from anywhere, the same
 * reason gfx_dirty.h and test/suites/suite_boot_anim.c already hardcode
 * them - a BSP header is exactly what a host build cannot include. */
#ifdef ESP_PLATFORM
#define GFX_WIDTH   BSP_LCD_H_RES   /* 368 */
#define GFX_HEIGHT  BSP_LCD_V_RES   /* 448 */
#else
#define GFX_WIDTH   368
#define GFX_HEIGHT  448
#endif

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
 * reused, if this ever moves again.
 *
 * An in-between clock is not on the table, so do not spend time on that
 * idea if 80 MHz is ever revisited. GPSPI2's clock is derived from an
 * 80 MHz source with an integer pre/n divider (spi_ll_master_cal_clock() in
 * the IDF's spi_ll.h): a request over 60 MHz uses the source directly at
 * 80, and a request at or under 60 MHz is bound by the divider search's
 * n >= 2 floor to at most 80/2 = 40. 60 MHz specifically was tried and
 * measured byte-identical to plain 40 - it silently landed on the 40 MHz
 * divider, not a real intermediate rate, because 60 is on the low side of
 * that 60 MHz boundary. Every value in this range resolves to one of
 * exactly two clocks. */
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

/* Enable or disable partial clear mode. When enabled, gfx_clear() erases only
 * the bounding box of what was marked dirty on the previous frame instead of
 * wiping the entire 322 KiB framebuffer, and automatically marks that erased
 * region dirty for presentation. Off by default. */
void gfx_set_partial_clear(bool enabled);
bool gfx_partial_clear_enabled(void);

/* Enable or disable interlace mode. When enabled, gfx_present() updates
 * only even-numbered strips on even frames and odd-numbered strips on odd
 * frames. Off by default. */
void gfx_set_interlace(bool enabled);
bool gfx_interlace_enabled(void);

/* Forces the next gfx_clear() to wipe the entire screen in full, resetting
 * partial clear tracking. Needed when an app opens, closes, or rotates. */
void gfx_invalidate(void);

void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color);

/* gfx_fill_rect(), but at `alpha`'s own apparent coverage (0 nothing, 255
 * solid, 16 graduated steps between) rather than solid - an ordered
 * (Bayer) dither, the cheapest fake transparency this panel can do since
 * it has no blending anywhere. See gfx.c's own comment above the
 * definition for the dither table itself and why 255 is guaranteed to be
 * exactly as solid as gfx_fill_rect(). */
void gfx_fill_rect_dither(int x, int y, int w, int h, gfx_color_t color,
                          uint8_t alpha);

/* Both clip to the framebuffer, so callers need not bounds-check. */
void gfx_pixel(int x, int y, gfx_color_t color);

/* A line from (x0, y0) to (x1, y1), both endpoints included.
 *
 * Coordinates may be anywhere, on screen or not: a line is shortened to its
 * visible part before anything is drawn, so one running far off the panel
 * costs almost nothing and one entirely off it costs a handful of compares.
 *
 * Exists because the startup animation plots a curve, and a curve is a few
 * hundred short segments - see boot_anim.c. Nothing before it needed a line
 * at all, which is why this is the newest primitive in the file. */
void gfx_line(int x0, int y0, int x1, int y1, gfx_color_t color);

/*---------------------------------------------------------------------------
 * Lines, with options
 *
 * Two independent choices - how it composites, and whether it owns its first
 * pixel - which is four combinations, and there was a third. Flags on one
 * function rather than a name per combination, because they genuinely are
 * independent and a family of gfx_line_add_open() spellings is both
 * unreadable and a standing invitation to add another.
 *
 * There WAS a GFX_LINE_SMOOTH doing Xiaolin Wu antialiasing here. It worked
 * and it was tested, and it was taken out again: on a stroke one to three
 * pixels wide it is close to invisible, because antialiasing redistributes
 * light WITHIN a pixel and what reads as a lit curve on this panel is a
 * falloff several pixels ACROSS. It cost 6.7 fps to be almost unnoticeable.
 * The thing to come back with is a wide-support filter - a distance-indexed
 * intensity table in the manner of Gupta & Sproull, whose reach is a
 * parameter - not this. See the commit that removed it.
 *
 * gfx_line() above is the no-flags case, kept as its own name because it is
 * what most callers want and reads better than passing a zero.
 *-------------------------------------------------------------------------*/

/* Add to what is already in the framebuffer instead of replacing it, so two
 * strokes crossing on a black field make a brighter, mixed colour rather than
 * whichever was drawn second. Costs a read as well as a write per pixel. */
#define GFX_LINE_ADD    (1u << 0)

/* Leave the STARTING pixel undrawn.
 *
 * For chaining segments into a polyline. Two segments that meet share a
 * pixel, and under GFX_LINE_ADD a shared pixel is added twice - so a curve
 * drawn as a few hundred short segments comes out beaded, with a brighter dot
 * at every joint. Drawing each segment half-open puts exactly one
 * contribution on every pixel of the chain.
 *
 * Only useful from the second segment onward: the first has nothing before it
 * to have drawn its start. */
#define GFX_LINE_OPEN   (1u << 1)

void gfx_line_ex(int x0, int y0, int x1, int y1, gfx_color_t color,
                 unsigned flags);

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

/* The font every gfx_text*() call above draws with. gfx_font.h's gfx_font_t
 * is what carries a font from here on; this is the one entry the scheme has
 * today. A caller that wants a specific font rather than "whatever gfx draws
 * with" - microui's mu_Font, once something honours it - asks for this. */
const gfx_font_t *gfx_default_font(void);

/* The single font-aware drawing path gfx_text(), gfx_text_scaled() and
 * gfx_text_turned() all delegate to, passing gfx_default_font(). Same
 * (x, y)-is-the-first-glyph's-cell and turn convention as gfx_text_turned().
 *
 * Only draws 1bpp glyphs - see the definition in gfx.c for why a bpp != 1
 * font is silently skipped rather than drawn wrong. */
void gfx_text_font(int x, int y, const char *text, gfx_color_t color,
                   int scale, int quarter_turns, const gfx_font_t *font);

/* gfx_text_font(), but every glyph pixel is drawn through
 * gfx_fill_rect_dither() at `alpha` instead of solid - text that fades
 * rather than cuts. A deliberately separate function, not a parameter
 * added to gfx_text_font() itself - see gfx.c's own comment above the
 * definition for why. */
void gfx_text_font_dither(int x, int y, const char *text, gfx_color_t color,
                          int scale, int quarter_turns,
                          const gfx_font_t *font, uint8_t alpha);

/* gfx_text_width()'s general form: the width `text` would draw at in
 * `font`, at `scale`. gfx_text_width() is this called with
 * gfx_default_font(). See gfx_font_text_width() in gfx_font.h for the pure
 * metric this wraps, and its own comment for the `len < 0` contract. */
int gfx_font_width(const gfx_font_t *font, const char *text, int len,
                   int scale);

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

/* A second, independent layer on top of the overlay above: draws the fixed
 * leaf-grid boundaries (see gfx_dirty.h) inside whatever box a gathered
 * send actually covers, in green, so the finer subdivision underneath a
 * yellow box is visible on real hardware rather than only in
 * suite_gfx_dirty.c. Has no effect unless gfx_debug_overlay() is also on -
 * it refines what the overlay shows, it is not a mode of its own. Same
 * undefined-outside-development reasoning as the toggle above. */
void gfx_set_leaf_overlay(bool on);
bool gfx_debug_leaf_overlay(void);

/* Per-strip counts of which send path the last stretch of gfx_present()
 * calls actually took - full-band send_full_row() versus a gathered send
 * of at least one run versus a full-width send at less than the whole
 * band's height (send_partial_band(), for a box that carries a real
 * sub-strip Y extent) - for a device test to log alongside its own timing
 * rather than guessing the split from the number alone. Reset explicitly,
 * not by gfx_present() itself, so a caller can accumulate across exactly
 * the frames it is measuring. See gfx.c's send_one_row() for where these
 * are counted. */
void gfx_reset_strip_send_counts(void);
void gfx_get_strip_send_counts(int *full_bands, int *gathered,
                               int *partial_bands);

/* Test-only: one raw esp_lcd_panel_draw_bitmap() of the whole framebuffer,
 * bypassing every dirty-tracking decision gfx_present() makes - see gfx.c
 * for what this isolates and why one wait is still correct despite the
 * driver chunking the transfer internally. */
void gfx_present_raw_full_frame_for_test(void);
#endif
