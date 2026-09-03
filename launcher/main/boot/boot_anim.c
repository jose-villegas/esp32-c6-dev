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
#include "gfx/gfx.h"
#include "util/fixed.h"
#include "util/intmath.h"

/* The shipped photo's own shape has to match this panel exactly - see
 * gen_boot_anim_image.py's own top comment for why it is generated
 * pre-rotated into panel space rather than rotated at draw time. This is
 * the "shipped artifact tested independently of the generator" half of
 * this project's generated-file convention (CLAUDE.md's "Generated
 * files" section) for an asset with no underlying math to check pixel
 * content against - what CAN still be checked, at compile time, for
 * free, is that the array is exactly one framebuffer's worth of the
 * right shape, which is also the exact fact draw_image()'s own memcpy
 * fast path below depends on being true. */
_Static_assert(BOOT_ANIM_IMAGE_W == GFX_WIDTH && BOOT_ANIM_IMAGE_H == GFX_HEIGHT,
    "boot_anim_image.h was generated for a different panel - regenerate it: "
    "python tools/gen_boot_anim_image.py ../design/boot/boot.png");
_Static_assert(sizeof(boot_anim_image) ==
                   (size_t)GFX_WIDTH * GFX_HEIGHT * sizeof(gfx_color_t),
    "the photo is not exactly one framebuffer - draw_image()'s memcpy "
    "fast path assumes it is");

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
 * Projection
 *
 * boot_anim.h's projection family - a matrix-vector multiply by the frame's
 * composed space-then-camera transform, then a perspective divide - does
 * the real work; there is no local wrapper here any more, so every draw_*
 * call site below reads one of boot_anim.h's own functions directly.
 * Which one depends on what is being drawn, not a single shared choice:
 * a lone point (a zero marker, a pen head, an axis label anchor) reads
 * boot_anim_project_point(), which rejects outright rather than draw
 * somewhere nonsensical for a point behind the camera (see that
 * function's own comment); a LINE (a curve segment, a grid ring or
 * spoke, an axis arm) reads boot_anim_project_segment()/boot_anim_
 * project_segment_cs() instead, which clips a segment straddling the
 * near plane to where it actually crosses it rather than rejecting the
 * whole thing - see that function's own comment for why a segment needs
 * the extra step a lone point does not. Nothing left here calls the raw,
 * unclipped boot_anim_project() at all - see the three "Not boot_anim_
 * project() directly" comments below for the class of bug that used to
 * risk.
 *
 * There is no separate "shrunk" variant either - what used to be a post-
 * projection pixel-space shrink (boot_anim_motif_shrink_q8(), applied here
 * via csx()/csy()) is now the space transform's own SCALE channel, baked
 * into the matrix `view` already carries. Every draw_* call below reads
 * this, at full scale, always. */

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

/* Segments per ring's own circle - fixed, a tuned-by-eye smoothness/cost
 * tradeoff, the same reasoning BOOT_ANIM_SPLINE_STEPS already is for the
 * curve: a keyframe has no business tuning it. Not authored like
 * BOOT_ANIM_GRID_SPOKES below (a spoke is a creative choice - how many,
 * whether to show them at all - a ring's own roundness is not). */
#define BOOT_ANIM_GRID_CIRCLE_STEPS 12

/* How finely the NEAR portion of a spoke is walked, unlike a ring's own
 * CIRCLE_STEPS above - a spoke's own vertices span a real distance (0 to
 * BOOT_ANIM_GRID_SPOKE_NEAR_UNITS below), not one shared radius, so a
 * growing reach (boot_anim_grid_spoke_reach()) needs several steps to
 * read as smoothly extending outward rather than jumping in visible
 * chunks, and a dashed spoke (BOOT_ANIM_GRID_SPOKE_DASH) needs at least a
 * couple of segments per dash/gap pair - NOT the wave (see
 * draw_grid_spoke()'s own comment on why a spoke stays flat regardless of
 * it). Not per-spoke: BOOT_ANIM_GRID_SPOKES (generated - "Grid" in tools/
 * boot_anim_editor.html, 0 hides them outright) is rarely more than a
 * handful, so even a step count this fine stays cheap regardless of how
 * many spokes are actually drawn. */
#define BOOT_ANIM_GRID_SPOKE_STEPS 32

