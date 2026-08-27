/*=============================================================================
 * boot_anim - drawing the startup animation, and the five seconds it owns.
 *
 * The projection, the smoothing, the colour and the timeline are all in
 * boot_anim.h, where they are host-testable, and the curve is a generated
 * table in boot_anim_curve.h. What is left here is gfx calls and one loop.
 *
 * THIS LOOP IS NOT THE SHELL'S FRAME LOOP
 *
 * docs/Launcher-Architecture.md says there is exactly one frame loop and it
 * belongs to the shell, which is a rule about APPS: an app must not loop,
 * because the shell has to stay able to switch away from it. Nothing can be
 * switched to yet at this point in boot - touch is not even running - so this
 * runs to completion before app_main() reaches its loop at all, the same way
 * show_post_failures() already blocks on a hardware fault. It still yields
 * every frame, so the idle task keeps feeding the watchdog.
 *
 * EVERY FRAME IS A FULL REPAINT
 *
 * Which is the one thing the rest of this project works hard to avoid - see
 * ui.c on skipping unchanged canvases. It is right there and wrong here: the
 * trail behind the pen re-colours a long stretch of curve every frame, so a
 * frame differs from the one before it almost everywhere and there is nothing
 * to save. The clear also gets the picture back to true black, which is both
 * what the additive strokes need underneath them and what the dissolve at the
 * end fades into.
 *===========================================================================*/

#include "boot_anim.h"

#include "gfx.h"
#include "intmath.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "boot_anim";

/* True black, not the launcher's near-black. On an AMOLED that is the pixel
 * switched off, and lit colour on switched-off pixels is the thing this panel
 * does that a backlit one cannot. */
#define COL_BG        GFX_RGB(0x000000)
#define COL_WHITE     GFX_RGB(0xFFFFFF)

#define COL_AXIS      0x5A6478
#define COL_TICK      0x8792A8
#define COL_GRID      0x121A2B
#define COL_ZERO      0xFFFFFF

#define LABEL_SCALE 1           /* 8x8 glyphs: an axis label is small */
#define LABEL_GAP   5

/* The bright head of the pen: three nested squares rather than a circle,
 * because at this size the difference is a couple of corner pixels and this
 * is three fills. */
#define HEAD_OUTER 13
#define HEAD_MID   8
#define HEAD_CORE  4

/* A zero of zeta, marked on the t axis where the curve crosses it. */
#define ZERO_DOT 5

/*---------------------------------------------------------------------------
 * Colour
 *-------------------------------------------------------------------------*/

/* a * b / 255, both 0..255. Folds the global dissolve into whatever alpha a
 * thing already had, so one multiply takes the whole picture down together
 * rather than each element fading on its own schedule. */
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
 * Projection helpers
 *
 * Thin wrappers that pin the panel size, so the rest of this file talks in
 * world coordinates and never repeats GFX_WIDTH/GFX_HEIGHT.
 *-------------------------------------------------------------------------*/

static int sx(int32_t re, int32_t im)
{
    return boot_anim_screen_x(GFX_WIDTH, re, im);
}

static int sy(int32_t re, int32_t im, int32_t t)
{
    return boot_anim_screen_y(GFX_HEIGHT, re, im, t);
}

/* A whole number of grid units, as a Q12 value. */
static int32_t units(int n)
{
    return (int32_t)n * BOOT_ANIM_ONE;
}

/*---------------------------------------------------------------------------
 * The floor
 *
 * The complex plane zeta's value lives in, drawn as a grid at t = 0. Giving
 * it a floor rather than leaving the two axes bare is what makes the third
 * axis read as height instead of as a third line through the same point.
 *-------------------------------------------------------------------------*/

/* How far a floor line is run before it is handed to gfx_line(). Well past
 * the panel on purpose: gfx_line() clips, so the off-screen part costs a few
 * compares, and running the lines out rather than stopping them at a tidy
 * boundary is what removes the floor's edge. */
#define FLOOR_REACH 24

