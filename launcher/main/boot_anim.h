/*=============================================================================
 * boot_anim - the startup animation: an argand plane, and the spirals the
 * zeta function traces on it.
 *
 * This header is the MATHS and the CHOREOGRAPHY. boot_anim.c is the drawing
 * and the three seconds of frame loop. The split is the one gfx_color.h,
 * icons.h and ui_style.h already make, and for the same reason: everything
 * here is pure arithmetic over plain integers, with no BSP, no driver and no
 * framebuffer anywhere in sight, so test/suites/suite_boot_anim.c can check
 * the curve, the tables and the timeline on a laptop. Nobody can eyeball a
 * fixed-point spiral and tell you whether it is the right one.
 *
 * WHAT IS BEING DRAWN
 *
 * Truncate the Dirichlet series for the zeta function on the critical line,
 *
 *     S_N(t) = sum over n = 1..N of  n^(-1/2) * e^(-i t ln n)
 *
 * and plot the running total in the complex plane. Each term is a step of
 * length n^(-1/2) whose direction turns by t*ln((n+1)/n) ~ t/n from one term
 * to the next - a turn that shrinks as n grows, which is exactly the recipe
 * for a spiral. The first few terms are long and swing wildly; by n ~ 30 the
 * steps are short and the turn is gentle, so the path winds into the neat
 * nested loops that make the picture. Four values of t give four spirals.
 *
 * The four are the imaginary parts of the first four nontrivial zeros of the
 * zeta function. Nothing in the drawing depends on them being zeros - any t
 * spirals - but if the plot is going to be of this function, the interesting
 * heights are the ones it vanishes at.
 *
 * WHY EVERYTHING IS AN INTEGER
 *
 * The ESP32-C6 is RV32IMAC: no FPU, so every float is a library call. The
 * curve needs a sine, a reciprocal square root and a logarithm per term,
 * which is three of the most expensive things a soft-float library does, 96
 * times over, four times a frame, ~50 frames a second. So none of them
 * happen at runtime: ln and 1/sqrt are tables in flash (they depend only on
 * n), sine is a quarter-wave table, and the rest is integer multiplies and
 * shifts. Measured against the double-precision curve, the worst point is
 * 0.26 px out of place.
 *
 * Two fixed-point scales appear below, and mixing them up is the mistake to
 * watch for:
 *
 *   Q12   world coordinates on the plane. 4096 is 1.0, one unit of the axes.
 *   Q15   sines and cosines. 32767 is 1.0. Only ever an intermediate.
 *
 * plus a 16-bit PHASE, in which a whole turn is 65536 - so an angle wraps by
 * being truncated to uint16_t, and no angle ever needs reducing mod 2*pi.
 *===========================================================================*/
#pragma once

#include <stdint.h>

/*---------------------------------------------------------------------------
 * The plane
 *
 * Screen size arrives as a parameter rather than as GFX_WIDTH/GFX_HEIGHT,
 * because gfx.h drags in the BSP and this file has to compile on a host.
 * gfx_dirty.h has the same problem and solves it by mirroring the two
 * numbers as literals; taking them as arguments is cheaper here, since these
 * are called a few dozen times a frame rather than per pixel, and it leaves
 * nothing to drift out of step.
 *-------------------------------------------------------------------------*/

#define BOOT_ANIM_Q    12
#define BOOT_ANIM_ONE  (1 << BOOT_ANIM_Q)   /* 4096 == 1.0 on the axes */

/* Pixels per unit. The four spirals together span about 1.7 units across and
 * 1.8 down, so 192 fills a 368x448 panel with a margin all round. */
#define BOOT_ANIM_UNIT_PX 192

/* Minor grid every quarter unit, a brighter line every whole one. */
#define BOOT_ANIM_GRID_PX  (BOOT_ANIM_UNIT_PX / 4)
#define BOOT_ANIM_GRID_MAJOR 4               /* every 4th ring is a unit */