/* How far the NEAR, finely-stepped portion of a spoke reaches - where
 * BOOT_ANIM_GRID_SPOKE_STEPS's own resolution actually matters, because a
 * dash or a mid-reveal tip is only ever close enough to a camera to be
 * legible somewhere near the origin, at any framing this project uses.
 * Fixed, not authored, and deliberately NOT related to the grid's own
 * reach (BOOT_ANIM_GRID_RINGS * BOOT_ANIM_GRID_STEP_Q12) - a spoke has no
 * business being bounded by how many rings happen to be authored. */
#define BOOT_ANIM_GRID_SPOKE_NEAR_UNITS 10

/* How far out a spoke's own FAR tail reaches, past the near portion above -
 * a spoke is a structural guide line, not the ring data the wave is
 * actually about, so ITS limit is wherever the projected line crosses the
 * panel's edge, not a fixed distance in world space. gfx_line_ex()'s own
 * clip_line() already computes exactly that x,y crossing for any segment
 * reaching past the panel - this only has to be far enough that the
 * unclipped end is ALWAYS past it, for every camera framing this project
 * uses, and let clipping do the rest. Walked as a SINGLE extra segment
 * (see draw_grid_spoke()'s own comment on why), not stepped the way the
 * near portion is - nothing out there is ever close enough to a camera
 * for a dash or a reveal tip to matter, only whether the line eventually
 * clips. */
#define BOOT_ANIM_GRID_SPOKE_FAR_UNITS 500

/* A point at zeta-distance `radius` from the origin, `turn`/BOOT_ANIM_ONE
 * of the way round a full turn - boot_anim_sin()/cos() take a phase in
 * their own Q16-per-turn convention, not BOOT_ANIM_ONE's Q12, so this is
 * the one conversion every polar point here needs. int64_t: `radius` can
 * be tens of thousands of Q12 units for a wide grid reach, and boot_anim_
 * cos()/sin() return up to S3L_F's own Q15 magnitude - gfx.c's clip_line()
 * leans on the same int64_t-intermediate trick for the same reason. */
static void polar_point(int32_t radius, uint16_t turn, int32_t *re, int32_t *im)
{
    const int32_t cos_v = boot_anim_cos(turn);
    const int32_t sin_v = boot_anim_sin(turn);
    *re = (int32_t)(((int64_t)radius * cos_v) >> 15);
    *im = (int32_t)(((int64_t)radius * sin_v) >> 15);
}

/* One ring, walked all the way round rather than drawn as a single
 * segment - a straight line cannot approximate a circle at all, however
 * short. Every vertex is at the SAME distance from the origin by
 * construction (it is a circle), so unlike a spoke (see draw_grid_spoke()
 * below) the wave's own height is identical for the whole ring - `t` is
 * computed once by the caller, not per vertex here. */
static void draw_grid_circle(int32_t radius, int32_t t, gfx_color_t c,
                             const boot_anim_view_t *view)
{
    S3L_Vec4 prev_cs;
    bool have_prev = false;

    for (int step = 0; step <= BOOT_ANIM_GRID_CIRCLE_STEPS; step++) {
        /* step == STEPS closes the loop back onto step 0's own point,
         * rather than leaving a gap on the last edge of the ring. */
        const int i = (step == BOOT_ANIM_GRID_CIRCLE_STEPS) ? 0 : step;
        const uint16_t turn =
            (uint16_t)((i * 65536) / BOOT_ANIM_GRID_CIRCLE_STEPS);
        int32_t re, im;
        polar_point(radius, turn, &re, &im);
        const S3L_Vec4 cs = boot_anim_to_camera_space(re, im, t, view);

        if (have_prev) {
            int ax, ay, bx, by;
            if (boot_anim_project_segment_cs(prev_cs, cs, view,
                                             &ax, &ay, &bx, &by)) {
                gfx_line_ex(ax, ay, bx, by, c, 0u);
            }
        }
        prev_cs = cs;
        have_prev = true;
    }
}

/* Segments per dash, or per gap - BOOT_ANIM_GRID_SPOKE_DASH's own on/off
 * period, grouping several of the near portion's own fine steps into one
 * visible dash rather than alternating every single one of them: at 1,
 * each dash was a single ~0.3m step, too tight to read as a dashed line
 * at any camera distance this project actually uses rather than a
 * slightly-thinner-looking solid one. */
#define BOOT_ANIM_GRID_SPOKE_DASH_STEPS 3

