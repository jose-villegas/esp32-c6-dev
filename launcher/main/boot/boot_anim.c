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
 * what the additive strokes need underneath them and (until the photograph
 * arrives - see draw_image()'s own comment on why that phase composites
 * rather than clearing into) what the dissolve at the end fades into.
 *===========================================================================*/

#include "boot/boot_anim.h"

#include <string.h>

#include "boot/boot_anim_image.h"
#include "display/display.h"
#include "gfx/fonts/font_lmroman_40.h"
#include "gfx/gfx.h"
#include "gfx/gfx_font_roles.h"
#include "util/fixed.h"
#include "util/intmath.h"

/* See gen_boot_anim_image.py; CLAUDE.md "Generated files". Also what
 * draw_image()'s own memcpy fast path below depends on being true. */
_Static_assert(BOOT_ANIM_IMAGE_W == GFX_WIDTH && BOOT_ANIM_IMAGE_H == GFX_HEIGHT,
    "boot_anim_image.h was generated for a different panel - regenerate it: "
    "python tools/gen_boot_anim_image.py ../design/boot/boot.png");
_Static_assert(sizeof(boot_anim_image) ==
                   (size_t)GFX_WIDTH * GFX_HEIGHT * sizeof(gfx_color_t),
    "the photo is not exactly one framebuffer - draw_image() hands it to "
    "gfx_blit_dither() as a full-screen source, which reads exactly that "
    "many pixels");

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "boot_anim";
#endif

#define COL_BG        GFX_RGB(0x000000)
#define COL_WHITE     GFX_RGB(0xFFFFFF)

#define COL_AXIS      0x5A6478
#define COL_TICK      0x8792A8
#define COL_GRID      0x121A2B
#define COL_ZERO      0xFFFFFF

#define LABEL_SCALE 1           /* 8x8 glyphs: an axis label is small */
#define LABEL_GAP   5

#define HEAD_OUTER 13
#define HEAD_MID   8
#define HEAD_CORE  4

/* A zero of zeta, marked on the t axis where the curve crosses it. */
#define ZERO_DOT 5

/*---------------------------------------------------------------------------
 * Colour
 *-------------------------------------------------------------------------*/

/* Folds global dissolve into existing alpha. */
static uint8_t scale8(uint8_t a, uint8_t b)
{
    return (uint8_t)(((uint32_t)a * b + 127u) / 255u);
}

/* `rgb` lifted `alpha` of the way off the background. */
static gfx_color_t lit(uint32_t rgb, uint8_t alpha)
{
    return gfx_color_mix(COL_BG, gfx_rgb(rgb), alpha);
}

/* The floor needs this extra mix and lit() alone does not: a saturated
 * hue only ever gets more OPAQUE through lit() on its own, never any
 * less colour-muddy, and it is the colour itself moving toward white
 * that is supposed to be doing most of the work of making the grid read
 * here. */
static gfx_color_t lit_whitened(uint32_t rgb, uint8_t whiten, uint8_t alpha)
{
    const gfx_color_t whitened = gfx_color_mix(gfx_rgb(rgb), COL_WHITE, whiten);
    return gfx_color_mix(COL_BG, whitened, alpha);
}

/*---------------------------------------------------------------------------
 * Projection
 *
 * boot_anim.h's projection family - a matrix-vector multiply by the frame's
 * composed space-then-camera transform, then a perspective divide - does
 * the real work; every draw_* call site below reads one of its functions
 * directly, never the raw, unclipped boot_anim_project() (see the three
 * "Not boot_anim_project() directly" comments below). Which one depends on
 * what is being drawn: a lone point (a zero marker, a pen head, an axis
 * label anchor) reads boot_anim_project_point(), which rejects outright
 * rather than draw somewhere nonsensical for a point behind the camera (see
 * that function's own comment); a LINE (a curve segment, a grid ring or
 * spoke, an axis arm) reads boot_anim_project_segment()/boot_anim_
 * project_segment_cs() instead, which clips a segment straddling the near
 * plane to where it actually crosses it rather than rejecting the whole
 * thing - see that function's own comment for why a segment needs the
 * extra step a lone point does not.
 *
 * There is no separate "shrunk" variant: scale lives in the space
 * transform's own SCALE channel, baked into the matrix `view` already
 * carries. Every draw_* call below reads this, at full scale, always. */