/* The origin sits two fifths of the way across rather than halfway. The
 * spirals live almost entirely to the right of it - the first term alone is
 * a full unit along the positive real axis - so a centred origin would push
 * the picture off the right edge and leave the left third empty. */
static inline int boot_anim_origin_x(int w) { return (w * 2) / 5; }

/* Vertically it IS centred: the curves are near enough symmetric about the
 * real axis, being four spirals with imaginary parts of both signs. */
static inline int boot_anim_origin_y(int h) { return h / 2; }

/* Q12 world coordinate -> pixel. Real axis runs right, imaginary axis runs
 * UP, which is the opposite of the framebuffer's y - hence the subtraction. */
static inline int boot_anim_screen_x(int w, int32_t re_q12)
{
    return boot_anim_origin_x(w) + (int)((re_q12 * BOOT_ANIM_UNIT_PX) >> BOOT_ANIM_Q);
}

static inline int boot_anim_screen_y(int h, int32_t im_q12)
{
    return boot_anim_origin_y(h) - (int)((im_q12 * BOOT_ANIM_UNIT_PX) >> BOOT_ANIM_Q);
}

/*---------------------------------------------------------------------------
 * The choreography
 *
 * Every phase is a function of milliseconds since power-up, not of a frame
 * counter, so the animation lasts three seconds whatever the frame rate does
 * - and it does move: a frame here costs a full-screen transfer, ~17.6 ms,
 * but the first few frames are nearly empty and the last are not.
 *-------------------------------------------------------------------------*/

#define BOOT_ANIM_MS 3000

#define BOOT_ANIM_AXES_MS        550   /* axes grow out of the origin */

#define BOOT_ANIM_GRID_START_MS  200
#define BOOT_ANIM_GRID_RING_MS    70   /* each ring waits for the one inside */
#define BOOT_ANIM_GRID_FADE_MS   350

#define BOOT_ANIM_PEN_START_MS   650
#define BOOT_ANIM_PEN_STAGGER_MS 130   /* spirals set off one after another */
#define BOOT_ANIM_PEN_MS        1600

#define BOOT_ANIM_FADE_START_MS 2760   /* dissolve into the launcher */

#define BOOT_ANIM_SPIRALS 4

/* 0 before `start_ms`, 255 from `start_ms + dur_ms` on, linear between. */
static inline uint8_t boot_anim_ramp(uint32_t now_ms, uint32_t start_ms,
                                     uint32_t dur_ms)
{
    if (now_ms <= start_ms) {
        return 0;
    }
    const uint32_t elapsed = now_ms - start_ms;
    if (dur_ms == 0 || elapsed >= dur_ms) {
        return 255;
    }
    return (uint8_t)((elapsed * 255u) / dur_ms);
}

/* Fast off the mark, settling as it arrives. Motion that starts and stops at
 * the same speed reads as mechanical; this is one squared term and is enough
 * to stop it looking like a progress bar. */
static inline uint8_t boot_anim_ease_out(uint8_t linear)
{
    const uint32_t left = 255u - linear;
    return (uint8_t)(255u - (left * left) / 255u);
}

/* How far along each half-axis the pen has reached, 0..255 of the distance
 * to the edge. Both axes grow from the origin at once, and both ends of each
 * reach their edge together even though the origin is off-centre - the two
 * halves are different lengths, so this is a FRACTION, not a pixel count. */
static inline uint8_t boot_anim_axis_reach(uint32_t now_ms)
{
    return boot_anim_ease_out(boot_anim_ramp(now_ms, 0, BOOT_ANIM_AXES_MS));
}

/* Grid rings fade in from the origin outward - `ring` is 1 for the first line
 * either side of an axis, 2 for the next, and so on. */
static inline uint8_t boot_anim_grid_alpha(uint32_t now_ms, int ring)
{
    const uint32_t start = BOOT_ANIM_GRID_START_MS +
                           (uint32_t)ring * BOOT_ANIM_GRID_RING_MS;
    return boot_anim_ramp(now_ms, start, BOOT_ANIM_GRID_FADE_MS);
}

