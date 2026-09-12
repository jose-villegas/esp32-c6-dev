/*
 * ui_transform - a 2x3 affine transform for the UI layer, in fixed point.
 *
 * Every rect, icon, clip and text position microui hands to ui.c is in the
 * UI's own LOGICAL coordinates. This turns a logical point into the PHYSICAL
 * one that lands on the panel, so the whole UI can be rotated without any call
 * site knowing: nothing above ui.c ever sees a physical coordinate. Pure
 * geometry, which is what keeps it linkable on a host.
 *
 * Fixed point because this is RISC-V with no FPU, and the transform is applied
 * per rect, per glyph and per icon on every repaint - a software float trap on
 * every multiply is not worth paying for a shape this simple. Q16.16 has ample
 * range for a 368x448 panel and ample precision for rotate, scale, translate.
 *
 * The type is more general than the backend, and that gap is the trap.
 * ui_transform_t can express any affine map, shear included, while
 * gfx_fill_rect() and gfx_set_clip() are axis-aligned only, the dirty tracker
 * works in axis-aligned grid cells, and gfx_text_turned() knows only quarter
 * turns. ui_transform_is_axis_preserving() is the boundary between the two,
 * and the extension point if gfx ever grows a rotated blitter; without it a
 * caller could push a 30 degree rotation and get silent misrendering back.
 *
 * Conversions back to an integer pixel round to nearest rather than
 * truncating, so a rect mapped and inverted lands where it started.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx/gfx_font.h"
#include "gfx/icon.h"
#include "microui.h"
#include "util/fixed.h"

#define UI_FP_SHIFT 16
#define UI_FP_ONE   (1 << UI_FP_SHIFT)

typedef int32_t ui_fp_t;

/* | a c tx |
   | b d ty |

   A point maps as ox = a*x + c*y + tx, oy = b*x + d*y + ty. */
typedef struct {
    ui_fp_t a, b, c, d, tx, ty;
} ui_transform_t;

/*
 * Thin wrappers over util/fixed.h, where the arithmetic and its
 * floor-vs-round reasoning live: fixed-point multiply is not a UI concept,
 * and other callers need the same operation at a different shift.
 *
 * ui_fp_round() keeps its own shape because it rounds an already-computed
 * ACCUMULATOR, not two raw operands, which fx_mul_round() cannot accept. It
 * delegates to fx_round_shift(), so the rounding rule lives in one place.
 */

static inline int64_t
ui_fp_round(int64_t v) {
    return fx_round_shift(v, UI_FP_SHIFT);
}

/* Multiply two Q16.16 numbers, rounding the result to Q16.16. The product of
 * two Q16.16 ints is implicitly Q32.32; fx_mul_round() brings it back down. */
static inline ui_fp_t
ui_fp_mul(ui_fp_t a, ui_fp_t b) {
    return (ui_fp_t)fx_mul_round(a, b, UI_FP_SHIFT);
}

/* Divide two Q16.16 numbers, rounding the result to Q16.16. `den` must be
 * nonzero - callers here only ever divide by a determinant already checked
 * against zero. */
static inline ui_fp_t
ui_fp_div(ui_fp_t num, ui_fp_t den) {
    return (ui_fp_t)fx_div_round(num, den, UI_FP_SHIFT);
}

/* Construction */

static inline ui_transform_t
ui_transform_identity(void) {
    return (ui_transform_t){UI_FP_ONE, 0, 0, UI_FP_ONE, 0, 0};
}

/* Rotation by `turn` quarter turns (mod 4, negative allowed), about a
 * viewport `viewport_w` x `viewport_h` wide - the PHYSICAL panel,
 * unchanged in size when the UI turns. The domain expected is the
 * LOGICAL canvas: for an odd turn that is viewport_h wide, viewport_w
 * tall (see ui_width()/ui_height()), for turn 0 or 2 it is viewport_w x
 * viewport_h. Each matrix entry is exactly 0, UI_FP_ONE or -UI_FP_ONE,
 * translations exact integers scaled by UI_FP_ONE - exact in Q16.16, no
 * rounding to compound. */
