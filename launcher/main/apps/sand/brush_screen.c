#include "brush_screen.h"

#include "ui/ui.h"

/* Space between the three stacked panels - and between a panel's own
 * caption row and the control below it, which reuses the same value so the
 * screen reads as one rhythm rather than two unrelated gaps. */
#define BRUSH_SCREEN_GAP   12
#define BRUSH_SCREEN_PAD   8
#define BRUSH_SCREEN_INNER 8

#define CAPTION_ROW_H 20

/* Square, and the header's content height (see HEADER_H below) exactly - the
 * swatch fills the row top to bottom rather than being a smaller square
 * floating inside it. */
#define SWATCH_SIDE 80

/* 56, not the 44px tap-target floor exactly: a bit of headroom so the "i"
 * glyph Phase 5b/6 draws into it isn't pressed against the button's own
 * edge. */
#define INFO_BTN_SIDE 56

/* Exactly "06 PX" at scale 2 on the 8px-wide UI font: 5 glyphs * 8px *
 * 2 = 80. Monospace, so this is exact, not a guess - a narrower value
 * clipped the trailing "X" (measured with ui_measure_text() while wiring
 * the screen's drawing code). */
#define SIZE_VALUE_W 80

#define SEG_GAP 8

/* Panel heights, tuned so the whole stack fits the tighter of the two real
 * canvases (448x368 landscape, 336px of content height after margins) with
 * every tap target still >= 44px - see brush_screen_layout()'s own
 * comment for the sum this must clear. */
#define HEADER_H 96
#define MODE_H   112
#define SIZE_H   96

#define BLOCK_H (HEADER_H + BRUSH_SCREEN_GAP + MODE_H + BRUSH_SCREEN_GAP + SIZE_H)

/* Both real canvases this screen is ever drawn at - 368x448 portrait and its
 * quarter turn - must have room for BLOCK_H under UI_MARGIN top and bottom;
 * see palette.h's PALETTE_FITS for the same reasoning applied to the other
 * panel in this app. 368 is the tighter height of the two, so it is the one
 * that actually constrains BLOCK_H above. */
_Static_assert(BLOCK_H <= 368 - 2 * UI_MARGIN,
               "the brush screen's three panels are taller than the "
               "shorter of the two real canvases (448x368) allows");

/* Lays `n` equal-width segments across `row`, with `gap` between
 * neighbours, any leftover pixel split evenly on the outside - the same
 * centring shape palette.c's row_left_x() uses, so "equal widths, equal
 * gaps" holds exactly rather than only up to a rounding pixel on one end. */
static void lay_out_segments(mu_Rect row, int gap, int n, mu_Rect *out)
{
    const int seg_w   = (row.w - (n - 1) * gap) / n;
    const int used    = n * seg_w + (n - 1) * gap;
    const int start_x = row.x + (row.w - used) / 2;

    for (int i = 0; i < n; i++) {
        out[i] = (mu_Rect){ start_x + i * (seg_w + gap), row.y, seg_w, row.h };
    }
}

static void layout_header(mu_Rect panel, brush_screen_layout_t *out)
{
    const int content_h = panel.h - 2 * BRUSH_SCREEN_PAD;

    out->swatch = (mu_Rect){ panel.x + BRUSH_SCREEN_PAD, panel.y + BRUSH_SCREEN_PAD,
                             SWATCH_SIDE, SWATCH_SIDE };

    out->info_button = (mu_Rect){
        panel.x + panel.w - BRUSH_SCREEN_PAD - INFO_BTN_SIDE,
        panel.y + BRUSH_SCREEN_PAD + (content_h - INFO_BTN_SIDE) / 2,
        INFO_BTN_SIDE, INFO_BTN_SIDE,
    };

    const int text_x = out->swatch.x + SWATCH_SIDE + BRUSH_SCREEN_INNER;
    const int text_w = out->info_button.x - BRUSH_SCREEN_INNER - text_x;

    out->material_caption = (mu_Rect){ text_x, panel.y + BRUSH_SCREEN_PAD,
                                       text_w, CAPTION_ROW_H };
    out->material_name = (mu_Rect){
        text_x, out->material_caption.y + CAPTION_ROW_H + 4,
        text_w, content_h - CAPTION_ROW_H - 4,
    };
}

static void layout_mode(mu_Rect panel, brush_screen_layout_t *out)
{
    const int content_w = panel.w - 2 * BRUSH_SCREEN_PAD;
    const int content_h = panel.h - 2 * BRUSH_SCREEN_PAD;

    out->mode_caption = (mu_Rect){ panel.x + BRUSH_SCREEN_PAD, panel.y + BRUSH_SCREEN_PAD,
                                   content_w, CAPTION_ROW_H };

    const mu_Rect row = {
        panel.x + BRUSH_SCREEN_PAD,
        out->mode_caption.y + CAPTION_ROW_H + BRUSH_SCREEN_INNER,
        content_w,
        content_h - CAPTION_ROW_H - BRUSH_SCREEN_INNER,
    };
    lay_out_segments(row, SEG_GAP, BRUSH_SCREEN_SEGMENT_COUNT, out->segments);
}

static void layout_size(mu_Rect panel, brush_screen_layout_t *out)
{
    const int content_w = panel.w - 2 * BRUSH_SCREEN_PAD;
    const int content_h = panel.h - 2 * BRUSH_SCREEN_PAD;

    out->size_value = (mu_Rect){
        panel.x + panel.w - BRUSH_SCREEN_PAD - SIZE_VALUE_W,
        panel.y + BRUSH_SCREEN_PAD,
        SIZE_VALUE_W, CAPTION_ROW_H,
    };
    out->size_caption = (mu_Rect){
        panel.x + BRUSH_SCREEN_PAD, panel.y + BRUSH_SCREEN_PAD,
        content_w - SIZE_VALUE_W - BRUSH_SCREEN_INNER, CAPTION_ROW_H,
    };

    out->slider_track = (mu_Rect){
        panel.x + BRUSH_SCREEN_PAD,
        out->size_caption.y + CAPTION_ROW_H + BRUSH_SCREEN_INNER,
        content_w,
        content_h - CAPTION_ROW_H - BRUSH_SCREEN_INNER,
    };
}

void brush_screen_layout(int screen_w, int screen_h, brush_screen_layout_t *out)
{
    const int panel_x = UI_MARGIN;
    const int panel_w = screen_w - 2 * UI_MARGIN;

    /* Any slack left by BLOCK_H under the taller of the two real canvases
     * (448px, the portrait height) is split evenly above and below rather
     * than left pinned to the top - see BLOCK_H's own _Static_assert. */
    const int top_y = UI_MARGIN + (screen_h - 2 * UI_MARGIN - BLOCK_H) / 2;

    out->header_panel = (mu_Rect){ panel_x, top_y, panel_w, HEADER_H };
    out->mode_panel = (mu_Rect){
        panel_x, out->header_panel.y + HEADER_H + BRUSH_SCREEN_GAP, panel_w, MODE_H,
    };
    out->size_panel = (mu_Rect){
        panel_x, out->mode_panel.y + MODE_H + BRUSH_SCREEN_GAP, panel_w, SIZE_H,
    };

    layout_header(out->header_panel, out);
    layout_mode(out->mode_panel, out);
    layout_size(out->size_panel, out);
}