/* A whole number of grid units, as a Q12 value. */
static int32_t units(int n)
{
    return (int32_t)n * BOOT_ANIM_ONE;
}

/*---------------------------------------------------------------------------
 * The floor
 *
 * The complex plane zeta's value lives in, drawn as a floor at t = 0.
 * Giving it a floor rather than leaving the two axes bare is what makes
 * the third axis read as height instead of as a third line through the
 * same point.
 *
 * A POLAR grid - concentric rings, each a genuine circle, plus a handful
 * of radial spokes - not the square lattice of crossing horizontal/
 * vertical lines this used to be. Two things needed that, together:
 * boot_anim_wave_height() (see boot_anim.h's "The wave" section) already
 * lifts a point by its true distance from the origin, so a ring drawn as
 * a real circle rises as one uniform ring, exactly like a water ripple's
 * own wavefront; a square ring cannot ever BE that shape, whatever its
 * height does, and neither can its COLOUR - boot_anim_grid_hue() below
 * still colours one ring in one flat tone, so on the old crossing-line
 * grid that tone traced the same square/diamond the lines themselves did,
 * not the circle the maths already treated it as. A circle fixes both
 * with the one change.
 *
 * `far`/`d` are fixed local-space reach now, not scaled by a shrink of
 * their own - the grid IS the plane the curve and axes are drawn against,
 * and now reads as the same scale changing they do because it goes
 * through the exact same space transform they do (see "Projection" above),
 * rather than riding its own separate pulse the way it used to. */

#define BOOT_ANIM_GRID_CIRCLE_STEPS 12

/* NEAR portion vertices span distance, smooth growth, dashed segments - not
 * wave. */
#define BOOT_ANIM_GRID_SPOKE_STEPS 32

/* NEAR spoke reach limit, unrelated to grid size. */
#define BOOT_ANIM_GRID_SPOKE_NEAR_UNITS 10

/* ITS limit is panel edge crossing. Walked as a single extra segment.
 * Clipping handles rest. */
#define BOOT_ANIM_GRID_SPOKE_FAR_UNITS 500

static void polar_point(int32_t radius, uint16_t turn, int32_t *re, int32_t *im)
{
    const int32_t cos_v = boot_anim_cos(turn);
    const int32_t sin_v = boot_anim_sin(turn);
    *re = (int32_t)(((int64_t)radius * cos_v) >> 15);
    *im = (int32_t)(((int64_t)radius * sin_v) >> 15);
}

static void draw_grid_circle(int32_t radius, int32_t t, gfx_color_t c,
                             int steps, const boot_anim_view_t *view)
{
    S3L_Vec4 prev_cs;
    bool prev_front = false;
    int prev_sx = 0, prev_sy = 0;
    bool have_prev = false;

    for (int step = 0; step <= steps; step++) {
        /* step == steps closes loop back to step 0, no gap on last edge. */
        const int i = (step == steps) ? 0 : step;
        const uint16_t turn = (uint16_t)(((uint32_t)i * 65536u) /
                                         (uint32_t)steps);
        int32_t re, im;
        polar_point(radius, turn, &re, &im);
        const S3L_Vec4 cs = boot_anim_to_camera_space(re, im, t, view);
        const bool front = cs.z > BOOT_ANIM_NEAR_Z;
        int sx = 0, sy = 0;
        if (front) {
            boot_anim_camera_to_screen(cs, view->focal, &sx, &sy);
        }

        if (have_prev) {
            if (prev_front && front) {
                gfx_line_ex(prev_sx, prev_sy, sx, sy, c, 0u);
            } else if (prev_front != front) {
                int ax, ay, bx, by;
                if (boot_anim_project_segment_cs(prev_cs, cs, view,
                                                 &ax, &ay, &bx, &by)) {
                    gfx_line_ex(ax, ay, bx, by, c, 0u);
                }
            }
        }
        prev_cs = cs;
        prev_front = front;
        prev_sx = sx;
        prev_sy = sy;
        have_prev = true;
    }
}

#define BOOT_ANIM_GRID_SPOKE_DASH_STEPS 3

/* Drawn FLAT: a spoke is a plane guide line, not the ring data the wave
 * is about. TWO PARTS, ONE reveal: `reach` scales one target radius
 * across the full near..far span - the near portion (stepped, dashable)
 * walks to min(target, near); the tail (one segment, undashed) appears
 * once that is done. Stepping the tail at the near resolution wastes
 * budget nothing is close enough to see. `dash` skips whole segment
 * GROUPS (a full gfx_line_ex() call each), not pixel-thinning - genuinely
 * cheaper. */