static inline ui_transform_t
ui_transform_quarter_turn(int turn, int viewport_w, int viewport_h) {
    const int t = ((turn % 4) + 4) % 4;
    const ui_fp_t w = (ui_fp_t)viewport_w << UI_FP_SHIFT;
    const ui_fp_t h = (ui_fp_t)viewport_h << UI_FP_SHIFT;

    switch (t) {
        case 1: /* one quarter turn clockwise */ return (ui_transform_t){0, UI_FP_ONE, -UI_FP_ONE, 0, w, 0};
        case 2: /* half turn */ return (ui_transform_t){-UI_FP_ONE, 0, 0, -UI_FP_ONE, w, h};
        case 3: /* three quarter turns clockwise (one counter-clockwise) */
            return (ui_transform_t){0, -UI_FP_ONE, UI_FP_ONE, 0, 0, h};
        default: return ui_transform_identity();
    }
}

/* The transform equivalent to applying `inner` first and `outer` second -
 * i.e. the point mapping outer(inner(p)), matrix-multiplied as outer*inner.
 * Read the argument order the way you would read compose(f, g) meaning
 * "f then g". */
static inline ui_transform_t
ui_transform_compose(ui_transform_t inner, ui_transform_t outer) {
    ui_transform_t out;
    out.a = ui_fp_mul(outer.a, inner.a) + ui_fp_mul(outer.c, inner.b);
    out.c = ui_fp_mul(outer.a, inner.c) + ui_fp_mul(outer.c, inner.d);
    out.tx = ui_fp_mul(outer.a, inner.tx) + ui_fp_mul(outer.c, inner.ty) + outer.tx;

    out.b = ui_fp_mul(outer.b, inner.a) + ui_fp_mul(outer.d, inner.b);
    out.d = ui_fp_mul(outer.b, inner.c) + ui_fp_mul(outer.d, inner.d);
    out.ty = ui_fp_mul(outer.b, inner.tx) + ui_fp_mul(outer.d, inner.ty) + outer.ty;
    return out;
}

/* Inverts `t` into `*out`. Returns false, leaving `*out` untouched, if `t` is
 * singular (zero determinant) - a caller pushing a degenerate transform (a
 * zero scale, say) gets told rather than handed nonsense. */