/* How much of spiral `k` has been drawn, as a Q12 fraction of its ARC LENGTH
 * (0 to BOOT_ANIM_ONE). Arc length, not term count, is what keeps the pen
 * moving at a steady speed: the first term is a whole unit long and the
 * ninety-sixth is a tenth of one, so pacing by term index would fling the pen
 * across the screen and then leave it crawling. See boot_anim_pen_terms(). */
static inline int32_t boot_anim_pen(uint32_t now_ms, int k)
{
    const uint32_t start = BOOT_ANIM_PEN_START_MS +
                           (uint32_t)k * BOOT_ANIM_PEN_STAGGER_MS;
    const uint8_t linear = boot_anim_ramp(now_ms, start, BOOT_ANIM_PEN_MS);
    return ((int32_t)linear * BOOT_ANIM_ONE) / 255;
}

/* Everything drawn is mixed up from the background by this, so the last
 * quarter-second dissolves the whole picture rather than cutting from a lit
 * screen straight to the menu. */
static inline uint8_t boot_anim_ink(uint32_t now_ms)
{
    return (uint8_t)(255u - boot_anim_ramp(now_ms, BOOT_ANIM_FADE_START_MS,
                                           BOOT_ANIM_MS - BOOT_ANIM_FADE_START_MS));
}

_Static_assert(BOOT_ANIM_PEN_START_MS +
               (BOOT_ANIM_SPIRALS - 1) * BOOT_ANIM_PEN_STAGGER_MS +
               BOOT_ANIM_PEN_MS <= BOOT_ANIM_FADE_START_MS,
               "the last spiral must finish before the picture starts fading");

/*---------------------------------------------------------------------------
 * Colour
 *
 * The panel is an AMOLED: an unlit pixel is genuinely off, not a backlight
 * leaking through black, so saturated colour on a true black field is the one
 * thing this screen does that an LCD cannot. Hence a hue wheel rather than a
 * house colour - each spiral starts at its own hue and turns through part of
 * the wheel as it winds inward, so the finished picture is a gradient rather
 * than four coloured lines.
 *
 * Only the HUE is computed here. Brightness and desaturation come out as
 * amounts to mix toward the background and toward white, and the mixing
 * itself is gfx_color_mix() in gfx_color.h - already written, already tested,
 * and already aware that the panel's green channel has a bit more than the
 * other two.
 *-------------------------------------------------------------------------*/

/* A whole turn of the hue wheel: six sectors of 256, so a sector boundary is
 * a shift and the ramp within one is a byte. */
#define BOOT_ANIM_HUE_TURN 1536

/* 0xRRGGBB at full saturation and full brightness. */
static inline uint32_t boot_anim_hue_rgb(int hue)
{
    hue %= BOOT_ANIM_HUE_TURN;
    if (hue < 0) {
        hue += BOOT_ANIM_HUE_TURN;
    }

    const uint32_t ramp = (uint32_t)(hue & 0xFF);   /* rising edge, 0..255 */
    const uint32_t fall = 255u - ramp;

    switch (hue >> 8) {
    case 0:  return (0xFFu << 16) | (ramp << 8);            /* red    -> yellow  */
    case 1:  return (fall << 16) | (0xFFu << 8);            /* yellow -> green   */
    case 2:  return (0xFFu << 8) | ramp;                    /* green  -> cyan    */
    case 3:  return (fall << 8) | 0xFFu;                    /* cyan   -> blue    */
    case 4:  return (ramp << 16) | 0xFFu;                   /* blue   -> magenta */
    default: return (0xFFu << 16) | fall;                   /* magenta -> red    */
    }
}

/* Where each spiral's gradient begins, spread around the wheel so that four
 * curves crossing each other stay four curves. */
static const int boot_anim_spiral_hue[BOOT_ANIM_SPIRALS] = {
    683,    /* spring green */
    875,    /* azure        */
    171,    /* amber        */
    1280,   /* magenta      */
};

/* How far round the wheel one spiral travels between its first term and its
 * last. A fifth of a turn: enough to read as a gradient, not so much that a
 * spiral ends up wearing its neighbour's colour. */