static void draw_floor(uint32_t now_ms, uint8_t ink)
{
    const int32_t far = units(FLOOR_REACH);

    for (int ring = 1; ring <= BOOT_ANIM_GRID_RINGS; ring++) {
        const uint8_t alpha = scale8(boot_anim_grid_alpha(now_ms, ring), ink);
        if (alpha == 0) {
            continue;       /* faded out with distance - and so is everything
                             * beyond it, but the loop is nine long */
        }

        /* The floor is the one thing on screen the whole time that is not
         * doing anything, so it is where a slow colour drift costs nothing
         * and competes with nothing. */
        const gfx_color_t c =
            lit(boot_anim_hue_rgb(boot_anim_grid_hue(now_ms, ring)), alpha);
        const int32_t d = units(ring);

        /* Four lines per ring: two either side of the real axis, two either
         * side of the imaginary one, each run past the edge of the panel. */
        for (int sign = -1; sign <= 1; sign += 2) {
            const int32_t off = sign * d;

            gfx_line_ex(sx(off, -far), sy(off, -far, 0),
                        sx(off,  far), sy(off,  far, 0), c, 0u);
            gfx_line_ex(sx(-far, off), sy(-far, off, 0),
                        sx( far, off), sy( far, off, 0), c, 0u);
        }
    }
}

/*---------------------------------------------------------------------------
 * The axes
 *-------------------------------------------------------------------------*/

/* One axis arm, grown `reach`/255 of the way from the origin to (re, im, t).
 *
 * A fraction rather than a pixel count, so the three arms - which are three
 * different lengths on screen - still arrive at their ends together. */
static void draw_arm(int32_t re, int32_t im, int32_t t, uint8_t reach,
                     uint8_t ink)
{
    const int32_t fre = (re * reach) / 255;
    const int32_t fim = (im * reach) / 255;
    const int32_t ft  = (t  * reach) / 255;

    gfx_line_ex(sx(0, 0), sy(0, 0, 0), sx(fre, fim), sy(fre, fim, ft),
                lit(COL_AXIS, ink), 0u);
}

static void draw_label(int x, int y, const char *text, uint8_t ink)
{
    /* Asked for at LABEL_SCALE rather than through gfx_text_width(), which
     * answers for GFX_GLYPH_SCALE - the 16px size the menu is laid out
     * around. An axis label wants to be small. */
    const int w = gfx_font_width(gfx_default_font(), text, -1, LABEL_SCALE);
    const int h = gfx_font_height(gfx_default_font(), LABEL_SCALE);

    gfx_text_scaled(x - w / 2, y - h / 2, text, lit(COL_TICK, ink),
                    LABEL_SCALE);
}

static void draw_axes(uint32_t now_ms, uint8_t ink)
{
    const uint8_t reach = boot_anim_axis_reach(now_ms);
    if (reach == 0) {
        return;
    }

    /* The axes stop where a reader can still see them end, rather than
     * running out with the floor: an unbounded axis has nowhere to put its
     * label. */
    const int32_t arm = units(4);
    const int32_t top = (BOOT_ANIM_T_MAX << BOOT_ANIM_TQ) +
                        (1 << BOOT_ANIM_TQ);

    draw_arm(arm, 0, 0, reach, ink);      /* real      */
    draw_arm(0, arm, 0, reach, ink);      /* imaginary */
    draw_arm(0, 0, top, reach, ink);      /* t         */

    /* Named rather than tick-marked. Which axis is which is the one thing a
     * reader cannot work out from the picture, and three short labels say it
     * where a ladder of numbers up a 315px axis would just be clutter. */
    if (reach == 255) {
        draw_label(sx(arm, 0) + LABEL_GAP * 2, sy(arm, 0, 0) + LABEL_GAP,
                   "Re", ink);
        draw_label(sx(0, arm) - LABEL_GAP * 2, sy(0, arm, 0) + LABEL_GAP,
                   "Im", ink);
        draw_label(sx(0, 0) + LABEL_GAP * 2, sy(0, 0, top) - LABEL_GAP, "t",
                   ink);
    }
}

/*---------------------------------------------------------------------------
 * The zeros
 *-------------------------------------------------------------------------*/

/* A dot on the t axis for each zero the pen has climbed past.
 *
 * These are the only points of the whole picture that mean anything on their
 * own: the curve touching the axis is zeta being zero, and the height it
 * happens at is one of the numbers the Riemann hypothesis is about. Drawn
 * white and flat rather than blended, so they stay legible through whatever
 * the curve is doing around them. */
static void draw_zeros(int32_t pen_t_q8, uint8_t ink)
{
    for (int i = 0; i < BOOT_ANIM_ZEROS; i++) {
        const int32_t t = boot_anim_zero_t[i];
        if (t > pen_t_q8) {
            break;      /* the table is in order, so nothing after it either */
        }
        gfx_fill_rect(sx(0, 0) - ZERO_DOT / 2, sy(0, 0, t) - ZERO_DOT / 2,
                      ZERO_DOT, ZERO_DOT, lit(COL_ZERO, ink));
    }
}

