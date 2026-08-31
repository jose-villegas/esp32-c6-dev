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

#include "boot/boot_anim.h"

#include "display/display.h"
#include "gfx/gfx.h"
#include "util/fixed.h"
#include "util/intmath.h"

/* Only boot_anim_run() itself, at the bottom of this file, touches
 * ESP-IDF/FreeRTOS - see gfx.c's own ESP_PLATFORM comment for why that is
 * the natural, zero-plumbing switch for a host build (a plain `gcc`
 * invocation never defines it). boot_anim_draw_frame() above it is plain
 * gfx calls and already took `now_ms` as a parameter rather than reading
 * the clock itself, so it needs none of this. */
#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "boot_anim";
#endif

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

/* `rgb`, first mixed `whiten` of the way toward white, then lifted `alpha`
 * of the way off the background - see boot_anim_grid_whiten()'s own
 * comment for why the floor needs the extra mix and lit() alone does not:
 * a saturated hue only ever gets more OPAQUE through lit() on its own,
 * never any less colour-muddy, and it is the colour itself moving toward
 * white that is supposed to be doing most of the work of making the grid
 * read here. */
static gfx_color_t lit_whitened(uint32_t rgb, uint8_t whiten, uint8_t alpha)
{
    const gfx_color_t whitened = gfx_color_mix(gfx_rgb(rgb), COL_WHITE, whiten);
    return gfx_color_mix(COL_BG, whitened, alpha);
}

/*---------------------------------------------------------------------------
 * Projection helpers
 *
 * Thin wrappers that pin the panel size, so the rest of this file talks in
 * world coordinates and never repeats GFX_WIDTH/GFX_HEIGHT.
 *-------------------------------------------------------------------------*/

static int sx(int32_t re, int32_t im, const boot_anim_view_t *view)
{
    return boot_anim_screen_x(re, im, view);
}

/* `view` carries the frame's camera state - see boot_anim_view() in
 * boot_anim.h - so it is threaded down alongside now_ms and ink rather than
 * recomputed here: it is the same for every point drawn this frame, and the
 * trig it is built from is worth paying for once, not per point. */
static int sy(int32_t re, int32_t im, int32_t t, const boot_anim_view_t *view)
{
    return boot_anim_screen_y(GFX_HEIGHT, re, im, t, view);
}

/* sx/sy, shrunk toward the origin by shrink_q8/256 - see
 * boot_anim_motif_shrink_q8() in boot_anim.h for why the axes need this
 * alongside the curve during the finale. draw_floor() shrinks too, but
 * rides its own boot_anim_grid_shrink_q8() instead of going through this -
 * see that function's own comment for why the floor's shrink is worked
 * out on `d` before projection rather than on the drawn pixel the way
 * this is. Applied to the already-projected pixel, not to (re, im, t): the
 * projection is linear, so scaling the offset from the origin after
 * projecting is the same picture as scaling the point before, without
 * needing a scaled copy of every sample. */
static int csx(int32_t re, int32_t im, int shrink_q8, const boot_anim_view_t *view)
{
    const int raw = sx(re, im, view);
    return view->ox + (((raw - view->ox) * shrink_q8) >> 8);
}

