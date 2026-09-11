/*
 * brush_screen_preview - render the sand app's brush screen through the real
 * firmware drawing code on a host, at both canvas sizes, so its composition
 * can be judged without a device build or a flash. Same shape as
 * launcher/tools/boot_anim_render_host.c: real drawing code, a malloc'd
 * framebuffer, no device.
 *
 *     main/apps/sand/tools/report_brush_screen_preview.sh
 *
 * writes brush_screen_portrait.png and brush_screen_landscape.png into this
 * tool's own build/. Not built by idf.py, not part of run_tests.sh.
 *
 * EVERY RECT COMES FROM brush_screen_layout() - nothing here hardcodes a
 * position. A preview that disagrees with the screen it previews is worse
 * than no preview.
 *
 * Approximated, since this stays out of ui.c and microui.c: nothing is
 * pressed or focused; the slider draws in microui's default colours (so does
 * the real screen, making that a value to know rather than a gap); a
 * segment's icon/label sub-layout is mirrored from draw_brush_screen()'s own
 * inline arithmetic; and a flat fill stands in for the frozen sandbox.
 *
 * Landscape: gfx.c's framebuffer is fixed at 368x448 at compile time, so the
 * landscape render goes through the same quarter-turn transform ui.c applies
 * for a rotated device, and landscape_pixel() reads it back through that
 * turn's inverse - derived by mapping a rect's corner, because a raw
 * per-pixel inverse of a rect-based transform is off by one at the edge.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brush_screen.h"
#include "gfx/gfx.h"
#include "gfx/gfx_color.h"
#include "gfx/gfx_font_roles.h"
#include "gfx/icon.h"
#include "icons_sand.h"
#include "material.h"
#include "material_palette.h"
#include "sand_swatch.h"
#include "sand_ui.h"
#include "ui/ui.h"              /* UI_MARGIN, UI_SLIDER_KNOB_W only */
#include "ui/ui_slider.h"
#include "ui/ui_style.h"
#include "ui/ui_transform.h"
#include "util/screenshot.h"

/* A real brush cell, mode and radius to draw - GUNPOWDER_CELL(0) is
 * "Gunpowder", the longest name in app_sand.c's brushes[] (see
 * brush_screen.c's own comment on the name-shrink loop it forced). */
#define PREVIEW_MATERIAL  GUNPOWDER_CELL(0)
#define PREVIEW_MODE      SAND_MODE_PAINT
#define PREVIEW_RADIUS_PX ((SAND_UI_RADIUS_MIN + SAND_UI_RADIUS_MAX) / 2)

/* app_sand.c's own inline sub-layout inside draw_brush_screen() - see this
 * file's header for why these are mirrored rather than shared. */
#define SEG_PAD       8
#define SEG_LABEL_GAP 4
#define INFO_ICON_PAD 12
#define SWATCH_CELLS  8

/* microui's default_style (components/microui/src/microui.c) - see this
 * file's header for why the slider has to carry these rather than ask
 * ui.c for them. */
#define SLIDER_BASE_COLOR   0x1E1E1E /* MU_COLOR_BASE        30,30,30 */
#define SLIDER_BORDER_COLOR 0x191919 /* MU_COLOR_BORDER      25,25,25 */
#define SLIDER_FILL_COLOR   0x737373 /* MU_COLOR_BUTTONFOCUS 115,115,115 */
#define SLIDER_KNOB_COLOR   0x4B4B4B /* MU_COLOR_BUTTON      75,75,75 */

