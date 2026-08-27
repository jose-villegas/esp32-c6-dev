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
 *
 * These are not symmetric, and deliberately so. This palette is nearly black
 * by design (see ui_init()'s note on OLED pixels), so a shadow mixed toward
 * black lands within a shade or two of the window background and effectively
 * disappears - there is no room below the face colour to carve into. The lit
 * pair therefore carries the whole effect, and WHICH TWO EDGES ARE LIT is the
 * cue that reads as raised or sunken. The shadow is kept anyway because it
 * still separates one button from the next where two rows meet. */
#define UI_BEZEL_HIGHLIGHT 96
#define UI_BEZEL_SHADOW    120

/* Mix a colour toward white (t > 0) or black (t < 0), |t| out of 255.
 *
 * Plain 8-bit channels, unlike gfx_color_mix() in gfx_color.h, which has to
 * unpack the panel's byte-swapped RGB565 first. A style works in microui's
 * colour space and never sees a panel pixel; the conversion happens once, in
 * ui.c's draw_command(). Same rounded (a*(255-t) + b*t + 127)/255 mix, so the
 * ends land exactly on the input and on the target. Alpha is carried through
 * untouched - the edges of a frame are exactly as opaque as its face. */
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

/* The rectangles making up one bezelled frame, back to front.
 *
 * `sunken` swaps which pair of edges is lit, turning a raised button into a
 * pressed one. Returns how many spans were written, at most
 * UI_BEZEL_MAX_SPANS, or 0 if `max` cannot hold a whole bezel - a partial one
 * would draw a face with edges missing, which looks like a bug rather than
 * like a plainer style.
 *
 * The edge rects deliberately OVERLAP at the corners, and the shadowed pair
 * is written last, so both the top-right and bottom-left corners come out
 * dark. That is the classic mitre-free bevel: the alternative, insetting each
 * edge so nothing overlaps, leaves two bare corner pixels of face colour that
 * read as chipped at this thickness.
 *
 * Thickness is clamped so the two edges can never meet or cross in a control
 * smaller than the bezel was drawn for; if there is no room for even one
 * pixel of edge, the result is a single flat face span, which is the same
 * picture UI_BUTTON_FLAT would give without its border. */
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
