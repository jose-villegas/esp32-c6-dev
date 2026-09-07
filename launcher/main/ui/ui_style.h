/*=============================================================================
 * ui_style - how a control's frame is drawn, separately from what it is.
 *
 * microui decides WHAT to draw (a button here, at this rect, in this state);
 * this decides HOW that frame looks. The split exists because the two change
 * for different reasons: adding a control is a UI question, and giving every
 * control a raised edge is a looks question that should not require touching
 * a single call site.
 *
 * The hook is microui's own. mu_Context carries a draw_frame function
 * pointer, and every frame - button, checkbox, slider, scrollbar, window
 * background - goes through it with a rect and a colour id. ui.c replaces it
 * (see styled_draw_frame there), so a style applies everywhere at once and
 * nothing in components/microui/ is patched.
 *
 * PURE GEOMETRY, SEPARATE FROM DRAWING
 *
 * The same split icons.h makes, for the same reason: ui_bezel_spans() returns
 * WHERE the rectangles go and touches nothing else, so a host test can check
 * the shape (see test/suites/suite_ui_style.c) without linking gfx.c or even
 * microui.c. Nothing here calls a microui function - mu_rect() is a real
 * function in microui.c, so the rectangles below are built as compound
 * literals rather than through it, which is what keeps this header linkable
 * on its own.
 *
 * A STYLE MUST EMIT COMMANDS, NOT PIXELS
 *
 * It would be simpler to reach for gfx_fill_rect() and paint the edges
 * directly. That would break the thing ui.c exists to provide: the repaint
 * skip works by hashing microui's command list, so anything drawn outside
 * that list is invisible to the hash and would survive on screen as a stale
 * edge after the control underneath it changed. Styles produce spans; ui.c
 * turns spans into mu_draw_rect() calls; the hash sees all of it.
 *===========================================================================*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "microui.h"

/* How button frames are drawn for the rest of this UI frame.
 *
 * Buttons only. Checkboxes, sliders and text boxes frame themselves from
 * MU_COLOR_BASE and are left alone - a 3px bezel around a 64px checkbox reads
 * as a mistake, not as a style. */
typedef enum {
    /* microui's own: a flat fill plus a one-pixel MU_COLOR_BORDER box. */
    UI_BUTTON_FLAT = 0,

    /* Lit from the top left, and inverted while a finger is on it, so a
     * press physically pushes the button in. */
    UI_BUTTON_BEZEL,
} ui_button_style_t;

/* One flat rectangle of a styled frame, in paint order - later spans draw
 * over earlier ones, which is what decides how the corners meet. */
typedef struct {
    mu_Rect  rect;
    mu_Color color;
} ui_span_t;

/*---------------------------------------------------------------------------
 * The bezel
 *-------------------------------------------------------------------------*/

/* Face, plus a lit pair of edges and a shadowed pair. */
#define UI_BEZEL_MAX_SPANS 5

/* 3px, for a 64px row (UI_ROW_HEIGHT) at a 2x glyph scale. One pixel is a
 * hairline at this size and vanishes on an OLED at arm's length; much more
 * starts eating the label's padding. */
#define UI_BEZEL_THICKNESS 3

/* How far each edge is mixed toward white and toward black, out of 255.
 * Not symmetric, deliberately: this palette is nearly black by design,
 * so a shadow mixed toward black lands within a shade or two of the
 * window background and disappears - no room below the face colour to
 * carve into. The lit pair carries the whole effect, and WHICH TWO
 * EDGES ARE LIT is the cue that reads as raised or sunken. The shadow
 * is kept anyway because it still separates one button from the next
 * where two rows meet. */
#define UI_BEZEL_HIGHLIGHT 96
#define UI_BEZEL_SHADOW    120

/* Mix a colour toward white (t > 0) or black (t < 0), |t| out of 255.
 * Plain 8-bit channels, unlike gfx_color_mix() in gfx_color.h, which
 * unpacks the panel's byte-swapped RGB565 first - a style works in
 * microui's colour space, never sees a panel pixel; the conversion
 * happens once, in ui.c's draw_command(). Same rounded
 * (a*(255-t) + b*t + 127)/255 mix, so the ends land exactly on the
 * input and target. Alpha is carried through untouched - the edges of a
 * frame are exactly as opaque as its face. */
static inline uint8_t ui_shade_channel(uint8_t v, int t)
{
    const int target = (t >= 0) ? 255 : 0;
    const int amount = (t >= 0) ? t : -t;
    return (uint8_t)((v * (255 - amount) + target * amount + 127) / 255);
}

static inline mu_Color ui_shade(mu_Color c, int t)
{
    return (mu_Color){ ui_shade_channel(c.r, t),
                       ui_shade_channel(c.g, t),
                       ui_shade_channel(c.b, t),
                       c.a };
}

/* The rects making one bezelled frame, back to front. `sunken` swaps
 * which edge pair is lit, raised into pressed. Returns spans written, or
 * 0 if `max` can't hold a bezel - a partial one looks like a bug. Edge
 * rects OVERLAP at corners, shadowed pair last, so both dark corners
 * come out right - the classic mitre-free bevel; insetting leaves bare
 * corner pixels that read as chipped. Thickness is clamped so edges
 * never cross; with no room, the result is one flat face span, same as
 * UI_BUTTON_FLAT. */