static void draw_grid_spoke(uint16_t turn, int32_t near, int32_t far,
                            gfx_color_t c, bool dash, uint8_t reach,
                            const boot_anim_view_t *view)
{
    S3L_Vec4 prev_cs;
    bool have_prev = false;
    int32_t last_radius = 0;

    const int32_t target = boot_anim_spoke_reveal_target(near, far, reach);
    const int32_t near_target = target < near ? target : near;
    const int max_step = near > 0 ?
        (int)(((int64_t)BOOT_ANIM_GRID_SPOKE_STEPS * near_target) / near) : 0;

    for (int step = 0; step <= max_step; step++) {
        const int32_t radius = (near * step) / BOOT_ANIM_GRID_SPOKE_STEPS;
        int32_t re, im;
        polar_point(radius, turn, &re, &im);
        const S3L_Vec4 cs = boot_anim_to_camera_space(re, im, 0, view);

        /* Groups of BOOT_ANIM_GRID_SPOKE_DASH_STEPS alternate on and off to
         * form a visible dash. */
        const bool draw_segment = !dash ||
            ((step / BOOT_ANIM_GRID_SPOKE_DASH_STEPS) % 2) == 1;
        if (have_prev && draw_segment) {
            int ax, ay, bx, by;
            if (boot_anim_project_segment_cs(prev_cs, cs, view,
                                             &ax, &ay, &bx, &by)) {
                gfx_line_ex(ax, ay, bx, by, c, 0u);
            }
        }
        prev_cs = cs;
        have_prev = true;
        last_radius = radius;
    }

    if (have_prev && last_radius < target) {
        int32_t re, im;
        polar_point(target, turn, &re, &im);
        const S3L_Vec4 cs = boot_anim_to_camera_space(re, im, 0, view);
        int ax, ay, bx, by;
        if (boot_anim_project_segment_cs(prev_cs, cs, view,
                                         &ax, &ay, &bx, &by)) {
            gfx_line_ex(ax, ay, bx, by, c, 0u);
        }
    }
}

void draw_floor(uint32_t now_ms, uint8_t ink,
                const boot_anim_view_t *view)
{
    const int32_t amp_q12 = (int32_t)(((int64_t)BOOT_ANIM_WAVE_HEIGHT_Q12 *
        boot_anim_wave_envelope(now_ms)) / 255);
    const int32_t wavelength_q12 = BOOT_ANIM_WAVE_WAVELENGTH_Q12;
    const uint32_t period_ms = BOOT_ANIM_WAVE_PERIOD_MS;

    bool last_ring_tiny = false;

    const int dissolve_level =
        gfx_dither_level(boot_anim_image_reveal(now_ms));

    for (int ring = 1; ring <= BOOT_ANIM_GRID_RINGS; ring++) {
        const uint8_t alpha = scale8(boot_anim_grid_alpha(now_ms, ring), ink);
        if (alpha == 0) {
            break;
        }

        if ((ring & 1) &&
            (last_ring_tiny ||
             dissolve_level >= BOOT_ANIM_DISSOLVE_COARSE_LEVEL)) {
            continue;
        }

        /* BOOT_ANIM_GRID_RINGS's comment explains closer rings */
        const int32_t d = (int32_t)ring * BOOT_ANIM_GRID_STEP_Q12;
        const int32_t t = boot_anim_wave_height(d, now_ms, amp_q12,
                                                wavelength_q12, period_ms);

        int32_t rim_re, rim_im;
        polar_point(d, 0, &rim_re, &rim_im);
        const S3L_Vec4 rim_a =
            boot_anim_to_camera_space(rim_re, rim_im, t, view);
        polar_point(d, 32768, &rim_re, &rim_im);
        const S3L_Vec4 rim_b =
            boot_anim_to_camera_space(rim_re, rim_im, t, view);

        int steps = BOOT_ANIM_GRID_CIRCLE_STEPS;
        if (boot_anim_screen_chord_lt(rim_a, rim_b, view, 32)) {
            steps = 4;
        } else if (boot_anim_screen_chord_lt(rim_a, rim_b, view, 72)) {
            steps = 6;
        } else if (boot_anim_screen_chord_lt(rim_a, rim_b, view, 128)) {
            steps = 8;
        }
        if (dissolve_level >= BOOT_ANIM_DISSOLVE_COARSE_LEVEL && steps > 6) {
            steps = 6;
        } else if (dissolve_level >= BOOT_ANIM_DISSOLVE_HALF_LEVEL &&
                   steps > 8) {
            steps = 8;
        }
        last_ring_tiny = boot_anim_screen_chord_lt(rim_a, rim_b, view, 16);

        const gfx_color_t c = lit_whitened(
            boot_anim_hue_rgb(boot_anim_grid_hue(now_ms, ring)),
            boot_anim_grid_whiten(now_ms), alpha);

        draw_grid_circle(d, t, c, steps, view);
    }

    const gfx_color_t spoke_c = lit(COL_AXIS, ink);
    const uint8_t spoke_reach = boot_anim_grid_spoke_reach(now_ms);
    if (spoke_reach > 0) {
        for (int i = 0; i < BOOT_ANIM_GRID_SPOKES; i++) {
            const uint16_t turn = (uint16_t)((i * 65536) / BOOT_ANIM_GRID_SPOKES);
            draw_grid_spoke(turn, units(BOOT_ANIM_GRID_SPOKE_NEAR_UNITS),
                            units(BOOT_ANIM_GRID_SPOKE_FAR_UNITS), spoke_c,
                            BOOT_ANIM_GRID_SPOKE_DASH != 0, spoke_reach, view);
        }
    }
}

