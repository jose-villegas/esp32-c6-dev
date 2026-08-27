/*=============================================================================
 * ui_transform - a 2x3 affine transform for the UI layer, in fixed point.
 *
 * Every rect, icon, clip and text position microui hands to ui.c's
 * draw_command() is expressed in the UI's own LOGICAL coordinates - the ones
 * layout code lays widgets out in. This header is what turns a logical point
 * into the PHYSICAL point that actually lands on the panel, so a caller can
 * rotate (and later scale or translate) the whole UI without any call site
 * knowing: nothing above ui.c ever sees a physical coordinate.
 *
 * PURE GEOMETRY, SEPARATE FROM DRAWING
 *
 * Same split as ui_style.h documents for a bezel: this header only computes
 * WHERE things go, and touches neither gfx.c nor microui.c, so it stays
 * linkable on a host (see test/suites/suite_ui_transform.c). Only "microui.h"
 * is included, for mu_Rect - the same dependency ui_style.h takes and for the
 * same reason.
 *
 * FIXED POINT, NOT FLOAT
 *
 * This is RISC-V with no FPU. The transform is applied per rect, per glyph
 * and per icon, every repaint, so a software float emulation trap on every
 * multiply is not a cost worth paying for a shape this simple. Q16.16 has
 * plenty of range for a 368x448 panel (the panel's largest coordinate needs
 * only 9 bits) and plenty of precision for a rotate/scale/translate matrix.
 *
 * THE CONTRACT: A GENERAL TYPE, A NARROWER BACKEND
 *
 * ui_transform_t can express any 2x3 affine map - shear included. The
 * renderer underneath it cannot: gfx_fill_rect() and gfx_set_clip() are
 * axis-aligned only, the dirty tracker works in axis-aligned grid cells, and
 * gfx_text_turned() only knows quarter turns. So this renderer can honour
 * rotation by multiples of 90 degrees, translation, and scale - and nothing
 * else. A general matrix invites a caller to push a 30 degree rotation and
 * get silent misrendering back.
 *
 * ui_transform_is_axis_preserving() is the boundary between what the type can
 * express and what the backend can draw. ui.c's draw_command() classifies the
 * transform in force with it before painting a frame; see the comment there
 * for what happens when a transform fails the check. It is also the
 * extension point: if gfx ever grows a rotated blitter, the classifier widens
 * to admit more matrices and nothing above it - not this header, not ui.c's
 * callers - has to change.
 *
 * ROUNDING
 *
 * Every conversion back to an integer pixel rounds to nearest rather than
 * truncating, so a rect mapped and mapped back through ui_transform_invert()
 * lands where it started instead of drifting toward zero by up to a pixel
 * each way - see fp_round() below.
 *===========================================================================*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "microui.h"

#define UI_FP_SHIFT 16
#define UI_FP_ONE   (1 << UI_FP_SHIFT)

typedef int32_t ui_fp_t;

/* | a c tx |
   | b d ty |

   A point maps as ox = a*x + c*y + tx, oy = b*x + d*y + ty. */
typedef struct {
    ui_fp_t a, b, c, d, tx, ty;
} ui_transform_t;

/*---------------------------------------------------------------------------
 * Fixed-point helpers
 *
 * Not part of the public shape of this header, but static inline like
 * everything else here so they still link on their own.
 *-------------------------------------------------------------------------*/

/* Round v (a Q16.16 accumulator) to the nearest plain integer, ties away
 * from zero. Splitting on sign rather than just adding half and shifting
 * matters here: a plain arithmetic shift of a negative number rounds toward
 * -infinity, which would round -0.5px toward -1 and +0.5px toward +1 - a
 * one-pixel bias against the origin that a truncating cast does not have
 * either, but that plain rounding would introduce. */
static inline int64_t ui_fp_round(int64_t v)
{
    const int64_t half = (int64_t)1 << (UI_FP_SHIFT - 1);
    return (v >= 0) ? (v + half) >> UI_FP_SHIFT : -(((-v) + half) >> UI_FP_SHIFT);
}

/* Multiply two Q16.16 numbers, rounding the result to Q16.16. The product of
 * two Q16.16 ints is implicitly Q32.32; ui_fp_round() brings it back down. */
static inline ui_fp_t ui_fp_mul(ui_fp_t a, ui_fp_t b)
{
    return (ui_fp_t)ui_fp_round((int64_t)a * (int64_t)b);
}

/* Divide two Q16.16 numbers, rounding the result to Q16.16. `den` must be
 * nonzero - callers here only ever divide by a determinant already checked
 * against zero. */