/*---------------------------------------------------------------------------
 * The curve
 *-------------------------------------------------------------------------*/

/* One piece of curve. Added to what is underneath rather than written over
 * it, so the places where the curve crosses itself - which is most of the
 * middle of the picture - come out brighter and mixed instead of showing
 * whichever piece happened to be drawn last. */
/* `joined` says this segment continues one already drawn, so its starting
 * pixel has been put down already - see GFX_LINE_OPEN in gfx.h. */
static void draw_stroke(int x0, int y0, int x1, int y1,
                        boot_anim_stroke_t s, uint8_t ink, bool joined)
{
    gfx_color_t c = gfx_rgb(boot_anim_hue_rgb(s.hue));
    c = gfx_color_mix(c, COL_WHITE, s.bloom);
    c = gfx_color_mix(COL_BG, c, scale8(s.glow, ink));

    /* Width by repeating the stroke across whichever axis the segment is
     * shorter on - the cheapest wide line that does not leave gaps on a
     * diagonal. Offsets are spread either side of the centre so the curve
     * thickens where it is rather than drifting sideways as it fattens. */
    const int half = s.width / 2;
    const bool shallow = im_abs(x1 - x0) > im_abs(y1 - y0);

    for (int i = 0; i < s.width; i++) {
        const int off = i - half;
        const int ax = shallow ? x0 : x0 + off;
        const int ay = shallow ? y0 + off : y0;
        const int bx = shallow ? x1 : x1 + off;
        const int by = shallow ? y1 + off : y1;

        gfx_line_ex(ax, ay, bx, by, c,
                    GFX_LINE_ADD | (joined ? GFX_LINE_OPEN : 0u));
    }
}

/* A filled disc, one horizontal span per row.
 *
 * Round rather than the three nested squares this used to be. At nine pixels
 * a square passes for a dot; at thirteen it stops being a pen and starts
 * being a box sliding along the curve, which is what the bigger brush made
 * obvious. The inner loop walks out at most six pixels to find the span, so
 * no square root is needed and the whole thing is still a handful of fills. */
static void draw_disc(int x, int y, int diameter, gfx_color_t c)
{
    const int r = diameter / 2;

    for (int dy = -r; dy <= r; dy++) {
        int dx = r;
        while (dx > 0 && dx * dx + dy * dy > r * r) {
            dx--;
        }
        gfx_fill_rect(x - dx, y + dy, 2 * dx + 1, 1, c);
    }
}

static void draw_head(int x, int y, uint32_t rgb, uint8_t ink)
{
    draw_disc(x, y, HEAD_OUTER, lit(rgb, scale8(70, ink)));
    draw_disc(x, y, HEAD_MID, lit(rgb, scale8(215, ink)));
    draw_disc(x, y, HEAD_CORE, gfx_color_mix(COL_BG, COL_WHITE, ink));
}

/* Every pen that is currently on the curve.
 *
 * Their positions are re-derived from the curve rather than remembered,
 * because the curve is walked from the start every frame anyway - so a pen
 * is just an index into it, and there is no per-pen state to keep in step.
 *
 * The leading pen is skipped once it reaches the end: a bright dot left
 * sitting on the finish of a static curve reads as a blemish rather than as
 * a pen. */
static void draw_heads(int32_t pen, uint8_t ink)
{
    const int32_t span = (int32_t)(BOOT_ANIM_CURVE_POINTS - 1);

    for (int k = 0; k < BOOT_ANIM_TRAILS; k++) {
        const int32_t at = boot_anim_trail_pos(pen, k);
        if (at <= 0 || at >= BOOT_ANIM_ONE) {
            continue;       /* not set off yet, or finished */
        }

        const int i = (int)(((int64_t)at * span) >> BOOT_ANIM_Q);
        const boot_anim_pt_t p = boot_anim_sample(i);

        draw_head(sx(p.re, p.im), sy(p.re, p.im, p.t),
                  boot_anim_hue_rgb(boot_anim_stroke(at, pen).hue), ink);
    }
}

/* The curve, from the start up to wherever the pen has reached.
 *
 * Redrawn in full every frame. Not laziness: the trail re-colours everything
 * within half the curve's length of the head, so most of what is on screen
 * genuinely changes from one frame to the next, and the alternative would be
 * a second framebuffer this device does not have.
 *
 * Returns the height the pen has climbed to, which is what decides how many
 * of the zeros have been marked. */