/*---------------------------------------------------------------------------
 * The axes
 *-------------------------------------------------------------------------*/

/* Arms arrive together despite differing lengths. */
static void draw_arm(int32_t re, int32_t im, int32_t t, uint8_t reach,
                     uint8_t ink, const boot_anim_view_t *view)
{
    const int32_t fre = tween_lerp_i32(0, re, reach);
    const int32_t fim = tween_lerp_i32(0, im, reach);
    const int32_t ft  = tween_lerp_i32(0, t,  reach);

    int ax, ay, bx, by;
    if (boot_anim_project_segment(0, 0, 0, fre, fim, ft, view,
                                  &ax, &ay, &bx, &by)) {
        gfx_line_ex(ax, ay, bx, by, lit(COL_AXIS, ink), 0u);
    }
}

static void draw_label(int x, int y, const char *text, uint8_t ink)
{
    const int w = gfx_font_width(gfx_font_ui(), text, -1, LABEL_SCALE);
    const int h = gfx_font_height(gfx_font_ui(), LABEL_SCALE);

    gfx_text_scaled(x - w / 2, y - h / 2, text, lit(COL_TICK, ink),
                    LABEL_SCALE);
}

void draw_axes(uint32_t now_ms, uint8_t ink,
              const boot_anim_view_t *view)
{
    const uint8_t reach = boot_anim_axis_reach(now_ms);
    if (reach == 0) {
        return;
    }

    const uint8_t finale = boot_anim_finale_reach(now_ms);
    const int32_t short_arm = units(4);
    const int32_t long_arm  = units(BOOT_ANIM_AXIS_FAR_UNITS);
    const int32_t arm = tween_lerp_i32(short_arm, long_arm, finale);

    const int32_t short_top = (BOOT_ANIM_T_MAX_PHASE1 + 1) << BOOT_ANIM_TQ;
    const int32_t long_top  = boot_anim_zeta_to_t_q8(long_arm);
    const int32_t top = tween_lerp_i32(short_top, long_top, finale);

    draw_arm(arm, 0, 0, reach, ink, view);      /* real      */
    draw_arm(0, arm, 0, reach, ink, view);      /* imaginary */
    draw_arm(0, 0, top, reach, ink, view);      /* t         */

    if (reach == 255 && finale == 0) {
        /* finale == 0 ensures arms stay in front; consistency with other
         * calls. */
        int re_x, re_y, im_x, im_y, t_x, t_y;
        const bool re_ok = boot_anim_project_point(arm, 0, 0, view, &re_x, &re_y);
        const bool im_ok = boot_anim_project_point(0, arm, 0, view, &im_x, &im_y);
        const bool t_ok  = boot_anim_project_point(0, 0, top, view, &t_x, &t_y);

        if (re_ok) {
            draw_label(re_x + LABEL_GAP * 2, re_y + LABEL_GAP, "Re", ink);
        }
        if (im_ok) {
            draw_label(im_x - LABEL_GAP * 2, im_y + LABEL_GAP, "Im", ink);
        }
        if (t_ok) {
            draw_label(t_x + LABEL_GAP * 2, t_y - LABEL_GAP, "t", ink);
        }
    }
}