static inline ui_fp_t ui_fp_div(ui_fp_t num, ui_fp_t den)
{
    const bool neg = (num < 0) != (den < 0);
    const int64_t n = ((int64_t)(num < 0 ? -num : num)) << UI_FP_SHIFT;
    const int64_t d = den < 0 ? -den : den;
    const int64_t q = (n + d / 2) / d;
    return (ui_fp_t)(neg ? -q : q);
}

/*---------------------------------------------------------------------------
 * Construction
 *-------------------------------------------------------------------------*/

static inline ui_transform_t ui_transform_identity(void)
{
    return (ui_transform_t){ UI_FP_ONE, 0, 0, UI_FP_ONE, 0, 0 };
}

/* Rotation by `turn` quarter turns (mod 4, negative allowed), about a
 * viewport `viewport_w` x `viewport_h` wide - the PHYSICAL panel, always
 * GFX_WIDTH x GFX_HEIGHT in practice, which does not itself change size when
 * the UI is turned.
 *
 * The domain this transform expects is therefore the LOGICAL canvas: for an
 * odd turn that is viewport_h wide and viewport_w tall (see ui_width() /
 * ui_height() in ui.h, which report exactly that swap), and for turn 0 or 2
 * it is viewport_w x viewport_h, same as the viewport itself.
 *
 * Each matrix entry is exactly 0, UI_FP_ONE or -UI_FP_ONE and every
 * translation is an exact integer scaled by UI_FP_ONE, so this is exact in
 * Q16.16 - no rounding error to compound turn after turn. The translation is
 * chosen so the domain's corners land exactly on the viewport's corners; see
 * suite_ui_transform.c's corner tests for the four cases spelled out. */
static inline ui_transform_t ui_transform_quarter_turn(int turn, int viewport_w,
                                                        int viewport_h)
{
    const int t = ((turn % 4) + 4) % 4;
    const ui_fp_t w = (ui_fp_t)viewport_w << UI_FP_SHIFT;
    const ui_fp_t h = (ui_fp_t)viewport_h << UI_FP_SHIFT;

    switch (t) {
    case 1: /* one quarter turn clockwise */
        return (ui_transform_t){ 0, UI_FP_ONE, -UI_FP_ONE, 0, w, 0 };
    case 2: /* half turn */
        return (ui_transform_t){ -UI_FP_ONE, 0, 0, -UI_FP_ONE, w, h };
    case 3: /* three quarter turns clockwise (one counter-clockwise) */
        return (ui_transform_t){ 0, -UI_FP_ONE, UI_FP_ONE, 0, 0, h };
    default:
        return ui_transform_identity();
    }
}

/* The transform equivalent to applying `inner` first and `outer` second -
 * i.e. the point mapping outer(inner(p)), matrix-multiplied as outer*inner.
 * Read the argument order the way you would read compose(f, g) meaning
 * "f then g". */
static inline ui_transform_t ui_transform_compose(ui_transform_t inner,
                                                   ui_transform_t outer)
{
    ui_transform_t out;
    out.a  = ui_fp_mul(outer.a, inner.a) + ui_fp_mul(outer.c, inner.b);
    out.c  = ui_fp_mul(outer.a, inner.c) + ui_fp_mul(outer.c, inner.d);
    out.tx = ui_fp_mul(outer.a, inner.tx) + ui_fp_mul(outer.c, inner.ty) + outer.tx;

    out.b  = ui_fp_mul(outer.b, inner.a) + ui_fp_mul(outer.d, inner.b);
    out.d  = ui_fp_mul(outer.b, inner.c) + ui_fp_mul(outer.d, inner.d);
    out.ty = ui_fp_mul(outer.b, inner.tx) + ui_fp_mul(outer.d, inner.ty) + outer.ty;
    return out;
}

/* Inverts `t` into `*out`. Returns false, leaving `*out` untouched, if `t` is
 * singular (zero determinant) - a caller pushing a degenerate transform (a
 * zero scale, say) gets told rather than handed nonsense. */
static inline bool ui_transform_invert(ui_transform_t t, ui_transform_t *out)
{
    const ui_fp_t det = ui_fp_mul(t.a, t.d) - ui_fp_mul(t.b, t.c);
    if (det == 0) {
        return false;
    }

    ui_transform_t inv;
    inv.a = ui_fp_div(t.d, det);
    inv.b = ui_fp_div(-t.b, det);
    inv.c = ui_fp_div(-t.c, det);
    inv.d = ui_fp_div(t.a, det);
    /* [x,y]' = Minv * ([ox,oy]' - [tx,ty]') = Minv*[ox,oy]' - Minv*[tx,ty]' */
    inv.tx = -(ui_fp_mul(inv.a, t.tx) + ui_fp_mul(inv.c, t.ty));
    inv.ty = -(ui_fp_mul(inv.b, t.tx) + ui_fp_mul(inv.d, t.ty));

    *out = inv;
    return true;
}