/* One spoke, from the origin out to `far` at a fixed angle `turn` - the
 * structure a polar grid needs that rings alone do not give it (nothing
 * otherwise says which way is "outward" at a glance). Drawn FLAT - a
 * spoke does not carry the wave (see boot_anim.h's "The wave" section):
 * it is a guide line for the plane itself, and a guide that bent along
 * with the very displacement it is meant to help read stops doing its
 * job - only the rings, which the wave is actually about, lift.
 *
 * TWO PARTS, not one uniform walk, but ONE CONTINUOUS GROWTH across
 * `reach` regardless: the NEAR portion (0 to `near`) is walked in
 * BOOT_ANIM_GRID_SPOKE_STEPS short spans, same as a ring's own finer
 * structure, so a dash pattern has several segments to work with rather
 * than one all-or-nothing line - this is the only part of a spoke a
 * camera is ever actually close enough to for that resolution to be
 * visible. Beyond it, a SINGLE straight tail out to wherever `reach`'s own
 * target currently sits between `near` and `far` (see BOOT_ANIM_GRID_
 * SPOKE_FAR_UNITS's own comment on why `far` needs to reach so far at
 * all) - stepping THAT at the same fine resolution would spend most of a
 * spoke's own segment budget on a stretch of line nothing is ever close
 * enough to see the texture of, and dividing the WHOLE reach into
 * BOOT_ANIM_GRID_SPOKE_STEPS pieces once made each piece tens of metres
 * long, breaking the dash pattern outright (the entire on-screen portion
 * sat inside a single dash-or-gap with nothing left to alternate
 * against).
 *
 * The two parts share ONE reveal, not two independent ones: `reach`
 * scales a single target radius across the FULL 0..`far` span, not just
 * 0..`near` - the near portion is walked up to min(target, near), and the
 * tail (unstepped, undashed) only appears once the near portion is fully
 * walked, itself growing from `near` to `far` as `reach` keeps climbing.
 * A spoke whose reveal ends the instant `reach` leaves zero - the far
 * tail popping to its full, screen-clipping length regardless of how far
 * the reveal had actually gotten - measured reading as "grows too fast"
 * and "just a straight line" (the tail is never dashed, and used to be
 * everything drawn beyond the first sliver of the animation).
 *
 * `dash`: BOOT_ANIM_GRID_SPOKE_DASH (generated - "Grid" in tools/
 * boot_anim_editor.html) skips drawing every OTHER BOOT_ANIM_GRID_SPOKE_
 * DASH_STEPS-sized GROUP of near-segments rather than thinning pixels
 * within one - each segment is a whole gfx_line_ex() call, so skipping
 * half of them skips half the walk()s outright - the bounding box and
 * dirty_mark() included, not just the plot()s inside one - genuinely
 * cheaper to draw, not just a different look. The tail is never dashed -
 * nothing out there is close enough to read a dash pattern on anyway,
 * only whether the line eventually clips.
 *
 * `reach` (boot_anim_grid_spoke_reach(), Q0) bounds how much of the WHOLE
 * spoke - both parts together - is currently revealed, not just whether a
 * given segment draws: the loop simply stops early and the tail simply
 * does not reach as far yet, so a spoke mid-reveal costs less to draw
 * than a finished one, not the same amount with the far end thrown
 * away.
 *
 * `target` itself (see boot_anim_spoke_reveal_target()) is NOT `reach`
 * scaled linearly onto `near..far` - a linear world-space target puts
 * nearly all of the tail's own ON-SCREEN growth in the first few percent
 * of the reveal, since screen position falls off closer to 1/radius than
 * linearly with it. */
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

        /* Segment `step` runs from vertex step-1 to step - alternating
         * BOOT_ANIM_GRID_SPOKE_DASH_STEPS-sized groups on and off, rather
         * than every single step, is what keeps a dash long enough to
         * actually read as one. */
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

/* draw_floor(), draw_axes(), draw_curve(), draw_zeros(), draw_image() and
 * draw_title() below are all non-static, deliberately - see
 * suite_boot_anim_perf.c, which times each on its own across the
 * animation's own timeline. Nothing here is declared in boot_anim.h (no
 * caller outside this file and the perf suite has any business calling
 * these individually); the suite reaches them through its own extern
 * declarations, mirroring app_cube.c's own cube_update_rotation()/
 * cube_clear_frame()/cube_rasterize_frame() and the reasoning in that
 * file's own comment on why: boot_anim_draw_frame() below is just these
 * calls in sequence, so a perf run exercises the exact code a real frame
 * runs, not a hand copy of it that could drift. */