static mu_Color hex_to_mu(uint32_t rgb)
{
    return (mu_Color){ (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, 255 };
}

static gfx_color_t mu_to_gfx(mu_Color c)
{
    return gfx_rgb(((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b);
}

static void fill_rect_logical(ui_transform_t t, mu_Rect r, gfx_color_t color)
{
    const mu_Rect p = ui_transform_rect(t, r);
    gfx_fill_rect(p.x, p.y, p.w, p.h, color);
}

/* Mirrors app_sand.c's draw_brush_panel(): one flat section frame. */
static void draw_panel(ui_transform_t t, mu_Rect r, uint32_t face_rgb, uint32_t border_rgb)
{
    ui_span_t spans[UI_PANEL_MAX_SPANS];
    const int n = ui_panel_spans(r, hex_to_mu(face_rgb), hex_to_mu(border_rgb),
                                 spans, UI_PANEL_MAX_SPANS);
    for (int i = 0; i < n; i++) {
        fill_rect_logical(t, spans[i].rect, mu_to_gfx(spans[i].color));
    }
}

/* Mirrors app_sand.c's draw_brush_bezel(); `sunken` is always false here -
 * see this file's header on hit state. */
static void draw_bezel(ui_transform_t t, mu_Rect r, uint32_t face_rgb)
{
    ui_span_t spans[UI_BEZEL_MAX_SPANS];
    const int n = ui_bezel_spans(r, hex_to_mu(face_rgb), false, spans, UI_BEZEL_MAX_SPANS);
    for (int i = 0; i < n; i++) {
        fill_rect_logical(t, spans[i].rect, mu_to_gfx(spans[i].color));
    }
}

typedef struct { gfx_color_t color; } icon_fill_ctx_t;

static void icon_emit(void *ctx, int x, int y, int w, int h)
{
    gfx_fill_rect(x, y, w, h, ((const icon_fill_ctx_t *)ctx)->color);
}

/* Mirrors ui.c's ui_draw_icon(), with `box` mapped through `t` the way
 * ui.c's own draw_command() maps MU_COMMAND_ICON. */
static void draw_icon(ui_transform_t t, mu_Rect box, const icon_t *icon, uint32_t color_rgb)
{
    icon_fill_ctx_t ctx = { gfx_rgb(color_rgb) };
    ui_transform_icon_blocks(t, icon_sand_rows + icon->offset, icon->w, icon->h,
                             icon->stride, box, icon_emit, &ctx);
}

/* Mirrors app_sand.c's draw_brush_text() for the logical (x, y), then ui.c's
 * draw_command() MU_COMMAND_TEXT case for turning it into a physical glyph
 * origin - see gfx_text_font() and ui_text_glyph0_origin(). The brush
 * screen always draws UI_TEXT_PLAIN, so this is the ink pass alone, no
 * halo. */
static void draw_text(ui_transform_t t, mu_Rect r, const char *str, uint32_t color_rgb,
                      int scale, int align)
{
    const gfx_font_t *font = gfx_font_ui();
    const int tw = gfx_font_text_width(font, str, -1, scale);
    const int th = gfx_font_height(font, scale);
    const int x = (align < 0) ? r.x
                : (align == 0) ? r.x + (r.w - tw) / 2
                               : r.x + r.w - tw;
    const int y = r.y + (r.h - th) / 2;

    const int quarter = ui_transform_quarter(t);
    const mu_Rect box = ui_transform_rect(t, (mu_Rect){ x, y, tw, th });
    int mx, my;
    ui_text_glyph0_origin(font, box, quarter, scale, &mx, &my);
    gfx_text_font(mx, my, str, gfx_rgb(color_rgb), scale, quarter, font);
}

/* Mirrors app_sand.c's brush_color(): which palette entry a brush cell's
 * own swatch border/badge draws in. */
static gfx_color_t preview_brush_color(cell_t c)
{
    const gfx_color_t *palette = material_palette();
    if (cell_is_gunpowder(c)) {
        return palette[GUNPOWDER_CELL(2)];
    }
    return palette[cell_is_extended(c) ? c : CELL_MAKE(CELL_MATERIAL(c), 13)];
}

/* Mirrors app_sand.c's draw_brush_swatch(). */
static void draw_swatch(ui_transform_t t, mu_Rect r, cell_t spec)
{
    const gfx_color_t *palette = material_palette();
    for (int row = 0; row < SWATCH_CELLS; row++) {
        const int y0 = r.y + row * r.h / SWATCH_CELLS;
        const int y1 = r.y + (row + 1) * r.h / SWATCH_CELLS;
        for (int col = 0; col < SWATCH_CELLS; col++) {
            const int x0 = r.x + col * r.w / SWATCH_CELLS;
            const int x1 = r.x + (col + 1) * r.w / SWATCH_CELLS;
            const cell_t cell = sand_swatch_cell(spec, col, row, SWATCH_CELLS);
            fill_rect_logical(t, (mu_Rect){ x0, y0, x1 - x0, y1 - y0 }, palette[cell]);
        }
    }

    ui_span_t spans[UI_BEZEL_MAX_SPANS];
    const int n = ui_bezel_spans(r, hex_to_mu(gfx_color_rgb888(preview_brush_color(spec))),
                                 false, spans, UI_BEZEL_MAX_SPANS);
    for (int i = 1; i < n; i++) {
        fill_rect_logical(t, spans[i].rect, mu_to_gfx(spans[i].color));
    }
}

/* Mirrors ui.c's ui_slider_int() body, minus hit-testing (nothing is being
 * dragged in a static preview) - same span order (face, fill, border,
 * knob), see that function's own comment for why. */
static void draw_slider(ui_transform_t t, mu_Rect track, int lo, int hi, int value)
{
    ui_span_t panel[UI_PANEL_MAX_SPANS];
    const int pn = ui_panel_spans(track, hex_to_mu(SLIDER_BASE_COLOR),
                                  hex_to_mu(SLIDER_BORDER_COLOR), panel, UI_PANEL_MAX_SPANS);
    if (pn <= 0) {
        return;
    }
    fill_rect_logical(t, panel[0].rect, mu_to_gfx(panel[0].color));
    fill_rect_logical(t, ui_slider_fill_rect(track, lo, hi, value, UI_SLIDER_KNOB_W),
                      gfx_rgb(SLIDER_FILL_COLOR));
    for (int i = 1; i < pn; i++) {
        fill_rect_logical(t, panel[i].rect, mu_to_gfx(panel[i].color));
    }

    ui_span_t knob[UI_BEZEL_MAX_SPANS];
    const mu_Rect knob_rect = ui_slider_knob_rect(track, lo, hi, value, UI_SLIDER_KNOB_W);
    const int kn = ui_bezel_spans(knob_rect, hex_to_mu(SLIDER_KNOB_COLOR), false,
                                  knob, UI_BEZEL_MAX_SPANS);
    for (int i = 0; i < kn; i++) {
        fill_rect_logical(t, knob[i].rect, mu_to_gfx(knob[i].color));
    }
}

/* Everything draw_brush_screen() (app_sand.c) draws, minus the hit-testing
 * and the microui plumbing around it - see this file's header for exactly
 * what that leaves out. Every rect comes from `lay`, filled by
 * brush_screen_layout() itself - see this file's header, Task C. */
static void draw_screen(ui_transform_t t, int screen_w, int screen_h)
{
    brush_screen_layout_t lay;
    brush_screen_layout(screen_w, screen_h, &lay);

    const gfx_font_t *font = gfx_font_ui();
    const int scale = BRUSH_SCREEN_CAPTION_SCALE;

    /* Header: swatch, caption/name, info button. */
    draw_panel(t, lay.header_panel, BRUSH_PANEL_FACE_COLOR, BRUSH_PANEL_BORDER_COLOR);
    draw_swatch(t, lay.swatch, PREVIEW_MATERIAL);
    draw_text(t, lay.material_caption, BRUSH_SCREEN_MATERIAL_CAPTION,
             BRUSH_CAPTION_COLOR, scale, -1);

    const char *name = material_name(PREVIEW_MATERIAL);
    int name_scale = 4;
    for (; name_scale > 1; name_scale--) {
        if (gfx_font_text_width(font, name, -1, name_scale) <= lay.material_name.w) {
            break;
        }
    }
    draw_text(t, lay.material_name, name, BRUSH_TEXT_COLOR, name_scale, -1);

    draw_bezel(t, lay.info_button, BRUSH_SEG_UNSELECTED_COLOR);
    {
        const mu_Rect icon_r = {
            lay.info_button.x + INFO_ICON_PAD, lay.info_button.y + INFO_ICON_PAD,
            lay.info_button.w - 2 * INFO_ICON_PAD, lay.info_button.h - 2 * INFO_ICON_PAD,
        };
        draw_icon(t, icon_r, &icon_sand_table[ICON_SAND_INFO], BRUSH_TEXT_COLOR);
    }

    /* Brush mode: caption, three segments. */
    draw_panel(t, lay.mode_panel, BRUSH_PANEL_FACE_COLOR, BRUSH_PANEL_BORDER_COLOR);
    draw_text(t, lay.mode_caption, BRUSH_SCREEN_MODE_CAPTION, BRUSH_CAPTION_COLOR, scale, -1);

    for (int i = 0; i < BRUSH_SCREEN_SEGMENT_COUNT; i++) {
        const mu_Rect r = lay.segments[i];
        const char *label = brush_screen_segment_label((brush_screen_segment_t)i);
        const bool selected = ((sand_mode_t)i == PREVIEW_MODE);
        const uint32_t face = selected ? BRUSH_SEG_SELECTED_COLOR : BRUSH_SEG_UNSELECTED_COLOR;
        const uint32_t ink  = selected ? BRUSH_SEG_SELECTED_INK_COLOR : BRUSH_TEXT_COLOR;

        draw_bezel(t, r, face);

        const int label_h = gfx_font_height(font, scale);
        int icon_side = r.h - 2 * SEG_PAD - label_h - SEG_LABEL_GAP;
        const int icon_side_max = r.w - 2 * SEG_PAD;
        if (icon_side > icon_side_max) {
            icon_side = icon_side_max;
        }
        const mu_Rect icon_r = {
            r.x + (r.w - icon_side) / 2, r.y + SEG_PAD, icon_side, icon_side,
        };
        draw_icon(t, icon_r, &icon_sand_table[i], ink);

        const mu_Rect label_r = {
            r.x + SEG_PAD, icon_r.y + icon_side + SEG_LABEL_GAP,
            r.w - 2 * SEG_PAD, label_h,
        };
        draw_text(t, label_r, label, ink, scale, 0);
    }

    /* Brush size: caption/value, slider. */
    draw_panel(t, lay.size_panel, BRUSH_PANEL_FACE_COLOR, BRUSH_PANEL_BORDER_COLOR);
    draw_text(t, lay.size_caption, brush_screen_size_caption((brush_screen_segment_t)PREVIEW_MODE),
             BRUSH_CAPTION_COLOR, scale, -1);

    char size_value[8];
    snprintf(size_value, sizeof size_value, "%02u PX", (unsigned)PREVIEW_RADIUS_PX);
    draw_text(t, lay.size_value, size_value, BRUSH_TEXT_COLOR, scale, 1);

    draw_slider(t, lay.slider_track, SAND_UI_RADIUS_MIN, SAND_UI_RADIUS_MAX, PREVIEW_RADIUS_PX);
}

/* Task C: the numbers behind the composition question, read straight off
 * brush_screen_layout() rather than assumed - never a second, hand-derived
 * copy of BLOCK_H's own arithmetic. */
static void print_gaps(const char *label, int screen_w, int screen_h)
{
    brush_screen_layout_t lay;
    brush_screen_layout(screen_w, screen_h, &lay);
    const int top_gap = lay.header_panel.y - UI_MARGIN;
    const int bottom_gap = screen_h - UI_MARGIN - (lay.size_panel.y + lay.size_panel.h);
    printf("%-10s %dx%d: top gap %dpx, bottom gap %dpx\n",
          label, screen_w, screen_h, top_gap, bottom_gap);
}

static bool write_bmp(const char *path, int32_t width, int32_t height,
                      gfx_color_t (*pixel_at)(int x, int y))
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s for writing\n", path);
        return false;
    }

    uint8_t header[SCREENSHOT_BMP_HEADER_SIZE];
    screenshot_bmp_header(header, width, height);
    fwrite(header, 1, sizeof header, f);

    const int32_t stride = screenshot_bmp_row_stride(width);
    uint8_t *row = calloc(1, (size_t)stride);
    if (row == NULL) {
        fprintf(stderr, "out of memory\n");
        fclose(f);
        return false;
    }

    /* Bottom-up, per screenshot_bmp_header()'s own contract. */
    for (int y = height - 1; y >= 0; y--) {
        for (int x = 0; x < width; x++) {
            const uint32_t rgb = gfx_color_rgb888(pixel_at(x, y));
            row[x * 3 + 0] = (uint8_t)(rgb);
            row[x * 3 + 1] = (uint8_t)(rgb >> 8);
            row[x * 3 + 2] = (uint8_t)(rgb >> 16);
        }
        fwrite(row, 1, (size_t)stride, f);
    }

    free(row);
    return fclose(f) == 0;
}

static gfx_color_t portrait_pixel(int x, int y)
{
    return gfx_framebuffer()[(size_t)y * GFX_WIDTH + x];
}

/* The inverse of ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT) - see
 * this file's header ("HOW LANDSCAPE GETS DRAWN AT ALL") for the rect-corner
 * derivation this formula comes from: logical pixel (lx, ly) lands at
 * physical column (GFX_WIDTH - 1 - ly), row lx. */
static gfx_color_t landscape_pixel(int lx, int ly)
{
    return gfx_framebuffer()[(size_t)lx * GFX_WIDTH + (size_t)(GFX_WIDTH - 1 - ly)];
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output_dir>\n", argv[0]);
        return 1;
    }
    const char *out_dir = argv[1];

    if (!gfx_init()) {
        fprintf(stderr, "gfx_init failed\n");
        return 1;
    }

    print_gaps("portrait", GFX_WIDTH, GFX_HEIGHT);
    print_gaps("landscape", GFX_HEIGHT, GFX_WIDTH);

    char path[1024];

    /* Portrait: identity transform, logical == physical, dumped directly. */
    gfx_clear(gfx_rgb(0x000000));
    draw_screen(ui_transform_identity(), GFX_WIDTH, GFX_HEIGHT);
    snprintf(path, sizeof path, "%s/brush_screen_portrait.bmp", out_dir);
    if (!write_bmp(path, GFX_WIDTH, GFX_HEIGHT, portrait_pixel)) {
        return 1;
    }

    /* Landscape: drawn through the same quarter turn ui.c itself applies for
     * a physically rotated device - see this file's header. */
    gfx_clear(gfx_rgb(0x000000));
    const ui_transform_t landscape_t = ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT);
    draw_screen(landscape_t, GFX_HEIGHT, GFX_WIDTH);
    snprintf(path, sizeof path, "%s/brush_screen_landscape.bmp", out_dir);
    if (!write_bmp(path, GFX_HEIGHT, GFX_WIDTH, landscape_pixel)) {
        return 1;
    }

    return 0;
}