/*---------------------------------------------------------------------------
 * The zeros
 *-------------------------------------------------------------------------*/

void draw_zeros(int32_t pen_t_q8, uint8_t ink,
                const boot_anim_view_t *view)
{
    for (int i = 0; i < BOOT_ANIM_ZEROS; i++) {
        const int32_t t = boot_anim_zero_t[i];
        if (t > pen_t_q8) {
            break;      /* the table is in order, so nothing after it either */
        }
        /* Not boot_anim_project(): marker off-screen issue - see
         * boot_anim_project_point(). */
        int x, y;
        if (!boot_anim_project_point(0, 0, t, view, &x, &y)) {
            continue;
        }
        gfx_fill_rect(x - ZERO_DOT / 2, y - ZERO_DOT / 2,
                      ZERO_DOT, ZERO_DOT, lit(COL_ZERO, ink));
    }
}

/*---------------------------------------------------------------------------
 * The curve
 *-------------------------------------------------------------------------*/

static void draw_stroke(int x0, int y0, int x1, int y1,
                        gfx_color_t c, int width, bool joined)
{
    /* Offsets spread to thicken curve centrally. */
    const int half = width / 2;
    const bool shallow = im_abs(x1 - x0) > im_abs(y1 - y0);

    for (int i = 0; i < width; i++) {
        const int off = i - half;
        const int ax = shallow ? x0 : x0 + off;
        const int ay = shallow ? y0 + off : y0;
        const int bx = shallow ? x1 : x1 + off;
        const int by = shallow ? y1 + off : y1;

        gfx_line_ex(ax, ay, bx, by, c,
                    GFX_LINE_ADD | (joined ? GFX_LINE_OPEN : 0u));
    }
}

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

static void draw_heads(int32_t colour_pen, uint8_t ink,
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
        int x, y;
        if (!boot_anim_project_point(p.re, p.im, p.t, view, &x, &y)) {
            continue;
        }

        draw_head(x, y,
                  boot_anim_hue_rgb(boot_anim_stroke(at, colour_pen).hue), ink);
    }
}

/* Re-colours within half the curve's length of the head. No second
 * framebuffer available. */
