/*=============================================================================
 * boot_anim - drawing the startup animation, and the three seconds it owns.
 *
 * The maths, the layout and the timeline are all in boot_anim.h, where they
 * are host-testable. What is left here is gfx calls and one loop.
 *
 * THIS LOOP IS NOT THE SHELL'S FRAME LOOP
 *
 * docs/Launcher-Architecture.md says there is exactly one frame loop and it
 * belongs to the shell, which is a rule about APPS: an app must not loop,
 * because the shell has to stay able to switch away from it. Nothing can be
 * switched to yet at this point in boot - touch is not even running - so this
 * runs to completion before app_main() reaches its loop at all, in the same
 * way show_post_failures() already blocks on a hardware fault. It still
 * yields every frame, so the idle task keeps feeding the watchdog.
 *
 * EVERY FRAME IS A FULL REPAINT
 *
 * Which is the one thing the rest of this project works hard to avoid - see
 * ui.c on skipping unchanged canvases. It is right here and wrong there: the
 * grid is still fading up while the spirals are being drawn over it, so a
 * frame differs from the one before it almost everywhere, and there is
 * nothing to save. The clear also gets the picture back to true black, which
 * is what the dissolve at the end fades into.
 *===========================================================================*/

#include "boot_anim.h"

#include "gfx.h"
#include "intmath.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* True black, not the launcher's near-black. On an AMOLED that is the pixel
 * switched off, and a lit curve sitting on switched-off pixels is the thing
 * this panel does that a backlit one cannot. */
#define COL_BG        GFX_RGB(0x000000)
#define COL_WHITE     GFX_RGB(0xFFFFFF)

#define COL_AXIS      0x5A6478
#define COL_TICK      0x8792A8
#define COL_GRID      0x0E1524   /* quarter-unit lines */
#define COL_GRID_UNIT 0x1B2438   /* the whole-unit ones */

/* Ticks are drawn on the axes at whole units, out to this many either way.
 * Only the ones that land on screen are drawn, which at the current scale
 * means +1 on the real axis and +/-i on the imaginary one. */
#define TICK_UNITS 2
#define TICK_ARM   3            /* half-length of the cross-bar, pixels */
#define TICK_GAP   6            /* label's distance from the axis       */
#define LABEL_SCALE 1           /* 8x8 glyphs: an axis label is small   */

#define PEN_DOT 3               /* the bright square at the pen's head */

/*---------------------------------------------------------------------------
 * Colour
 *-------------------------------------------------------------------------*/

/* a * b / 255, both 0..255. Used to fold the global dissolve into whatever
 * alpha a thing already had, so one multiply takes the whole picture down
 * together rather than each element fading on its own schedule. */
static uint8_t scale8(uint8_t a, uint8_t b)
{
    return (uint8_t)(((uint32_t)a * b + 127u) / 255u);
}

/* `rgb` lifted `alpha` of the way off the background. */
static gfx_color_t lit(uint32_t rgb, uint8_t alpha)
{
    return gfx_color_mix(COL_BG, gfx_rgb(rgb), alpha);
}

/*---------------------------------------------------------------------------
 * The plane
 *-------------------------------------------------------------------------*/

/* How many grid rings fit before the furthest corner of the screen. Derived
 * rather than fixed, because the origin is off-centre: the right half needs
 * more rings than the left, and both get the same count so that a ring is
 * always a matched pair either side of the axis. */
static int grid_rings(void)
{
    const int ox = boot_anim_origin_x(GFX_WIDTH);
    const int oy = boot_anim_origin_y(GFX_HEIGHT);
    const int far = im_max(im_max(ox, GFX_WIDTH  - 1 - ox),
                           im_max(oy, GFX_HEIGHT - 1 - oy));
    return far / BOOT_ANIM_GRID_PX;
}