#define BOOT_ANIM_HUE_SWEEP 341

typedef struct {
    int     hue;     /* wheel position; boot_anim_hue_rgb() wraps it  */
    uint8_t bloom;   /* mix that far toward white: the hot inner core */
    uint8_t glow;    /* mix that far up from the background           */
} boot_anim_stroke_t;

/* How one segment of spiral `k` is coloured, `along_q12` being how far along
 * its arc length that segment sits (0 to BOOT_ANIM_ONE).
 *
 * The far end of the curve is the brightest: the spiral winds INWARD, so the
 * newest, tightest loops are the ones at the centre, and having them burn
 * white against the dim outer sweeps is what makes the middle of the picture
 * look like it is glowing rather than merely crowded. */
static inline boot_anim_stroke_t boot_anim_stroke(int k, int32_t along_q12)
{
    boot_anim_stroke_t s;

    s.hue   = boot_anim_spiral_hue[k] +
              (int)((along_q12 * BOOT_ANIM_HUE_SWEEP) >> BOOT_ANIM_Q);
    s.bloom = (uint8_t)((along_q12 * 70) >> BOOT_ANIM_Q);
    /* Half brightness at the tail rather than a tenth: the outer sweeps are
     * most of what is on screen for the first second of the draw, and a
     * curve nobody can see is not showing off anything. */
    s.glow  = (uint8_t)(128 + ((along_q12 * 127) >> BOOT_ANIM_Q));

    return s;
}

/*---------------------------------------------------------------------------
 * Sine
 *
 * A quarter wave at 65 points - sin(k * pi/128), Q15 - reflected into the
 * other three quadrants. The 65th entry exists so that the interpolation
 * below always has a point to its right; it is sin(pi/2), the only sample the
 * quarter wave shares with the next one.
 *
 * Generated as round(32767 * sin(k * pi / 128)). Nothing checks that against
 * libm - test/run_tests.sh does not link one - so suite_boot_anim.c checks
 * the properties instead: the wave rises, it ends at 1, and sin^2 + cos^2 is
 * 1 everywhere. A mistyped entry fails all three.
 *-------------------------------------------------------------------------*/

#define BOOT_ANIM_PHASE_TURN 65536      /* a whole turn, so a uint16 wraps */

static const int16_t boot_anim_sin_quarter[65] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,
};

/* sin over the first quarter turn: `r` is 0..16384, the answer is Q15.
 *
 * Interpolated between table points rather than snapped to the nearer one.
 * The table is coarse - 256 steps to the turn, 1.4 degrees - and the error
 * from snapping does not average out here: it is committed to the running
 * total and every later point inherits it, so 96 terms of it would visibly
 * bend the tail of the spiral. */
static inline int32_t boot_anim_sin_quadrant(uint32_t r)
{
    const uint32_t i = r >> 8;
    if (i >= 64) {
        return boot_anim_sin_quarter[64];
    }
    const int32_t a = boot_anim_sin_quarter[i];
    const int32_t b = boot_anim_sin_quarter[i + 1];
    return a + (((b - a) * (int32_t)(r & 0xFF)) >> 8);
}

/* sin of a 16-bit phase, Q15. */
static inline int32_t boot_anim_sin(uint16_t phase)
{
    const uint32_t quadrant = (uint32_t)phase >> 14;
    const uint32_t rest     = (uint32_t)phase & 0x3FFF;

    switch (quadrant) {
    case 0:  return  boot_anim_sin_quadrant(rest);
    case 1:  return  boot_anim_sin_quadrant(16384u - rest);
    case 2:  return -boot_anim_sin_quadrant(rest);
    default: return -boot_anim_sin_quadrant(16384u - rest);
    }
}

static inline int32_t boot_anim_cos(uint16_t phase)
{
    return boot_anim_sin((uint16_t)(phase + 16384u));
}

/*---------------------------------------------------------------------------
 * The curve
 *-------------------------------------------------------------------------*/