static int csy(int32_t re, int32_t im, int32_t t, int shrink_q8,
              const boot_anim_view_t *view)
{
    const int raw = sy(re, im, t, view);
    return view->oy + (((raw - view->oy) * shrink_q8) >> 8);
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

/* Each ring is a pair of crossing lines - one horizontal, one vertical, on
 * each side of the origin - bounded to `far`, the outermost ring's own
 * reach, rather than a closed square outline. A closed square only ever
 * draws a ring's own PERIMETER; it never crosses another ring's lines, so
 * no matter how many of them nest together the result is concentric
 * outlines, not a grid with cells - a direction was missing. Crossing
 * lines are what an actual square grid is made of: every ring's pair
 * crosses every OTHER ring's pair too, and it is that lattice of
 * intersections, not just the outermost boundary, that reads as a mesh of
 * cells. (An earlier version of this drew crossing lines that ran
 * unbounded past the panel - the same idea, but with too few, too dim
 * rings and a broken shrink collapsing them all toward the origin, the
 * result read as a bare cross rather than a grid; bounded, dense and
 * bright, the same construction reads as intended.)
 *
 * Its OWN shrink, not boot_anim_motif_shrink_q8() directly - see
 * boot_anim_grid_shrink_q8()'s own comment: the grid still rides that
 * same GROW/HOLD/SETTLE pulse, because the grid IS the plane the curve
 * and axes are drawn against and ought to read as the same scale
 * changing, not a separate thing standing still - but clamped to never
 * drop below a floor sized to cover every one of the panel's four
 * corners for the WHOLE animation, from the very first frame, which
 * boot_anim_motif_shrink_q8() alone cannot do: it starts small and grows
 * into place, and there is always a window before it has grown, and it
 * settles small again for the CURVE's own panel-fit safety, a completely
 * different constraint from what the grid needs. Applied to `d`/`far`
 * (world units, before projection) rather than via csx()/csy() on the
 * drawn endpoints, so the whole grid scales uniformly rather than each
 * endpoint sliding independently toward view->ox/oy. */
static void draw_floor(uint32_t now_ms, uint8_t ink,
                       const boot_anim_view_t *view)
{
    const int shrink_q8 = boot_anim_grid_shrink_q8(now_ms);
    const int32_t far = ((int32_t)BOOT_ANIM_GRID_RINGS * BOOT_ANIM_GRID_STEP_Q12 *
                         shrink_q8) >> 8;

    for (int ring = 1; ring <= BOOT_ANIM_GRID_RINGS; ring++) {
        const uint8_t alpha = scale8(boot_anim_grid_alpha(now_ms, ring), ink);
        if (alpha == 0) {
            continue;       /* faded out with distance - and so is everything
                             * beyond it, but the loop is nine long */
        }

        /* The floor is the one thing on screen the whole time that is not
         * doing anything, so it is where a slow colour drift costs nothing
         * and competes with nothing. Whitened, not just lit(): see
         * boot_anim_grid_whiten()'s own comment for why the hue itself
         * climbs toward white over the whole animation rather than only
         * getting more opaque. */
        const gfx_color_t c = lit_whitened(
            boot_anim_hue_rgb(boot_anim_grid_hue(now_ms, ring)),
            boot_anim_grid_whiten(now_ms), alpha);
        /* BOOT_ANIM_GRID_STEP_Q12, not units() (a whole unit) - see
         * BOOT_ANIM_GRID_RINGS's own comment on why the rings are a
         * quarter of a unit apart now. */
        const int32_t d = ((int32_t)ring * BOOT_ANIM_GRID_STEP_Q12 * shrink_q8) >> 8;

        for (int sign = -1; sign <= 1; sign += 2) {
            const int32_t off = sign * d;

            gfx_line_ex(sx(off, -far, view), sy(off, -far, 0, view),
                        sx(off,  far, view), sy(off,  far, 0, view), c, 0u);
            gfx_line_ex(sx(-far, off, view), sy(-far, off, 0, view),
                        sx( far, off, view), sy( far, off, 0, view), c, 0u);
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
                     uint8_t ink, int shrink_q8, const boot_anim_view_t *view)
{
    const int32_t fre = tween_lerp_i32(0, re, reach);
    const int32_t fim = tween_lerp_i32(0, im, reach);
    const int32_t ft  = tween_lerp_i32(0, t,  reach);

    gfx_line_ex(csx(0, 0, shrink_q8, view), csy(0, 0, 0, shrink_q8, view),
                csx(fre, fim, shrink_q8, view),
                csy(fre, fim, ft, shrink_q8, view),
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

static void draw_axes(uint32_t now_ms, uint8_t ink,
                      const boot_anim_view_t *view)
{
    const uint8_t reach = boot_anim_axis_reach(now_ms);
    if (reach == 0) {
        return;
    }

    /* Short at first - the axes stop where a reader can still see them end,
     * so there is somewhere to put a label - then unbounded once the finale
     * starts, the same "run it well past the panel and let clipping do the
     * work" treatment the floor already gets. `finale` interpolates the arm
     * length between the two; see BOOT_ANIM_AXIS_FAR_UNITS in boot_anim.h. */
    const uint8_t finale = boot_anim_finale_reach(now_ms);
    const int32_t short_arm = units(4);
    const int32_t long_arm  = units(BOOT_ANIM_AXIS_FAR_UNITS);
    const int32_t arm = tween_lerp_i32(short_arm, long_arm, finale);

    /* T's own scale is Q8, not Q12 like re/im - units() (Q12) has no
     * business appearing here, which is exactly the mix-up the header's own
     * "FIXED POINT, AND FOUR SCALES OF IT" warns about. Both ends are a
     * plain unit count shifted into Q8 directly.
     *
     * short_top is sized from BOOT_ANIM_T_MAX_PHASE1, not BOOT_ANIM_T_MAX -
     * see that constant's own comment in boot_anim.h: this is the SHORT,
     * labelled arm drawn before the finale unbounds it, and it must stay
     * the length it was tuned to fit the panel at even though the climb
     * itself now reaches twice as high. */
    const int32_t short_top = (BOOT_ANIM_T_MAX_PHASE1 + 1) << BOOT_ANIM_TQ;
    const int32_t long_top  = (BOOT_ANIM_T_MAX * 3) << BOOT_ANIM_TQ;
    const int32_t top = tween_lerp_i32(short_top, long_top, finale);

    const int shrink_q8 = boot_anim_motif_shrink_q8(now_ms);
    draw_arm(arm, 0, 0, reach, ink, shrink_q8, view);      /* real      */
    draw_arm(0, arm, 0, reach, ink, shrink_q8, view);      /* imaginary */
    draw_arm(0, 0, top, reach, ink, shrink_q8, view);      /* t         */

    /* Named rather than tick-marked. Which axis is which is the one thing a
     * reader cannot work out from the picture, and three short labels say it
     * where a ladder of numbers up a 315px axis would just be clutter.
     *
     * Only before the finale starts: an unbounded axis has nowhere left to
     * anchor a label to, so rather than have one drift or clip, it is
     * simplest to let the labels belong to the short-arm phase only and
     * drop away once the arms start growing past it. */
    if (reach == 255 && finale == 0) {
        draw_label(sx(arm, 0, view) + LABEL_GAP * 2,
                   sy(arm, 0, 0, view) + LABEL_GAP, "Re", ink);
        draw_label(sx(0, arm, view) - LABEL_GAP * 2,
                   sy(0, arm, 0, view) + LABEL_GAP, "Im", ink);
        draw_label(sx(0, 0, view) + LABEL_GAP * 2,
                   sy(0, 0, top, view) - LABEL_GAP, "t", ink);
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
static void draw_zeros(int32_t pen_t_q8, uint8_t ink, int shrink_q8,
                       const boot_anim_view_t *view)
{
    for (int i = 0; i < BOOT_ANIM_ZEROS; i++) {
        const int32_t t = boot_anim_zero_t[i];
        if (t > pen_t_q8) {
            break;      /* the table is in order, so nothing after it either */
        }
        gfx_fill_rect(csx(0, 0, shrink_q8, view) - ZERO_DOT / 2,
                      csy(0, 0, t, shrink_q8, view) - ZERO_DOT / 2,
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
/* `colour_pen` is boot_anim_colour_progress(), not boot_anim_pen() - see
 * its own comment in boot_anim.h. It is a Q12 fraction of PHASE1's length,
 * not the whole (now longer) table, so a trail's raw position in it has to
 * be converted back into an actual table index - `* phase1_span` undoes
 * the `/ phase1_span` boot_anim_colour_progress() applied - before it can
 * be used to sample the curve; boot_anim_stroke() and boot_anim_trail_pos()
 * want the un-converted, colour-scale value instead, exactly as before. */
static void draw_heads(int32_t colour_pen, uint8_t ink, int shrink_q8,
                       const boot_anim_view_t *view)
{
    const int32_t phase1_span = (int32_t)(BOOT_ANIM_CURVE_PHASE1_POINTS - 1);

    for (int k = 0; k < BOOT_ANIM_TRAILS; k++) {
        const int32_t at = boot_anim_trail_pos(colour_pen, k);
        if (at <= 0) {
            continue;       /* not set off yet */
        }

        int i = fx_mul_floor(at, phase1_span, BOOT_ANIM_Q);
        if (i >= BOOT_ANIM_CURVE_POINTS) {
            i = BOOT_ANIM_CURVE_POINTS - 1;   /* the table ran out first */
        }
        const boot_anim_pt_t p = boot_anim_sample(i);

        draw_head(csx(p.re, p.im, shrink_q8, view),
                  csy(p.re, p.im, p.t, shrink_q8, view),
                  boot_anim_hue_rgb(boot_anim_stroke(at, colour_pen).hue), ink);
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
static int32_t draw_curve(uint32_t now_ms, uint8_t ink,
                          const boot_anim_view_t *view)
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
    const int shrink_q8 = boot_anim_motif_shrink_q8(now_ms);

    /* Colours the trail, not how much of the curve is drawn - see
     * boot_anim_colour_progress()'s own comment. `pen` (extent) still
     * decides `last`/`part` above: the curve itself stops growing once
     * fully drawn, only the light chasing round it keeps moving. Also what
     * `along` below is measured against, in place of `span` - see the same
     * comment for why a colour position is phase-1-relative rather than a
     * fraction of the whole table. */
    const int32_t colour = boot_anim_colour_progress(now_ms);
    const int32_t phase1_span = (int32_t)(BOOT_ANIM_CURVE_PHASE1_POINTS - 1);

    boot_anim_pt_t head = boot_anim_sample(0);
    int px = csx(head.re, head.im, shrink_q8, view);
    int py = csy(head.re, head.im, head.t, shrink_q8, view);
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
        const int32_t a0 = fx_div_round(i, phase1_span, BOOT_ANIM_Q);
        const int32_t a1 = fx_div_round(i + 1, phase1_span, BOOT_ANIM_Q);

        /* A partial span for the one the pen is inside: without it the head
         * would jump from sample to sample, and a sample is six pixels. */
        const int32_t limit = (i == last) ? part : BOOT_ANIM_ONE;

        for (int step = 1; step <= BOOT_ANIM_SPLINE_STEPS; step++) {
            const int32_t t = (limit * step) / BOOT_ANIM_SPLINE_STEPS;

            head = boot_anim_spline(c0, c1, c2, t);
            const int nx = csx(head.re, head.im, shrink_q8, view);
            const int ny = csy(head.re, head.im, head.t, shrink_q8, view);
            const int32_t along = a0 + (((a1 - a0) * t) >> BOOT_ANIM_Q);

            draw_stroke(px, py, nx, ny, boot_anim_stroke(along, colour), ink,
                        drawn);
            drawn = true;
            px = nx;
            py = ny;
        }
    }

    draw_heads(colour, ink, shrink_q8, view);
    return head.t;
}

/*---------------------------------------------------------------------------
 * The title
 *-------------------------------------------------------------------------*/

/* boot_anim_title_letter() lays the word out in the VIEWER's frame - see its
 * own comment in boot_anim.h - because this board is held a quarter turn
 * from its native upright (DISPLAY_LANDSCAPE, the same fact display.h and
 * main.c use to turn the rest of the shell once touch is running). Nothing
 * else this file draws needs correcting for that: a spiral and a floor grid
 * have no reading direction, so they look right on the raw panel either way,
 * but a WORD does, which is the only reason this is the one thing here that
 * has to be turned before it is drawn.
 *
 * A point in that frame is not simply a point on the panel, though - the
 * glyph itself has to rotate too, and rotating a box (the glyph's cell)
 * is not the same as rotating its corner - see ui.c's own comment on
 * MU_COMMAND_TEXT for the exact trap that is (a rotated glyph drawn at a
 * merely-rotated corner drifts off by its own height). This mirrors that:
 * the physical origin is the rotated box's own top-left, not the rotated
 * point. */
static void title_glyph_origin(int view_x, int view_y, int glyph_w,
                               int glyph_h, int *panel_x, int *panel_y)
{
    (void)glyph_w;   /* DISPLAY_LANDSCAPE only needs the box's height to
                      * correct the origin - see the two mapped corners in
                      * this function's own derivation above. */
    *panel_x = GFX_WIDTH - view_y - glyph_h;
    *panel_y = view_x;
}

/* "Autana", flying in - see boot_anim_title_letter() in boot_anim.h for the
 * choreography; this is only gfx calls. White rather than a hue-wheel
 * colour, deliberately: the word is the one thing on screen that is not
 * part of the curve, and it stays legible against whatever the spiral is
 * doing behind it precisely because it does not compete on colour. */
static void draw_title(uint32_t now_ms, uint8_t ink)
{
    const gfx_color_t c = gfx_color_mix(COL_BG, COL_WHITE, ink);
    const gfx_font_t *font = gfx_default_font();
    const int glyph_w = gfx_font_width(font, "A", -1, BOOT_ANIM_TITLE_SCALE);
    const int glyph_h = gfx_font_height(font, BOOT_ANIM_TITLE_SCALE);
    char one[2] = { 0, 0 };

    for (int i = 0; i < BOOT_ANIM_TITLE_LEN; i++) {
        const boot_anim_title_pos_t p = boot_anim_title_letter(i, now_ms);
        int px, py;
        title_glyph_origin(p.x, p.y, glyph_w, glyph_h, &px, &py);

        one[0] = BOOT_ANIM_TITLE[i];
        gfx_text_font(px, py, one, c, BOOT_ANIM_TITLE_SCALE,
                     DISPLAY_LANDSCAPE, font);
    }
}

/*---------------------------------------------------------------------------
 * The loop
 *-------------------------------------------------------------------------*/

void boot_anim_draw_frame(uint32_t now_ms)
{
    const uint8_t ink = boot_anim_ink(now_ms);

    /* Reads now_ms directly and derives everything from it internally - see
     * boot_anim_view()'s own comment in boot_anim.h for why the curve's
     * progress and the finale's each drive their own piece of it. Built
     * once here and threaded to every draw_* call below rather than
     * reconstructed per point: the trig it costs is worth paying for once
     * a frame. */
    const boot_anim_view_t view = boot_anim_view(GFX_WIDTH, GFX_HEIGHT,
                                                 now_ms);

    gfx_clear(COL_BG);
    const int shrink_q8 = boot_anim_motif_shrink_q8(now_ms);
    draw_floor(now_ms, ink, &view);
    draw_axes(now_ms, ink, &view);

    /* Zeros before the curve, so the curve's own glow lands on top of them
     * rather than the dots punching holes in it. */
    const int32_t reached = draw_curve(now_ms, ink, &view);
    draw_zeros(reached, ink, shrink_q8, &view);

    /* Gated rather than always called: every letter's own ramp is already
     * zero before BOOT_ANIM_TITLE_START_MS, so skipping the six gfx calls
     * entirely for the 2.7s before that is a real saving, not just tidiness. */
    if (now_ms >= BOOT_ANIM_TITLE_START_MS) {
        draw_title(now_ms, ink);
    }
}

#ifdef ESP_PLATFORM
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

        boot_anim_draw_frame(now_ms);
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
#endif