static void draw_grid(uint32_t now_ms, uint8_t ink)
{
    const int ox = boot_anim_origin_x(GFX_WIDTH);
    const int oy = boot_anim_origin_y(GFX_HEIGHT);
    const int rings = grid_rings();

    for (int r = 1; r <= rings; r++) {
        const uint8_t alpha = scale8(boot_anim_grid_alpha(now_ms, r), ink);
        if (alpha == 0) {
            continue;
        }
        const bool unit = (r % BOOT_ANIM_GRID_MAJOR) == 0;
        const gfx_color_t c = lit(unit ? COL_GRID_UNIT : COL_GRID, alpha);
        const int d = r * BOOT_ANIM_GRID_PX;

        for (int sign = -1; sign <= 1; sign += 2) {
            const int x = ox + sign * d;
            const int y = oy + sign * d;
            if (x >= 0 && x < GFX_WIDTH) {
                gfx_line(x, 0, x, GFX_HEIGHT - 1, c);
            }
            if (y >= 0 && y < GFX_HEIGHT) {
                gfx_line(0, y, GFX_WIDTH - 1, y, c);
            }
        }
    }
}

/* The imaginary axis is labelled i and -i rather than 1 and -1: this is one
 * complex plane, not two number lines that happen to cross. */
static const char *unit_label(int units, bool imaginary)
{
    switch (units) {
    case -2: return imaginary ? "-2i" : "-2";
    case -1: return imaginary ? "-i"  : "-1";
    case  1: return imaginary ? "i"   : "1";
    case  2: return imaginary ? "2i"  : "2";
    default: return "";
    }
}

/* A tick and its label, but only once the axis being drawn has actually
 * grown far enough out to reach it - so the marks appear in the wake of the
 * line rather than waiting for it and then arriving all at once.
 *
 * `reached` is how many pixels of axis exist so far on this side. */
static void draw_tick(int units, bool imaginary, int reached, uint8_t ink)
{
    const int ox = boot_anim_origin_x(GFX_WIDTH);
    const int oy = boot_anim_origin_y(GFX_HEIGHT);
    const int offset = units * BOOT_ANIM_UNIT_PX;

    if (im_abs(offset) > reached) {
        return;
    }

    const gfx_color_t c = lit(COL_TICK, ink);
    const char *label = unit_label(units, imaginary);

    /* Asked for at scale 1, not through gfx_text_width()/gfx_text_height(),
     * which answer for GFX_GLYPH_SCALE - the 16px size the menu is laid out
     * around. An axis label wants to be small. */
    const int lw = gfx_font_width(gfx_default_font(), label, -1, LABEL_SCALE);
    const int lh = gfx_font_height(gfx_default_font(), LABEL_SCALE);

    if (imaginary) {
        const int y = oy - offset;
        if (y < 0 || y >= GFX_HEIGHT) {
            return;
        }
        gfx_line(ox - TICK_ARM, y, ox + TICK_ARM, y, c);
        /* Left of the axis, so the labels do not sit under the spirals -
         * which all live to the right of it. */
        gfx_text_scaled(ox - TICK_GAP - lw, y - lh / 2, label, c, LABEL_SCALE);
    } else {
        const int x = ox + offset;
        if (x < 0 || x >= GFX_WIDTH) {
            return;
        }
        gfx_line(x, oy - TICK_ARM, x, oy + TICK_ARM, c);
        gfx_text_scaled(x - lw / 2, oy + TICK_GAP, label, c, LABEL_SCALE);
    }
}

static void draw_axes(uint32_t now_ms, uint8_t ink)
{
    const int ox = boot_anim_origin_x(GFX_WIDTH);
    const int oy = boot_anim_origin_y(GFX_HEIGHT);
    const uint8_t reach = boot_anim_axis_reach(now_ms);

    if (reach == 0) {
        return;
    }

    /* A fraction of each half-axis, not a fixed number of pixels: the origin
     * is off-centre, so the four arms are four different lengths and must
     * still arrive at their edges together. */
    const int left   = ox * reach / 255;
    const int right  = (GFX_WIDTH  - 1 - ox) * reach / 255;
    const int up     = oy * reach / 255;
    const int down   = (GFX_HEIGHT - 1 - oy) * reach / 255;

    const gfx_color_t c = lit(COL_AXIS, ink);
    gfx_line(ox - left, oy, ox + right, oy, c);
    gfx_line(ox, oy - up, ox, oy + down, c);

    for (int u = -TICK_UNITS; u <= TICK_UNITS; u++) {
        if (u == 0) {
            continue;
        }
        draw_tick(u, false, u < 0 ? left : right, ink);
        draw_tick(u, true,  u < 0 ? down : up,    ink);
    }
}

/*---------------------------------------------------------------------------
 * The spirals
 *-------------------------------------------------------------------------*/