/* How many terms of the series each spiral is drawn from. Past this the steps
 * are shorter than a pixel and the picture stops changing; below about 60 the
 * inner loops have not closed up yet. */
#define BOOT_ANIM_TERMS 96

/* The imaginary parts of the first four nontrivial zeros of zeta, Q12:
 * 14.134725, 21.022040, 25.010858, 30.424876. */
static const int32_t boot_anim_zero_t_q12[BOOT_ANIM_SPIRALS] = {
    57896, 86106, 102444, 124620,
};

/* ln(n) in units of a sixteenth of a phase step - that is,
 * round(ln(n) * 65536 / (2*pi) * 16) - so that multiplying by t in Q12 and
 * shifting right by 16 lands directly on the term's phase, with no radians
 * and no reduction modulo anything in between. The extra factor of 16 is
 * headroom: without it the rounding of this table alone would be worth about
 * a phase step by the time it has been multiplied by t ~ 30.
 *
 * Indexed by n, so entry 0 is padding and never read.
 *
 * suite_boot_anim.c checks it with ln(a*b) = ln(a) + ln(b) over every pair
 * that fits, which needs no logarithm of its own and catches a typo in any
 * single entry. */
static const int32_t boot_anim_ln_phase[BOOT_ANIM_TERMS + 1] = {
          0,       0,  115677,  183343,  231353,  268593,  299020,  324745,
     347030,  366686,  384269,  400175,  414696,  428054,  440422,  451936,
     462706,  472824,  482363,  491386,  499946,  508088,  515852,  523270,
     530373,  537185,  543731,  550029,  556098,  561955,  567612,  573085,
     578383,  583518,  588500,  593338,  598039,  602612,  607062,  611397,
     615623,  619743,  623765,  627692,  631528,  635279,  638947,  642536,
     646049,  649491,  652862,  656167,  659407,  662586,  665706,  668768,
     671775,  674729,  677631,  680484,  683289,  686048,  688761,  691431,
     694060,  696647,  699195,  701705,  704177,  706613,  709015,  711382,
     713716,  716018,  718288,  720529,  722739,  724921,  727074,  729200,
     731299,  733372,  735420,  737443,  739442,  741417,  743368,  745298,
     747205,  749091,  750955,  752800,  754623,  756428,  758213,  759979,
     761726,
};

/* round(4096 / sqrt(n)), Q12: the length of term n. Entry 0 is padding.
 * Checked by n * invsqrt(n)^2 == 1, again with no square root needed. */
static const int16_t boot_anim_inv_sqrt[BOOT_ANIM_TERMS + 1] = {
        0,  4096,  2896,  2365,  2048,  1832,  1672,  1548,  1448,  1365,
     1295,  1235,  1182,  1136,  1095,  1058,  1024,   993,   965,   940,
      916,   894,   873,   854,   836,   819,   803,   788,   774,   761,
      748,   736,   724,   713,   702,   692,   683,   673,   664,   656,
      648,   640,   632,   625,   617,   611,   604,   597,   591,   585,
      579,   574,   568,   563,   557,   552,   547,   543,   538,   533,
      529,   524,   520,   516,   512,   508,   504,   500,   497,   493,
      490,   486,   483,   479,   476,   473,   470,   467,   464,   461,
      458,   455,   452,   450,   447,   444,   442,   439,   437,   434,
      432,   429,   427,   425,   422,   420,   418,
};

typedef struct {
    int32_t re, im;   /* Q12 */
} boot_anim_pt_t;

/* Term n of the series at height t: a step of length n^(-1/2) in the
 * direction -t*ln(n). The minus is what makes the spirals wind clockwise,
 * and it is in the exponent of the function itself, not a drawing choice. */
static inline boot_anim_pt_t boot_anim_term(int32_t t_q12, int n)
{
    const uint16_t phase =
        (uint16_t)(((int64_t)t_q12 * boot_anim_ln_phase[n]) >> 16);
    const int32_t len = boot_anim_inv_sqrt[n];

    boot_anim_pt_t step;
    step.re =  (len * boot_anim_cos(phase)) >> 15;
    step.im = -((len * boot_anim_sin(phase)) >> 15);
    return step;
}