int32_t draw_curve(uint32_t now_ms, uint8_t ink,
                   const boot_anim_view_t *view)
{
    const int32_t pen = boot_anim_pen(now_ms);
    if (pen <= 0) {
        return 0;
    }

    const int32_t span = (int32_t)(BOOT_ANIM_CURVE_POINTS - 1);
    const int32_t at = pen * span;
    const int last = at >> BOOT_ANIM_Q;
    const int32_t part = at & (BOOT_ANIM_ONE - 1);

    const int32_t colour = boot_anim_colour_progress(now_ms);
    const int32_t phase1_span = (int32_t)(BOOT_ANIM_CURVE_PHASE1_POINTS - 1);

    const int stride = boot_anim_curve_stride(view);

    /* BOOT_ANIM_DISSOLVE_HALF_LEVEL halves; COARSE_LEVEL collapses to chord.
     * Quadratic limits deviation. */
    const int dissolve_level =
        gfx_dither_level(boot_anim_image_reveal(now_ms));
    const int max_steps =
        dissolve_level >= BOOT_ANIM_DISSOLVE_COARSE_LEVEL ? 1 :
        dissolve_level >= BOOT_ANIM_DISSOLVE_HALF_LEVEL   ? 2 :
        BOOT_ANIM_SPLINE_STEPS;

    boot_anim_pt_t s0 = boot_anim_sample(-stride);
    boot_anim_pt_t s1 = boot_anim_sample(0);
    S3L_Vec4 ta = boot_anim_to_camera_space(s0.re, s0.im, s0.t, view);
    S3L_Vec4 tb = boot_anim_to_camera_space(s1.re, s1.im, s1.t, view);
    /* Kept in CAMERA space across the loop - see
     * boot_anim_project_segment_cs() for explanation. */
    S3L_Vec4 prev_cs = tb;
    /* Cached position for perspective division if prev_front indicates point
     * in front. Straddling segments use boot_anim_project_segment_cs()'s
     * clip. */
    bool prev_front = prev_cs.z > BOOT_ANIM_NEAR_Z;
    int prev_sx = 0, prev_sy = 0;
    if (prev_front) {
        boot_anim_camera_to_screen(prev_cs, view->focal, &prev_sx, &prev_sy);
    }
    /* GFX_LINE_OPEN's `joined` comment: near-plane skips cause gaps; next
     * segment must skip start pixel. */
    bool joined = false;

    int32_t a0 = 0;

    for (int i = 0; i <= last && i < BOOT_ANIM_CURVE_POINTS; i += stride) {
        const boot_anim_pt_t s2 = boot_anim_sample(i + stride);
        const S3L_Vec4 tc = boot_anim_to_camera_space(s2.re, s2.im, s2.t, view);

        /* Interpolate colour across spans, not per sample. */
        const int32_t a1 = ((i + stride) * BOOT_ANIM_ONE + phase1_span / 2) /
                           phase1_span;

        /* Head jumps from sample to sample without partial span. */
        const int32_t limit =
            (stride == 1 && i == last) ? part : BOOT_ANIM_ONE;

        /* See boot_anim_curve_lod_steps(). Dissolve cap limits. */
        int steps = boot_anim_curve_lod_steps(ta, tc, view);
        if (steps > max_steps) {
            steps = max_steps;
        }

        const boot_anim_stroke_t s =
            boot_anim_stroke(a0 + ((a1 - a0) >> 1), colour);
        gfx_color_t span_c = gfx_rgb(boot_anim_hue_rgb(s.hue));
        span_c = gfx_color_mix(span_c, COL_WHITE, s.bloom);
        span_c = gfx_color_mix(COL_BG, span_c, scale8(s.glow, ink));

        for (int step = 1; step <= steps; step++) {
            const int32_t t = (limit * step) / steps;

            const S3L_Vec4 next_cs = boot_anim_spline_cs(ta, tb, tc, t);
            const bool next_front = next_cs.z > BOOT_ANIM_NEAR_Z;

            if (prev_front && next_front) {
                /* Near-plane clip inapplicable, project one point, reuse
                 * cached one. */
                int nsx, nsy;
                boot_anim_camera_to_screen(next_cs, view->focal, &nsx, &nsy);
                draw_stroke(prev_sx, prev_sy, nsx, nsy,
                            span_c, s.width, joined);
                joined = true;
                prev_sx = nsx;
                prev_sy = nsy;
            } else if (prev_front != next_front) {
                /* Straddles the near plane - the one case the pairwise
                 * clip exists for. */
                int ax, ay, bx, by;
                if (boot_anim_project_segment_cs(prev_cs, next_cs, view,
                                                 &ax, &ay, &bx, &by)) {
                    draw_stroke(ax, ay, bx, by, span_c, s.width, joined);
                    joined = true;
                    if (next_front) {
                        /* Cache valid across crossing. */
                        prev_sx = bx;
                        prev_sy = by;
                    }
                } else {
                    joined = false;
                }
            } else {
                joined = false;   /* both behind - nothing to draw */
            }
            prev_cs = next_cs;
            prev_front = next_front;
        }

        a0 = a1;
        ta = tb;
        tb = tc;
    }

    draw_heads(colour, ink, view);

    const boot_anim_pt_t final_c0 = boot_anim_sample(last - 1);
    const boot_anim_pt_t final_c1 = boot_anim_sample(last);
    const boot_anim_pt_t final_c2 = boot_anim_sample(last + 1);
    return boot_anim_spline(final_c0, final_c1, final_c2, part).t;
}

/*---------------------------------------------------------------------------
 * The title
 *-------------------------------------------------------------------------*/

static void title_glyph_origin(int view_x, int view_y, int glyph_w,
                               int glyph_h, int *panel_x, int *panel_y)
{
    (void)glyph_w;   /* DISPLAY_LANDSCAPE uses box height; see mapped corners. */
    *panel_x = GFX_WIDTH - view_y - glyph_h;
    *panel_y = view_x;
}