static gfx_color_t stroke_colour(int k, int32_t along_q12, uint8_t ink)
{
    const boot_anim_stroke_t s = boot_anim_stroke(k, along_q12);

    gfx_color_t c = gfx_rgb(boot_anim_hue_rgb(s.hue));
    c = gfx_color_mix(c, COL_WHITE, s.bloom);
    return gfx_color_mix(COL_BG, c, scale8(s.glow, ink));
}

/* One spiral, from term one up to wherever the pen has reached.
 *
 * Redrawn from the first term every frame. The alternative - keeping the
 * points and extending the drawing - needs both an array of them and a
 * framebuffer nothing else clears, and this device has neither going spare.
 * Recomputing four whole curves costs well under a millisecond, against the
 * ~17.6 ms the frame spends on the wire regardless. */
static void draw_spiral(int k, uint32_t now_ms, uint8_t ink)
{
    const int32_t progress = boot_anim_pen(now_ms, k);
    if (progress <= 0) {
        return;
    }

    const int32_t reached = boot_anim_pen_terms(progress);
    const int     whole   = reached >> BOOT_ANIM_Q;
    const int32_t part    = reached & (BOOT_ANIM_ONE - 1);

    boot_anim_walk_t w;
    boot_anim_walk_begin(&w, k);

    int px = boot_anim_screen_x(GFX_WIDTH,  w.z.re);
    int py = boot_anim_screen_y(GFX_HEIGHT, w.z.im);

    for (int n = 1; n <= whole && n <= BOOT_ANIM_TERMS; n++) {
        boot_anim_walk_step(&w);
        const int nx = boot_anim_screen_x(GFX_WIDTH,  w.z.re);
        const int ny = boot_anim_screen_y(GFX_HEIGHT, w.z.im);
        gfx_line(px, py, nx, ny, stroke_colour(k, boot_anim_along(&w), ink));
        px = nx;
        py = ny;
    }

    /* Part way into the next term. Without this the pen would jump from one
     * point to the next, and the first few terms are a fifth of the screen
     * long apiece - two frames of nothing followed by a leap. */
    if (whole < BOOT_ANIM_TERMS) {
        const boot_anim_pt_t from = w.z;
        boot_anim_walk_step(&w);
        const int32_t re = from.re + (((w.z.re - from.re) * part) >> BOOT_ANIM_Q);
        const int32_t im = from.im + (((w.z.im - from.im) * part) >> BOOT_ANIM_Q);
        px = boot_anim_screen_x(GFX_WIDTH,  re);
        py = boot_anim_screen_y(GFX_HEIGHT, im);
        gfx_line(boot_anim_screen_x(GFX_WIDTH,  from.re),
                 boot_anim_screen_y(GFX_HEIGHT, from.im), px, py,
                 stroke_colour(k, boot_anim_along(&w), ink));

        /* A bright head, so it reads as being drawn rather than revealed. It
         * is dropped once the curve is finished - a dot left sitting on the
         * end of a static line just looks like a blemish. */
        gfx_fill_rect(px - PEN_DOT / 2, py - PEN_DOT / 2, PEN_DOT, PEN_DOT,
                      gfx_color_mix(COL_BG, COL_WHITE, ink));
    }
}

/*---------------------------------------------------------------------------
 * The loop
 *-------------------------------------------------------------------------*/

static void draw_frame(uint32_t now_ms)
{
    const uint8_t ink = boot_anim_ink(now_ms);

    gfx_clear(COL_BG);
    draw_grid(now_ms, ink);
    draw_axes(now_ms, ink);
    for (int k = 0; k < BOOT_ANIM_SPIRALS; k++) {
        draw_spiral(k, now_ms, ink);
    }
}

void boot_anim_run(void)
{
    const int64_t started_us = esp_timer_get_time();

    for (;;) {
        const int64_t elapsed_us = esp_timer_get_time() - started_us;
        const uint32_t now_ms = (uint32_t)(elapsed_us / 1000);
        if (now_ms >= BOOT_ANIM_MS) {
            break;
        }

        draw_frame(now_ms);
        gfx_present();

        /* Same yield the shell's loop makes, for the same reason: the idle
         * task feeds the watchdog. */
        vTaskDelay(1);
    }

    /* Left black on purpose. The launcher repaints the whole screen on its
     * first frame - ui_init() invalidates - so there is no need to spend
     * another full transfer clearing it here, and the last frame drawn was
     * already all but faded out. */
}
