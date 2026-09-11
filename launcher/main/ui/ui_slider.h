/*
 * ui_slider - pure geometry for an integer-valued slider: the knob and
 * filled-track rects for a value, and the inverse (a touch x -> a value).
 *
 * Same split ui_style.h makes, for the same reason: WHERE things go is a
 * host-testable question, HOW they get onto the framebuffer is not -
 * ui_slider_int() in ui.c turns this into mu_draw_rect() calls via
 * ui_panel_spans()/ui_bezel_spans(). Nothing here calls a microui function -
 * mu_rect() is a real function in microui.c, so rects below are compound
 * literals, which is what keeps this header linkable on its own.
 *
 * Integer arithmetic throughout, no floats: this is a "06 PX" control, not a
 * continuous one, and the rest of ui/ stays integer for the same reason.
 */
#pragma once

#include "microui.h"

/* Round a/b to the nearest integer, ties away from zero. `a` and `b` are
 * both assumed non-negative here - every call site below only ever divides
 * a pixel offset or a value span, neither of which goes negative. */
static inline int ui_slider_round_div(int a, int b)
{
    return (b > 0) ? (a + b / 2) / b : 0;
}

/* How far the knob can travel: never the track's own width, or the knob
 * would hang half outside the track at lo and hi. Clamped so a knob wider
 * than its track still fits (0 travel, rather than a negative one) and so
 * a track narrower than requested never asks for a knob bigger than itself. */
static inline int ui_slider_knob_w(mu_Rect track, int knob_w)
{
    return mu_clamp(knob_w, 0, mu_max(track.w, 0));
}

static inline int ui_slider_travel(mu_Rect track, int knob_w)
{
    return track.w - ui_slider_knob_w(track, knob_w);
}

/* The knob rect for `value` within [lo, hi] over `track`, `knob_w` wide
 * (clamped to fit, see ui_slider_knob_w()). y and h always match the
 * track - only x moves. `lo == hi` (or any range <= 0) parks the knob at
 * the track's start rather than dividing by zero. */
static inline mu_Rect ui_slider_knob_rect(mu_Rect track, int lo, int hi, int value, int knob_w)
{
    const int w      = ui_slider_knob_w(track, knob_w);
    const int travel = ui_slider_travel(track, knob_w);
    const int range  = hi - lo;
    const int v      = mu_clamp(value, lo, hi);
    const int x      = (range > 0) ? track.x + ui_slider_round_div((v - lo) * travel, range)
                                    : track.x;
    return (mu_Rect){ x, track.y, w, track.h };
}

/* Where the knob's centre sits for `value` - the point a finger is
 * actually placing when it drags. ui_slider_value_at_x() inverts THIS,
 * not the knob's left edge: map the finger to the edge instead and the
 * knob rides half its own width to the right of the thumb pushing it. */
static inline int ui_slider_knob_center_x(mu_Rect track, int lo, int hi, int value,
                                          int knob_w)
{
    const mu_Rect knob = ui_slider_knob_rect(track, lo, hi, value, knob_w);
    return knob.x + knob.w / 2;
}

/* The filled portion of the track, from its left edge to the knob's
 * centre - the part of the design already "passed" by the current value. */
static inline mu_Rect ui_slider_fill_rect(mu_Rect track, int lo, int hi, int value, int knob_w)
{
    const mu_Rect knob   = ui_slider_knob_rect(track, lo, hi, value, knob_w);
    const int     fill_w = mu_clamp(knob.x + knob.w / 2 - track.x, 0, mu_max(track.w, 0));
    return (mu_Rect){ track.x, track.y, fill_w, track.h };
}

/* Touch x within `track` -> the value it represents, quantized to `step`
 * and clamped to [lo, hi]. Out-of-range x clamps, never wraps. `step <= 0`
 * falls back to 1. The exact inverse of ui_slider_knob_center_x(), not of
 * the knob's left edge - see that function for why. */
static inline int ui_slider_value_at_x(mu_Rect track, int lo, int hi, int knob_w, int step, int x)
{
    const int range = hi - lo;
    if (range <= 0) {
        return lo;
    }
    const int s     = (step > 0) ? step : 1;
    const int travel = ui_slider_travel(track, knob_w);
    const int w      = ui_slider_knob_w(track, knob_w);
    const int off   = mu_clamp(x - track.x - w / 2, 0, mu_max(travel, 0));
    const int raw   = (travel > 0) ? lo + ui_slider_round_div(off * range, travel) : lo;
    const int steps = ui_slider_round_div((raw - lo), s);
    return mu_clamp(lo + steps * s, lo, hi);
}