/*---------------------------------------------------------------------------
 * Application
 *-------------------------------------------------------------------------*/

static inline void ui_transform_point(ui_transform_t t, int x, int y, int *ox,
                                      int *oy)
{
    *ox = (int)ui_fp_round((int64_t)t.a * x + (int64_t)t.c * y + t.tx);
    *oy = (int)ui_fp_round((int64_t)t.b * x + (int64_t)t.d * y + t.ty);
}

/* Maps all four corners of `r` and returns their axis-aligned bounding box.
 *
 * This is a BOUNDING box, not a rotated rect - mu_Rect has no room to carry a
 * rotation, so a caller wanting the actual quadrilateral (nothing here does
 * yet) would have to map the corners itself. For anything
 * ui_transform_is_axis_preserving() accepts, the four mapped corners already
 * form an axis-aligned rectangle, so the bounding box IS the exact mapped
 * shape and nothing is lost. */
static inline mu_Rect ui_transform_rect(ui_transform_t t, mu_Rect r)
{
    int xs[4], ys[4];
    ui_transform_point(t, r.x,       r.y,       &xs[0], &ys[0]);
    ui_transform_point(t, r.x + r.w, r.y,       &xs[1], &ys[1]);
    ui_transform_point(t, r.x,       r.y + r.h, &xs[2], &ys[2]);
    ui_transform_point(t, r.x + r.w, r.y + r.h, &xs[3], &ys[3]);

    int min_x = xs[0], max_x = xs[0], min_y = ys[0], max_y = ys[0];
    for (int i = 1; i < 4; i++) {
        if (xs[i] < min_x) min_x = xs[i];
        if (xs[i] > max_x) max_x = xs[i];
        if (ys[i] < min_y) min_y = ys[i];
        if (ys[i] > max_y) max_y = ys[i];
    }
    return (mu_Rect){ min_x, min_y, max_x - min_x, max_y - min_y };
}

/* Which of gfx_text_turned()'s four quarters `t` represents, for a caller
 * that needs to draw text through it - see ui.c's draw_command(). Only the
 * SIGN pattern of the linear part is read, not its magnitude, so this still
 * answers correctly under a scaled transform (see ui_transform_compose()
 * with a quarter turn folded into a scale). Meaningless if
 * ui_transform_is_axis_preserving(t) is false; callers are expected to check
 * that first, exactly as draw_command() does. */
static inline int ui_transform_quarter(ui_transform_t t)
{
    if (t.a > 0 && t.d > 0) return 0;
    if (t.b > 0 && t.c < 0) return 1;
    if (t.a < 0 && t.d < 0) return 2;
    if (t.b < 0 && t.c > 0) return 3;
    return 0; /* not a rotation at all (e.g. the zero matrix) - identity is
                 the least wrong answer, and is_axis_preserving() would have
                 already rejected this transform anyway. */
}

/* Whether `t` maps axis-aligned rectangles to axis-aligned rectangles - the
 * exact geometric condition under which gfx_fill_rect(), gfx_set_clip() and
 * the grid dirty tracker (all axis-aligned by construction) can still be
 * trusted, and gfx_text_turned() has a quarter turn to be given. See this
 * header's top comment for why the type is wider than this.
 *
 * The condition is that each of the two logical axes maps onto ONE physical
 * axis, not a mix of both: either the x/y columns of the matrix keep to
 * their own axis (b == c == 0, an unrotated - possibly scaled - transform),
 * or they swap axes cleanly (a == d == 0, a 90 or 270 degree turn). A shear
 * or an arbitrary-angle rotation has a nonzero entry in both columns of at
 * least one axis and satisfies neither, so it is rejected. The `!= 0`
 * conjuncts additionally rule out the degenerate case of a zero scale on the
 * kept axis, which is not invertible and so not usable as a transform at
 * all. */
static inline bool ui_transform_is_axis_preserving(ui_transform_t t)
{
    if (t.b == 0 && t.c == 0) {
        return t.a != 0 && t.d != 0;
    }
    if (t.a == 0 && t.d == 0) {
        return t.b != 0 && t.c != 0;
    }
    return false;
}