/* White, not hue-wheel: stays legible by not competing on colour.
 * Drop-shadow dx/dy (authored, signed) go through
 * boot_anim_title_shadow_offset()'s quarter-turn first: "down-right" in
 * the reader's frame differs from panel space. DITHERED, not solid: fake
 * transparency, this panel's usual trick with no real blending. 255 is
 * pixel-identical to the old plain call; 0 disables. Halo is plain
 * COL_BG, not luminance-derived: that flips white the instant ink dips
 * dark, backwards for fading to black. */
void draw_title(uint32_t now_ms, uint8_t ink)
{
    const gfx_color_t c = gfx_color_mix(COL_BG, COL_WHITE, ink);
    /* See BOOT_ANIM_TITLE_FONT. Default 0 uses 40px Computer Modern; 1 uses
     * 8x8 bitmap. Use gfx_font_ui() for flexibility. gfx_font_lmroman_40 for
     * linker drop. */
    const gfx_font_t *font = (BOOT_ANIM_TITLE_FONT == BOOT_ANIM_TITLE_FONT_8X8)
                                 ? gfx_font_ui()
                                 : &gfx_font_lmroman_40;
    const int glyph_w = gfx_font_width(font, "A", -1, BOOT_ANIM_TITLE_SCALE);
    const int glyph_h = gfx_font_height(font, BOOT_ANIM_TITLE_SCALE);
    char one[2] = { 0, 0 };

    const bool has_shadow =
        (BOOT_ANIM_TITLE_SHADOW_DX != 0 || BOOT_ANIM_TITLE_SHADOW_DY != 0) &&
        BOOT_ANIM_TITLE_SHADOW_ALPHA != 0;

    for (int i = 0; i < BOOT_ANIM_TITLE_LEN; i++) {
        const boot_anim_title_pos_t p = boot_anim_title_letter(font, i, now_ms);
        int px, py;
        title_glyph_origin(p.x, p.y, glyph_w, glyph_h, &px, &py);

        one[0] = BOOT_ANIM_TITLE[i];
        if (has_shadow) {
            int shadow_dx, shadow_dy;
            boot_anim_title_shadow_offset(BOOT_ANIM_TITLE_SHADOW_DX,
                                          BOOT_ANIM_TITLE_SHADOW_DY,
                                          &shadow_dx, &shadow_dy);
            gfx_text_font_dither(px + shadow_dx, py + shadow_dy, one, COL_BG,
                                 BOOT_ANIM_TITLE_SCALE, DISPLAY_LANDSCAPE,
                                 font, BOOT_ANIM_TITLE_SHADOW_ALPHA);
        }
        gfx_text_font(px, py, one, c, BOOT_ANIM_TITLE_SCALE,
                     DISPLAY_LANDSCAPE, font);
    }
}

/*---------------------------------------------------------------------------
 * The photograph
 *
 * The one thing here that is not drawn but COMPOSITED: every draw_* call
 * above computes a colour and STORES it - lit()/lit_whitened() both mix
 * off the constant COL_BG, and gfx_pixel()/gfx_line_ex() write whatever
 * they are handed. Nothing above ever reads the pixel already sitting in
 * the framebuffer. That is fine over black and wrong over a photograph -
 * a half-faded grid line drawn that way would paint a dark, OPAQUE
 * scratch across the mountain, not a fading-transparent one - so the
 * crossfade happens here instead, the one place that can leave a scene
 * pixel exactly as drawn rather than overwrite it outright. NOT by
 * reading and blending it, though - gfx_dither_covers() below never
 * inspects a pixel's own value, only its (x, y) - but by choosing not to
 * write over it at all for whichever pixels the current reveal fraction
 * does not yet cover. boot_anim_draw_frame() below draws the scene
 * (floor/axes/curve/zeros) at full, undimmed ink, gated off once boot_
 * anim_scene_reach() says it is about to be fully covered anyway; this
 * then dithers the photograph OVER whatever that left behind, at boot_
 * anim_image_reveal()'s own coverage fraction - a stippled coverage
 * split between the two pictures, not a true per-pixel blend (see draw_
 * image()'s own comment below for why, and what that trades away). The
 * title is drawn AFTER this call, untouched by any of it - see boot_
 * anim_scene_reach()'s own comment in boot_anim.h for why that is a
 * deliberate departure from this file's "one multiply takes the whole
 * picture down together" design elsewhere.
 *
 * The compositing itself is gfx_blit_dither() - a gfx primitive now, not
 * boot-local code: it started life as this file's own row-pattern loop and
 * was extracted once it was clear the technique (cheap dithered image-over-
 * live-content) is exactly what an app transition or image viewer will
 * want, which is this whole animation's real job - proving out the
 * machinery the apps get to keep. See its contract in gfx.h; it marks its
 * own dirty band, so there is no gfx_mark_all_dirty() here any more. */