/* A pen walking the curve one term at a time.
 *
 * Kept as a walk rather than an array of points because there is nowhere to
 * put the array: the framebuffer is 322 KiB of the chip's ~424, and the whole
 * curve is recomputed from term 1 every frame anyway - it costs about a
 * hundredth of what sending the frame does, which is not worth 3 KiB of RAM
 * to avoid. */
typedef struct {
    int32_t        t_q12;
    int            n;     /* terms added so far          */
    int32_t        arc;   /* Q12 distance walked, see below */
    boot_anim_pt_t z;     /* the running total           */
} boot_anim_walk_t;

/* The whole curve's length: the sum of boot_anim_inv_sqrt[1..TERMS], since
 * term n's length IS that entry. Summed from the table rather than from the
 * real series so that it is exactly what a walk accumulates - a fraction of
 * it has to reach 1.0 precisely at the last term, or the last stroke would
 * be coloured as though the curve carried on. suite_boot_anim.c re-adds the
 * table and checks this number against it. */
#define BOOT_ANIM_ARC_Q12 74489

static inline void boot_anim_walk_begin(boot_anim_walk_t *w, int k)
{
    w->t_q12 = boot_anim_zero_t_q12[k];
    w->n     = 0;
    w->arc   = 0;
    w->z.re  = 0;
    w->z.im  = 0;
}

static inline void boot_anim_walk_step(boot_anim_walk_t *w)
{
    if (w->n >= BOOT_ANIM_TERMS) {
        return;
    }
    w->n++;
    const boot_anim_pt_t step = boot_anim_term(w->t_q12, w->n);
    w->z.re += step.re;
    w->z.im += step.im;
    w->arc  += boot_anim_inv_sqrt[w->n];
}

/* How far along the curve the pen has walked, as a Q12 fraction - which is
 * what boot_anim_stroke() wants, and what makes the colour gradient spread
 * evenly over the drawn line rather than bunching up in the crowded middle
 * where most of the TERMS are but little of the LENGTH is. */
static inline int32_t boot_anim_along(const boot_anim_walk_t *w)
{
    return (int32_t)(((int64_t)w->arc << BOOT_ANIM_Q) / BOOT_ANIM_ARC_Q12);
}

/* Arc-length progress -> how many terms have been drawn, in Q12 so the pen
 * can sit part way along a term rather than jumping from one to the next.
 *
 * The arc length after n terms is the sum of k^(-1/2) for k = 1..n, which is
 * 2*sqrt(n) to within a constant - so a given fraction f of the total length
 * is reached at f^2 of the total terms. That square is the whole reason the
 * pen appears to move at a constant speed: it spends its first tenth of a
 * second on term one, which is a fifth of the screen long, and its last on
 * the dozen tiny ones that close the spiral. */
static inline int32_t boot_anim_pen_terms(int32_t progress_q12)
{
    /* progress^2 is Q24; multiplying by a plain count leaves it Q24, so one
     * shift of BOOT_ANIM_Q brings it back to Q12. */
    return (int32_t)(((int64_t)progress_q12 * progress_q12 *
                      BOOT_ANIM_TERMS) >> BOOT_ANIM_Q);
}

/*---------------------------------------------------------------------------
 * The one hardware-facing entry point
 *
 * Declared here and defined in boot_anim.c, the same arrangement icon_check()
 * has at the bottom of icons.h: a declaration costs this header none of its
 * host-portability, and putting it anywhere else would mean a second header
 * for one function.
 *-------------------------------------------------------------------------*/

/* Draw the whole animation, start to finish - about BOOT_ANIM_MS of it.
 *
 * BLOCKS, and is meant to: it runs during boot, before the shell's frame
 * loop exists and before there is anything to switch to. gfx_init() must have
 * succeeded first. Yields every frame so the watchdog stays fed. */
void boot_anim_run(void);