void draw_floor(uint32_t now_ms, uint8_t ink,
                const boot_anim_view_t *view)
{
    /* The three values the ring loop below hands straight to
     * boot_anim_wave_height() - see its own comment on why a zero
     * amplitude or wavelength already comes back flat with no separate
     * "is the wave even running" branch needed here. Peak amplitude
     * scaled by boot_anim_wave_envelope() BEFORE it gets here, not inside
     * boot_anim_wave_height() itself - the ripple's own shape and its own
     * strength-over-time are two separate concerns (see that function's
     * own comment). Spokes do not take any of this at all - see
     * draw_grid_spoke()'s own comment on why they stay flat. */
    const int32_t amp_q12 = (int32_t)(((int64_t)BOOT_ANIM_WAVE_HEIGHT_Q12 *
        boot_anim_wave_envelope(now_ms)) / 255);
    const int32_t wavelength_q12 = BOOT_ANIM_WAVE_WAVELENGTH_Q12;
    const uint32_t period_ms = BOOT_ANIM_WAVE_PERIOD_MS;

    for (int ring = 1; ring <= BOOT_ANIM_GRID_RINGS; ring++) {
        const uint8_t alpha = scale8(boot_anim_grid_alpha(now_ms, ring), ink);
        if (alpha == 0) {
            /* Not just THIS ring faded out with distance - every ring
             * beyond it is too, so there is nothing left for the rest of
             * the loop to draw. boot_anim_grid_alpha()'s own `arrived`
             * and `near` factors are both non-increasing in `ring` for a
             * fixed `now_ms` (a later ring starts fading in later, and
             * `left` shrinks as ring grows), and scale8() is monotonic
             * in its own first argument - so once this product hits
             * zero, it stays zero for every ring after it. `break`, not
             * `continue`: with BOOT_ANIM_GRID_RINGS large (the ring
             * count is authored, not fixed), skipping the rest outright
             * saves a real number of otherwise-wasted boot_anim_grid_
             * alpha() calls every frame, not just a cosmetic difference. */
            break;
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
         * BOOT_ANIM_GRID_RINGS's own comment on why the rings are closer
         * together than that by default, and generated (an "other const",
         * like the ring count itself) rather than fixed. */
        const int32_t d = (int32_t)ring * BOOT_ANIM_GRID_STEP_Q12;
        const int32_t t = boot_anim_wave_height(d, now_ms, amp_q12,
                                                wavelength_q12, period_ms);

        draw_grid_circle(d, t, c, view);
    }

    /* Structure, not the thing being coloured - a fixed, unhued tone
     * throughout (the same one the axes themselves use) rather than
     * riding boot_anim_grid_hue()'s own per-ring cycle the way the rings
     * do: a spoke passes through every ring in turn, so there is no one
     * ring index left to colour it by. */
    const gfx_color_t spoke_c = lit(COL_AXIS, ink);
    const uint8_t spoke_reach = boot_anim_grid_spoke_reach(now_ms);
    if (spoke_reach > 0) {
        /* A spoke is a structural guide line, the same as an axis - not
         * the ring data the wave is actually about, which is what
         * BOOT_ANIM_GRID_RINGS's own finite count (BOOT_ANIM_GRID_RINGS *
         * BOOT_ANIM_GRID_STEP_Q12, the ring loop's own `d` above) is
         * really describing. A spoke is not bounded by, or even related
         * to, that finite reach at all: the actual limit is wherever the
         * projected line crosses the panel's own edge - gfx_line_ex()'s
         * own clip_line() already computes exactly that x,y intersection
         * for any segment reaching past it, so a radius this far out is
         * "effectively forever" for every camera framing this project
         * uses, not a guess at a specific reach that happens to usually
         * be enough. */
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

/* One axis arm, grown `reach`/255 of the way from the origin to (re, im, t).
 *
 * A fraction rather than a pixel count, so the three arms - which are three
 * different lengths on screen - still arrive at their ends together. */
static void draw_arm(int32_t re, int32_t im, int32_t t, uint8_t reach,
                     uint8_t ink, const boot_anim_view_t *view)
{
    const int32_t fre = tween_lerp_i32(0, re, reach);
    const int32_t fim = tween_lerp_i32(0, im, reach);
    const int32_t ft  = tween_lerp_i32(0, t,  reach);

    /* An unbounded axis (see BOOT_ANIM_AXIS_FAR_UNITS) reaches just as far
     * behind the origin's plane as the grid's own rings do, once the
     * finale starts turning the camera - same near-plane risk, same fix. */
    int ax, ay, bx, by;
    if (boot_anim_project_segment(0, 0, 0, fre, fim, ft, view,
                                  &ax, &ay, &bx, &by)) {
        gfx_line_ex(ax, ay, bx, by, lit(COL_AXIS, ink), 0u);
    }
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

void draw_axes(uint32_t now_ms, uint8_t ink,
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
     * "FIXED POINT, AND FOUR SCALES OF IT" warns about.
     *
     * short_top is sized from BOOT_ANIM_T_MAX_PHASE1, not BOOT_ANIM_T_MAX -
     * see that constant's own comment in boot_anim.h: this is the SHORT,
     * labelled arm drawn before the finale unbounds it, and it must stay
     * the length it was tuned to fit the panel at even though the climb
     * itself now reaches twice as high. long_top used to be its own
     * separate guess (BOOT_ANIM_T_MAX * 3) in t's own native scale,
     * completely unrelated to long_arm above - measured NOT far enough
     * even after long_arm's own reach was fixed (an axis is unbounded in
     * all three of its own arms or it is not really unbounded). Now
     * boot_anim_zeta_to_t_q8() - already built for exactly this "the same
     * reach along t as along re/im" conversion, see its own comment - so
     * this arm gets the identical guaranteed-past-the-panel reach the
     * other two do, not a shorter one just because it happens to live in
     * a different native scale. */
    const int32_t short_top = (BOOT_ANIM_T_MAX_PHASE1 + 1) << BOOT_ANIM_TQ;
    const int32_t long_top  = boot_anim_zeta_to_t_q8(long_arm);
    const int32_t top = tween_lerp_i32(short_top, long_top, finale);

    draw_arm(arm, 0, 0, reach, ink, view);      /* real      */
    draw_arm(0, arm, 0, reach, ink, view);      /* imaginary */
    draw_arm(0, 0, top, reach, ink, view);      /* t         */

    /* Named rather than tick-marked. Which axis is which is the one thing a
     * reader cannot work out from the picture, and three short labels say it
     * where a ladder of numbers up a 315px axis would just be clutter.
     *
     * Only before the finale starts: an unbounded axis has nowhere left to
     * anchor a label to, so rather than have one drift or clip, it is
     * simplest to let the labels belong to the short-arm phase only and
     * drop away once the arms start growing past it. */
    if (reach == 255 && finale == 0) {
        /* Not boot_anim_project() directly: a label anchored to a point
         * behind the camera would draw wherever the unclipped divide
         * happens to land it, not "no label" - see
         * boot_anim_project_point()'s own comment. finale == 0 keeps
         * every arm well within the short, always-in-front-of-camera
         * phase in practice, but the check costs nothing and keeps this
         * call site consistent with every other one that projects a
         * single point rather than a segment. */
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

/* A dot on the t axis for each zero the pen has climbed past.
 *
 * These are the only points of the whole picture that mean anything on their
 * own: the curve touching the axis is zeta being zero, and the height it
 * happens at is one of the numbers the Riemann hypothesis is about. Drawn
 * white and flat rather than blended, so they stay legible through whatever
 * the curve is doing around them. */
void draw_zeros(int32_t pen_t_q8, uint8_t ink,
                const boot_anim_view_t *view)
{
    for (int i = 0; i < BOOT_ANIM_ZEROS; i++) {
        const int32_t t = boot_anim_zero_t[i];
        if (t > pen_t_q8) {
            break;      /* the table is in order, so nothing after it either */
        }
        /* Not boot_anim_project() directly: a marker behind the camera is
         * not "off screen", it is a lit square somewhere it was never
         * meant to be - see boot_anim_project_point()'s own comment. */
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
        /* Not boot_anim_project() directly: a pen behind the camera is
         * not "off screen", it is a bright disc somewhere it was never
         * meant to be - see boot_anim_project_point()'s own comment. */
        int x, y;
        if (!boot_anim_project_point(p.re, p.im, p.t, view, &x, &y)) {
            continue;
        }

        draw_head(x, y,
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
int32_t draw_curve(uint32_t now_ms, uint8_t ink,
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
    /* Kept in CAMERA space across the loop, not re-derived from `head`'s
     * re/im/t each time a segment is drawn - see boot_anim_project_segment()
     * in boot_anim.h for why: every interior point is both the end of one
     * segment and the start of the next, and transforming it twice would
     * double the per-point matrix work over a curve of several hundred
     * segments. */
    S3L_Vec4 prev_cs = boot_anim_to_camera_space(head.re, head.im, head.t, view);
    /* Whether the immediately PRECEDING segment was actually drawn, not
     * merely whether anything has ever been drawn - see GFX_LINE_OPEN's own
     * comment on `joined`: a segment skipped by the near-plane clip below
     * (boot_anim_project_segment_cs() returning false) leaves a gap, so the
     * next segment that IS drawn must not claim its start pixel already
     * landed there. */
    bool joined = false;

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
            const S3L_Vec4 next_cs =
                boot_anim_to_camera_space(head.re, head.im, head.t, view);
            const int32_t along = a0 + (((a1 - a0) * t) >> BOOT_ANIM_Q);

            int ax, ay, bx, by;
            if (boot_anim_project_segment_cs(prev_cs, next_cs, view,
                                             &ax, &ay, &bx, &by)) {
                draw_stroke(ax, ay, bx, by,
                            boot_anim_stroke(along, colour), ink, joined);
                joined = true;
            } else {
                joined = false;
            }
            prev_cs = next_cs;
        }
    }

    draw_heads(colour, ink, view);
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
 * doing behind it precisely because it does not compete on colour.
 *
 * Drop-shadowed - a second copy of each letter, offset by (BOOT_ANIM_
 * TITLE_SHADOW_DX, _DY) pixels, drawn FIRST so the ink lands on top of
 * it. Both authored (title_shadow_dx/dy in the JSON) - signed, so the
 * pair controls direction as well as reach: (1,1) is the classic down-
 * right shadow, (-2,0) a hard-left one, (0,0) disables the extra draw
 * outright rather than drawing a zero-offset copy under the ink for no
 * visible effect.
 *
 * DX/DY are authored in the READER's frame (right/down, same as the
 * comment above promises), but title_glyph_origin() has already turned
 * (px, py) into PANEL space - so the offset goes through
 * boot_anim_title_shadow_offset() (boot_anim.h) first, the same quarter-
 * turn done to the origin itself, or "down-right" comes out as "up-
 * right". See that function's own comment for the derivation - it is
 * pure arithmetic, host-tested there, since this function cannot be.
 *
 * DITHERED, not solid - gfx_text_font_dither() at BOOT_ANIM_TITLE_
 * SHADOW_ALPHA (also authored) instead of a plain gfx_text_font(), so
 * the shadow can read as translucent rather than a hard black
 * silhouette: fake transparency, the same ordered-dither trick this
 * panel uses for it everywhere, since it has no real blending anywhere
 * to fall back on (see gfx_fill_rect_dither()'s own comment in gfx.c).
 * 255 (the backward-compatible default) is guaranteed identical to the
 * old plain gfx_text_font() call, pixel for pixel - see that same
 * comment for why 255 takes its own unconditional path rather than the
 * dither test. 0 disables the shadow outright, same as dx=dy=0.
 *
 * Plain COL_BG, not derived from the ink the way a general-purpose halo
 * would be (see ui_style.h's own ui_text_halo(), used briefly here
 * before this became a knob, dropped when it stopped being ABLE to
 * offer a variable offset anyway - no reason to keep a cross-layer
 * microui.h dependency for a fixed distance this file no longer uses).
 * A luminance-derived halo would flip to full white the instant ink
 * dips dark, which is backwards for what this word actually does at the
 * end of the animation: fading toward black together with everything
 * else (see boot_anim_ink()). Plain COL_BG is already true black on this
 * AMOLED (see this file's own top comment), so it is already invisible
 * against the black background for most of the animation and only
 * starts doing real work once the photograph arrives behind the word
 * (see draw_image()) - which is also the one moment a white-on-white-ish
 * word would actually need it.
 *
 * Up to two gfx_text_font*() calls per letter now instead of one - real,
 * but bounded to six letters, and paid only for the ~1s the title is
 * actually flying in and settling, not the whole animation. */
void draw_title(uint32_t now_ms, uint8_t ink)
{
    const gfx_color_t c = gfx_color_mix(COL_BG, COL_WHITE, ink);
    const gfx_font_t *font = gfx_default_font();
    const int glyph_w = gfx_font_width(font, "A", -1, BOOT_ANIM_TITLE_SCALE);
    const int glyph_h = gfx_font_height(font, BOOT_ANIM_TITLE_SCALE);
    char one[2] = { 0, 0 };

    const bool has_shadow =
        (BOOT_ANIM_TITLE_SHADOW_DX != 0 || BOOT_ANIM_TITLE_SHADOW_DY != 0) &&
        BOOT_ANIM_TITLE_SHADOW_ALPHA != 0;

    for (int i = 0; i < BOOT_ANIM_TITLE_LEN; i++) {
        const boot_anim_title_pos_t p = boot_anim_title_letter(i, now_ms);
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
 * Writes through gfx_framebuffer() directly rather than 164,864
 * gfx_pixel() calls - the same bulk path app_cube.c and app_sand.c's own
 * dirty-row writer already take for a full-frame write, and with it the
 * same obligation gfx.h's own dirty-tracking comment states: gfx cannot
 * see a write through this pointer, so the caller marks it. gfx_clear()
 * has already marked everything dirty this frame, so the call below is
 * redundant today - it is here because the contract says so regardless,
 * and because that redundancy is not guaranteed to still hold if this
 * frame ever grows a partial clear. */
void draw_image(uint8_t ink, uint8_t reveal)
{
    if (reveal == 0) {
        return;
    }

    gfx_color_t *fb = gfx_framebuffer();
    const size_t n = (size_t)GFX_WIDTH * GFX_HEIGHT;

    if (reveal == 255 && ink == 255) {
        /* Fully arrived and not yet dissolving - which is most of the
         * photograph's time on screen. gfx_dither_covers() is exact at
         * alpha 255 (its own unconditional path, not the dither test -
         * see that function's own comment in gfx_color.h), so this is
         * the identical result the dithered loop below would produce,
         * for a plain row-major copy of one framebuffer's worth of
         * already-panel-format pixels - true only because the two
         * _Static_asserts near this file's own includes make it true,
         * not merely likely. */
        memcpy(fb, boot_anim_image, n * sizeof *fb);
    } else if (ink == 255) {
        /* The crossfade itself, the common case: ink is 255 for the
         * whole of it unless a timeline is deliberately authored to
         * overlap the two dissolves.
         *
         * DITHERED, not blended - the same "no framebuffer-read blend
         * hardware, so fake transparency with an ordered dither" trade
         * this file already makes for the title's own shadow (see
         * draw_title()'s own comment, and gfx_fill_rect_dither()'s in
         * gfx.c). A per-pixel gfx_color_mix() over all 164,864 pixels
         * was measurably the single most expensive part of a crossfade
         * frame - see suite_boot_anim_perf.c's own measured numbers,
         * where this loop alone outweighed gfx_present()'s own full-
         * panel QSPI transfer. gfx_dither_covers() is the exact per-
         * pixel decision gfx_fill_rect_dither() makes internally, reused
         * here so both call sites stay in phase off one table (gfx_
         * color.h) rather than each keeping its own copy. A covered
         * pixel becomes the photo outright - no multiply, no div255,
         * just the pixel that was already sitting in boot_anim_image[].
         * An uncovered one is left exactly as the scene already drew it;
         * skipping the write there is not an approximation, it is the
         * whole point - the photo and the scene never actually blend,
         * only their coverage does, which is what "dither" has always
         * meant since long before this chip's own class of hardware
         * could afford a real blend. Nested row/col, not a flat 0..n
         * index into gfx_dither_covers(x, y, ...): recovering (x, y)
         * from a flat index needs a divide per pixel, exactly the
         * instruction this whole rewrite exists to stop paying for. */
        for (int y = 0; y < GFX_HEIGHT; y++) {
            gfx_color_t *row = fb + (size_t)y * GFX_WIDTH;
            const gfx_color_t *photo_row =
                (const gfx_color_t *)boot_anim_image + (size_t)y * GFX_WIDTH;
            for (int x = 0; x < GFX_WIDTH; x++) {
                if (gfx_dither_covers(x, y, reveal)) {
                    row[x] = photo_row[x];
                }
            }
        }
    } else {
        /* Dissolving, and possibly still arriving at once - the same
         * dither test as above, but a covered pixel still needs ink's
         * own lift off black first (see the ink==255 branch's own
         * comment for why that lift happens at all). One multiply, only
         * for the pixels the dither actually selects this frame, not
         * every one of them the way the blend this replaced always
         * paid for regardless of how few pixels reveal's own coverage
         * asked for. */
        for (int y = 0; y < GFX_HEIGHT; y++) {
            gfx_color_t *row = fb + (size_t)y * GFX_WIDTH;
            const gfx_color_t *photo_row =
                (const gfx_color_t *)boot_anim_image + (size_t)y * GFX_WIDTH;
            for (int x = 0; x < GFX_WIDTH; x++) {
                if (gfx_dither_covers(x, y, reveal)) {
                    row[x] = gfx_color_mix(COL_BG, photo_row[x], ink);
                }
            }
        }
    }

    gfx_mark_all_dirty();
}

/*---------------------------------------------------------------------------
 * The loop
 *-------------------------------------------------------------------------*/

/* The whole-panel wipe boot_anim_draw_frame() opens with - see this file's
 * own top comment ("EVERY FRAME IS A FULL REPAINT") for why there is no
 * partial-clear path here the way app_cube.c's own cube_clear_frame() has
 * one. A separate, non-static function for the same reason every other
 * draw_* here now is: suite_boot_anim_perf.c times it as its own phase,
 * since it is real work (322 KiB) paid every frame regardless of what else
 * that frame draws - see gfx_clear()'s own measured cost in gfx.c. */
void boot_anim_clear_frame(void)
{
    gfx_clear(COL_BG);
}

void boot_anim_draw_frame(uint32_t now_ms)
{
    const uint8_t ink    = boot_anim_ink(now_ms);
    const uint8_t reveal = boot_anim_image_reveal(now_ms);
    const uint8_t scene  = boot_anim_scene_reach(now_ms);

    /* Reads now_ms directly and derives everything from it internally - see
     * boot_anim_view()'s own comment in boot_anim.h for where the composed
     * space-then-camera transform comes from. Built once here and threaded
     * to every draw_* call below rather than reconstructed per point: two
     * matrix builds and a multiply are worth paying for once a frame, not
     * once per point drawn. */
    const boot_anim_view_t view = boot_anim_view(GFX_WIDTH, GFX_HEIGHT,
                                                 now_ms);

    boot_anim_clear_frame();

    /* Gated the same way the title already is below: once the photograph
     * is about to fully cover the panel, this is a whole frame of drawing
     * draw_image() is going to overwrite pixel for pixel - see boot_anim_
     * scene_reach()'s own comment in boot_anim.h for why this is a plain
     * skip rather than a second alpha multiply threaded into `ink`. */
    if (scene > 0) {
        draw_floor(now_ms, ink, &view);
        draw_axes(now_ms, ink, &view);

        /* Zeros before the curve, so the curve's own glow lands on top of
         * them rather than the dots punching holes in it. */
        const int32_t reached = draw_curve(now_ms, ink, &view);
        draw_zeros(reached, ink, &view);
    }

    draw_image(ink, reveal);

    /* Last, so it lands on top of the curve early in the animation and on
     * top of the photograph later - see draw_image()'s own comment.
     * Gated rather than always called: every letter's own ramp is already
     * zero before BOOT_ANIM_TITLE_START_MS, so skipping the six gfx calls
     * entirely for the 2.7s before that is a real saving, not just tidiness. */
    if (now_ms >= BOOT_ANIM_TITLE_START_MS) {
        draw_title(now_ms, ink);
    }
}

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Windowed, not per-frame or once-at-the-end - main.c's own report_fps()
 * (the shell's post-boot loop) already makes that case, and a log line
 * costs several ms of UART, enough to throttle the very thing being
 * measured. Duplicated here rather than shared: boot_anim_run() has its
 * own loop, entirely separate from the shell's, so there is no one call
 * site the two could share this from.
 *
 * 500ms windows, not main.c's 1.5s - short enough to actually localize a
 * dip to a ~2s window (the photograph's own crossfade, draw_image()'s
 * full-framebuffer blend loop - see that function's own comment) rather
 * than average it away against six-odd seconds of much cheaper curve/grid
 * drawing. `now_ms` in the log line, not just an fps number, so a dip can
 * be read straight off against boot_anim_timeline.json's own authored
 * ms values without having to count log lines to find it. */
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