static inline int ui_bezel_spans(mu_Rect r, mu_Color face, bool sunken,
                                 ui_span_t *out, int max)
{
    if (max < UI_BEZEL_MAX_SPANS || r.w <= 0 || r.h <= 0) {
        return 0;
    }

    out[0] = (ui_span_t){ r, face };

    /* Leave at least one pixel of face visible between the two edges. */
    const int room = (mu_min(r.w, r.h) - 1) / 2;
    const int t    = mu_min(UI_BEZEL_THICKNESS, room);
    if (t < 1) {
        return 1;
    }

    const mu_Color lit    = ui_shade(face,  UI_BEZEL_HIGHLIGHT);
    const mu_Color shadow = ui_shade(face, -UI_BEZEL_SHADOW);
    const mu_Color top_left     = sunken ? shadow : lit;
    const mu_Color bottom_right = sunken ? lit    : shadow;

    out[1] = (ui_span_t){ (mu_Rect){ r.x, r.y, r.w, t }, top_left };
    out[2] = (ui_span_t){ (mu_Rect){ r.x, r.y, t, r.h }, top_left };
    out[3] = (ui_span_t){ (mu_Rect){ r.x, r.y + r.h - t, r.w, t }, bottom_right };
    out[4] = (ui_span_t){ (mu_Rect){ r.x + r.w - t, r.y, t, r.h }, bottom_right };
    return UI_BEZEL_MAX_SPANS;
}

/*---------------------------------------------------------------------------
 * Text
 *
 * A second style, sibling to the bezel above, for exactly the same reason:
 * WHAT to draw (a string, at this position, in this colour) is microui's
 * question, and HOW it reads against whatever it sits on is a looks question
 * that should not require touching a call site. See suite_ui_style.c for the
 * geometry checks.
 *-------------------------------------------------------------------------*/

typedef enum {
    UI_TEXT_PLAIN = 0,   /* one pass, exactly as today */
    UI_TEXT_OUTLINED,    /* a halo at all eight neighbouring offsets */
    UI_TEXT_SHADOWED,    /* a single offset halo, down-right */
} ui_text_style_t;

/* One drawing pass of a styled string, in paint order. */
typedef struct { int dx, dy; bool ink; } ui_text_pass_t;

/* PLAIN is 1, SHADOWED is 2, OUTLINED is 9 (8 halo offsets + the ink) - the
 * largest of the three sizes the buffer for all of them. */
#define UI_TEXT_MAX_PASSES 9

/* The passes making up one styled string, back to front. Returns how
 * many were written, or 0 if `max` cannot hold them all - the same
 * all-or-nothing rule ui_bezel_spans() follows, for the same reason: a
 * half-drawn outline looks like a bug. THE INK PASS MUST ALWAYS BE
 * LAST. Every other pass paints the halo, which has to sit *behind* the
 * glyph it is haloing - draw the halo first and the glyph on top, or
 * the glyph disappears under its own halo. */
static inline int ui_text_passes(ui_text_style_t style, ui_text_pass_t *out,
                                 int max)
{
    switch (style) {
    case UI_TEXT_PLAIN:
        if (max < 1) {
            return 0;
        }
        out[0] = (ui_text_pass_t){ 0, 0, true };
        return 1;

    case UI_TEXT_SHADOWED:
        if (max < 2) {
            return 0;
        }
        out[0] = (ui_text_pass_t){ 1, 1, false };
        out[1] = (ui_text_pass_t){ 0, 0, true };
        return 2;

    case UI_TEXT_OUTLINED: {
        if (max < UI_TEXT_MAX_PASSES) {
            return 0;
        }
        /* One pixel each way, in screen space. This mirrors app_sand.c's
         * palette label outline (see draw_palette() there) exactly,
         * including the order - that code is the precedent this style
         * generalises, and it is worth staying a recognisably identical
         * list rather than an equivalent but different-looking one. All
         * eight, not just the four cardinals: at GFX_GLYPH_SCALE 2 each
         * font pixel is a 2x2 block, so skipping the diagonals leaves a
         * notch at every block corner rather than a clean edge. */
        static const int offsets[8][2] = {
            { -1, -1 }, { 0, -1 }, { 1, -1 },
            { -1,  0 },            { 1,  0 },
            { -1,  1 }, { 0,  1 }, { 1,  1 },
        };
        for (int i = 0; i < 8; i++) {
            out[i] = (ui_text_pass_t){ offsets[i][0], offsets[i][1], false };
        }
        out[8] = (ui_text_pass_t){ 0, 0, true };
        return UI_TEXT_MAX_PASSES;
    }

    default:
        return 0;
    }
}

/* The halo colour for a given ink, derived from the ink's luminance
 * rather than fixed - see UI_BEZEL_HIGHLIGHT/SHADOW above. A fixed halo
 * fails like a fixed button highlight: it vanishes against whichever
 * ink matches it. Same bug app_sand.c's palette spawn badge hit
 * deriving its ring from the swatch face (unreadable on Snow) - fixed
 * there to a max-contrast pair since it already had one. A general halo
 * has none, so it goes to the opposite extreme via ui_shade() - a
 * partial mix can still wash out. */
static inline mu_Color ui_text_halo(mu_Color ink)
{
    /* Same weights as a standard perceptual luma (~0.30/0.59/0.11 scaled to
     * whole numbers as 2:5:1), just enough to tell a dark ink from a light
     * one - it does not need to be exact, only decisive. Range is
     * 0..255*8 = 0..2040; 1020 is the midpoint. */
    const int luma = ink.r * 2 + ink.g * 5 + ink.b;
    return ui_shade(ink, (luma >= 1020) ? -255 : 255);
}