static int32_t draw_curve(uint32_t now_ms, uint8_t ink)
{
    const int32_t pen = boot_anim_pen(now_ms);
    if (pen <= 0) {
        return 0;
    }

    /* The pen's position along the table, in Q12 samples. The samples are
     * evenly spaced on screen, so this is also its position along the curve
     * as drawn. */
    const int32_t span = (int32_t)(BOOT_ANIM_CURVE_POINTS - 1);
    /* pen is already a Q12 FRACTION, so multiplying by the number of spans
     * gives a Q12 sample index directly - there is no second shift to do,
     * and doing one anyway lands the pen permanently on sample zero. */
    const int32_t at = pen * span;
    const int last = at >> BOOT_ANIM_Q;
    const int32_t part = at & (BOOT_ANIM_ONE - 1);

    boot_anim_pt_t head = boot_anim_sample(0);
    int px = sx(head.re, head.im);
    int py = sy(head.re, head.im, head.t);
    bool drawn = false;      /* has any segment been laid down yet? */

    for (int i = 0; i <= last && i < BOOT_ANIM_CURVE_POINTS; i++) {
        /* The span centred on sample i, cutting the corner there. Clamped
         * indices at both ends pin the curve to its first and last sample. */
        const boot_anim_pt_t c0 = boot_anim_sample(i - 1);
        const boot_anim_pt_t c1 = boot_anim_sample(i);
        const boot_anim_pt_t c2 = boot_anim_sample(i + 1);

        /* How far into the whole curve this span sits, and how far the next
         * one does, so the colour can be interpolated across it rather than
         * stepping once per sample. */
        const int32_t a0 = (int32_t)(((int64_t)i * BOOT_ANIM_ONE) / span);
        const int32_t a1 = (int32_t)(((int64_t)(i + 1) * BOOT_ANIM_ONE) / span);

        /* A partial span for the one the pen is inside: without it the head
         * would jump from sample to sample, and a sample is six pixels. */
        const int32_t limit = (i == last) ? part : BOOT_ANIM_ONE;

        for (int step = 1; step <= BOOT_ANIM_SPLINE_STEPS; step++) {
            const int32_t t = (limit * step) / BOOT_ANIM_SPLINE_STEPS;

            head = boot_anim_spline(c0, c1, c2, t);
            const int nx = sx(head.re, head.im);
            const int ny = sy(head.re, head.im, head.t);
            const int32_t along = a0 + (((a1 - a0) * t) >> BOOT_ANIM_Q);

            draw_stroke(px, py, nx, ny, boot_anim_stroke(along, pen), ink,
                        drawn);
            drawn = true;
            px = nx;
            py = ny;
        }
    }

    draw_heads(pen, ink);
    return head.t;
}

/*---------------------------------------------------------------------------
 * The loop
 *-------------------------------------------------------------------------*/

static void draw_frame(uint32_t now_ms)
{
    const uint8_t ink = boot_anim_ink(now_ms);

    gfx_clear(COL_BG);
    draw_floor(now_ms, ink);
    draw_axes(now_ms, ink);

    /* Zeros before the curve, so the curve's own glow lands on top of them
     * rather than the dots punching holes in it. */
    const int32_t reached = draw_curve(now_ms, ink);
    draw_zeros(reached, ink);
}

void boot_anim_run(void)
{
    const int64_t started_us = esp_timer_get_time();
    uint32_t frames = 0;

    for (;;) {
        const int64_t elapsed_us = esp_timer_get_time() - started_us;
        const uint32_t now_ms = (uint32_t)(elapsed_us / 1000);
        if (now_ms >= BOOT_ANIM_MS) {
            break;
        }

        draw_frame(now_ms);
        gfx_present();
        frames++;

        /* The same yield the shell's loop makes, for the same reason: the
         * idle task feeds the watchdog. */
        vTaskDelay(1);
    }

    /* Reported because it is the one thing about this that cannot be checked
     * anywhere but on the board. Everything else here is covered on a host;
     * whether a full-screen repaint plus a 322 KiB transfer leaves room for
     * the curve is a question only the real panel answers. */
    ESP_LOGI(TAG, "%u frames in %d ms (%.1f fps)", (unsigned)frames,
             BOOT_ANIM_MS, (double)frames * 1000.0 / BOOT_ANIM_MS);

    /* Left black on purpose. The launcher repaints the whole screen on its
     * first frame - ui_init() invalidates - so there is no need to spend
     * another full transfer clearing it here, and the last frame drawn was
     * already all but faded out. */
}