static inline bool
ui_transform_invert(ui_transform_t t, ui_transform_t* out) {
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

/* Application */

static inline void
ui_transform_point(ui_transform_t t, int x, int y, int* ox, int* oy) {
    *ox = (int)ui_fp_round((int64_t)t.a * x + (int64_t)t.c * y + t.tx);
    *oy = (int)ui_fp_round((int64_t)t.b * x + (int64_t)t.d * y + t.ty);
}

/* Maps all four corners of `r` and returns their axis-aligned bounding
 * box. This is a BOUNDING box, not a rotated rect - mu_Rect has no room
 * to carry a rotation, so a caller wanting the actual quadrilateral
 * (nothing here does yet) would have to map the corners itself. For
 * anything ui_transform_is_axis_preserving() accepts, the four mapped
 * corners already form an axis-aligned rectangle, so the bounding box
 * IS the exact mapped shape and nothing is lost. */
static inline mu_Rect
ui_transform_rect(ui_transform_t t, mu_Rect r) {
    int xs[4], ys[4];
    ui_transform_point(t, r.x, r.y, &xs[0], &ys[0]);
    ui_transform_point(t, r.x + r.w, r.y, &xs[1], &ys[1]);
    ui_transform_point(t, r.x, r.y + r.h, &xs[2], &ys[2]);
    ui_transform_point(t, r.x + r.w, r.y + r.h, &xs[3], &ys[3]);

    int min_x = xs[0], max_x = xs[0], min_y = ys[0], max_y = ys[0];
    for (int i = 1; i < 4; i++) {
        if (xs[i] < min_x) {
            min_x = xs[i];
        }
        if (xs[i] > max_x) {
            max_x = xs[i];
        }
        if (ys[i] < min_y) {
            min_y = ys[i];
        }
        if (ys[i] > max_y) {
            max_y = ys[i];
        }
    }
    return (mu_Rect){min_x, min_y, max_x - min_x, max_y - min_y};
}

/* icon_walk_blocks()'s callback context, adapted to transform each run
 * before it reaches the caller's own `emit` - see ui_transform_icon_blocks()
 * below for why this exists instead of transforming `box` once up front. */
typedef struct {
    ui_transform_t t;
    int box_x, box_y;
    icon_emit_fn emit;
    void* ctx;
} ui_transform_icon_ctx_t;

static inline void
ui_transform_icon_emit(void* ctx, int x, int y, int w, int h) {
    const ui_transform_icon_ctx_t* ic = ctx;
    const mu_Rect local = {ic->box_x + x, ic->box_y + y, w, h};
    const mu_Rect dst = ui_transform_rect(ic->t, local);
    ic->emit(ic->ctx, dst.x, dst.y, dst.w, dst.h);
}

/* icon_walk_blocks() (gfx/icon.h), with each emitted run mapped through `t`
 * before `emit` sees it, not just the enclosing `box` - transforming only
 * the box lands it correctly but leaves the glyph inside it upright under
 * any quarter turn, the bug MU_COMMAND_RECT/_TEXT never had. `box` is
 * LOGICAL, the same one microui's command carries. */
static inline void
ui_transform_icon_blocks(ui_transform_t t, const uint8_t* rows, int iw, int ih, int stride, mu_Rect box,
                         icon_emit_fn emit, void* ctx) {
    ui_transform_icon_ctx_t ic = {.t = t, .box_x = box.x, .box_y = box.y, .emit = emit, .ctx = ctx};
    icon_walk_blocks(rows, iw, ih, stride, box.w, box.h, ui_transform_icon_emit, &ic);
}

/* Which of gfx_text_turned()'s four quarters `t` represents, for a
 * caller that needs to draw text through it - see ui.c's
 * draw_command(). Only the SIGN pattern of the linear part is read, not
 * its magnitude, so this still answers correctly under a scaled
 * transform (see ui_transform_compose() with a quarter turn folded into
 * a scale). Meaningless if ui_transform_is_axis_preserving(t) is false;
 * callers are expected to check that first, exactly as draw_command()
 * does. */
static inline int
ui_transform_quarter(ui_transform_t t) {
    if (t.a > 0 && t.d > 0) {
        return 0;
    }
    if (t.b > 0 && t.c < 0) {
        return 1;
    }
    if (t.a < 0 && t.d < 0) {
        return 2;
    }
    if (t.b < 0 && t.c > 0) {
        return 3;
    }
    return 0; /* not a rotation at all (e.g. the zero matrix) - identity is
                 the least wrong answer, and is_axis_preserving() would have
                 already rejected this transform anyway. */
}

/* Where gfx_text_font()'s (x, y) origin - the FIRST glyph's cell,
 * whichever way the string is about to be drawn - must be placed so a
 * string measuring `box` draws flush inside it at `quarter` turns.
 * Turns 0 and 1 walk FORWARD from the origin, so box's own (x, y)
 * corner already IS where glyph 0 belongs - no correction, none
 * applied. */
static inline void
ui_text_glyph0_origin(const gfx_font_t* font, mu_Rect box, int quarter, int scale, int* out_x, int* out_y) {
    *out_x = box.x;
    *out_y = box.y;
    /* Turns 2 and 3 walk BACKWARD, so glyph 0 starts ONE GLYPH CELL in
     * from the box's FAR edge. That step is `font->cell_w` - NOT
     * cell_h, NOT advance - in BOTH turn 2 (X) and turn 3 (Y): rotation
     * always maps a glyph's COLUMN axis (span cell_w) onto whichever
     * screen axis the string walks along, so glyph 0's footprint is
     * cell_w in every turn, never cell_h. Verified three ways
     * (symbolically, via ui_transform_rect(), pixel-for-pixel against a
     * non-square font) - see suite_ui_transform.c. */
    if (quarter == 2) {
        *out_x = box.x + box.w - font->cell_w * scale;
    } else if (quarter == 3) {
        *out_y = box.y + box.h - font->cell_w * scale;
    }
}

/* Whether `t` maps axis-aligned rects to axis-aligned rects - the
 * condition under which gfx_fill_rect(), gfx_set_clip() and the dirty
 * tracker can be trusted, and gfx_text_turned() gets a turn to give.
 * Each logical axis maps onto ONE physical axis, not a mix - either the
 * x/y columns keep to their axis (b == c == 0, unrotated, scaled) or
 * swap cleanly (a == d == 0, a 90/270 turn). A shear or arbitrary
 * rotation has a nonzero entry in both columns. `!= 0` rules out a zero
 * scale, not invertible. */
static inline bool
ui_transform_is_axis_preserving(ui_transform_t t) {
    if (t.b == 0 && t.c == 0) {
        return t.a != 0 && t.d != 0;
    }
    if (t.a == 0 && t.d == 0) {
        return t.b != 0 && t.c != 0;
    }
    return false;
}