void draw_image(uint8_t ink, uint8_t reveal)
{
    if (reveal == 0) {
        return;
    }

    const gfx_color_t *photo = (const gfx_color_t *)boot_anim_image;

    if (ink == 255) {
        /* DITHERED, not blended: no framebuffer-read blend hardware, the
         * same trade this file makes for the title's shadow (draw_title()).
         * A per-pixel gfx_color_mix() over every pixel was measurably the
         * most expensive part of a crossfade frame - see
         * suite_boot_anim_perf.c. */
        gfx_blit_dither(0, 0, GFX_WIDTH, GFX_HEIGHT, photo, GFX_WIDTH,
                        reveal);
    } else {
        gfx_blit_dither(0, 0, GFX_WIDTH, GFX_HEIGHT, photo, GFX_WIDTH,
                        reveal < ink ? reveal : ink);
    }
}

/*---------------------------------------------------------------------------
 * The loop
 *-------------------------------------------------------------------------*/

/* EVERY FRAME IS A FULL REPAINT suite_boot_anim_perf.c times this phase
 * gfx_clear() cost in gfx.c */
void boot_anim_clear_frame(void)
{
    gfx_clear(COL_BG);
}

void boot_anim_draw_frame(uint32_t now_ms)
{
    const uint8_t ink    = boot_anim_ink(now_ms);
    const uint8_t reveal = boot_anim_image_reveal(now_ms);
    const uint8_t scene  = boot_anim_scene_reach(now_ms);

    const boot_anim_view_t view = boot_anim_view(GFX_WIDTH, GFX_HEIGHT,
                                                 now_ms);

    boot_anim_clear_frame();

    /* Gated like title. Full coverage skips draw_image(). See boot_anim.h. */
    if (scene > 0) {
        draw_floor(now_ms, ink, &view);
        draw_axes(now_ms, ink, &view);

        const int32_t reached = draw_curve(now_ms, ink, &view);
        draw_zeros(reached, ink, &view);
    }

    draw_image(ink, reveal);

    /* Gated to save gfx calls before BOOT_ANIM_TITLE_START_MS. */
    if (now_ms >= BOOT_ANIM_TITLE_START_MS) {
        draw_title(now_ms, ink);
    }
}

#if CONFIG_LAUNCHER_DEVELOPMENT
static void report_fps_windowed(int64_t now_us, uint32_t now_ms,
                                int64_t *window_start, uint32_t *frames)
{
    (*frames)++;
    const int64_t since = now_us - *window_start;
    if (since >= 500000) {
        ESP_LOGI(TAG, "t=%ums: %.1f fps", (unsigned)now_ms,
                 (double)*frames * 1000000.0 / (double)since);
        *frames = 0;
        *window_start = now_us;
    }
}
#endif

#ifdef ESP_PLATFORM
void boot_anim_run(void)
{
    const int64_t started_us = esp_timer_get_time();
    uint32_t frames = 0;
#if CONFIG_LAUNCHER_DEVELOPMENT
    int64_t fps_window_start = started_us;
    uint32_t fps_window_frames = 0;
#endif

    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        const int64_t elapsed_us = now_us - started_us;
        const uint32_t now_ms = (uint32_t)(elapsed_us / 1000);
        if (now_ms >= BOOT_ANIM_MS) {
            break;
        }

        boot_anim_draw_frame(now_ms);
        gfx_present();
        frames++;
#if CONFIG_LAUNCHER_DEVELOPMENT
        report_fps_windowed(now_us, now_ms, &fps_window_start,
                            &fps_window_frames);
#endif

        /* The same yield the shell's loop makes, for the same reason: the
         * idle task feeds the watchdog. */
        vTaskDelay(1);
    }

    /* Checked only on the board; not on host. */
    ESP_LOGI(TAG, "%u frames in %d ms (%.1f fps)", (unsigned)frames,
             BOOT_ANIM_MS, (double)frames * 1000.0 / BOOT_ANIM_MS);

}
#endif
