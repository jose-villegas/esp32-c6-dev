/*=============================================================================
 * ui - shared microui integration.  See ui.h for what and why.
 *
 * THE CANVAS MODEL
 *
 * A retained-mode engine knows what changed because changing it is an explicit
 * act: you mutate a node, the node marks itself dirty, and only its canvas is
 * rebuilt. Immediate mode throws that signal away by construction - the UI is
 * rebuilt from scratch every frame, so "was it modified?" has no answer.
 *
 * The signal is recoverable from the other end. microui's command list is a
 * complete description of the output, so two frames that hash the same ARE the
 * same picture. Comparing output where an engine compares intent gets to the
 * same place by a different route.
 *
 * The canvas split comes free with it. microui already groups commands by root
 * container, each with its own rect, so ONE WINDOW IS ONE CANVAS: hashed on its
 * own, repainted on its own, and marking only its own bands dirty. A live
 * readout in one window therefore does not force a static toolbar in another to
 * repaint, which is the whole point of splitting canvases in the first place.
 *
 * The one rule that has to be respected is painter's order. Windows are drawn
 * back to front, so repainting one means repainting anything above it that
 * overlaps - otherwise the repaint erases what was on top.
 *===========================================================================*/

#include "ui/ui.h"

#include <string.h>

#include "esp_log.h"

#include "gfx/gfx.h"
#include "gfx/gfx_font_roles.h"
#include "gfx/icons.h"
#include "ui/ui_pointer.h"
#include "ui/ui_slider.h"

static const char *TAG = "ui";

/* Per-canvas hash of the last painted output, indexed by microui's container
 * pool slot, which is stable for as long as a window keeps being used. */
static uint64_t canvas_hash[MU_CONTAINERPOOL_SIZE];

static mu_Context ctx;
static bool       invalidated = true;

static ui_button_style_t  button_style;
/* The style in force for the rest of this frame, and microui's own frame
 * painter, kept so UI_BUTTON_FLAT and every non-button frame stay exactly
 * what upstream draws. Captured from the context rather than
 * reimplemented here: microui's draw_frame() is static to microui.c, and
 * a hand-copied twin of it would be one more thing to keep in step
 * across a version bump. */
static void             (*base_draw_frame)(mu_Context *, mu_Rect, int);

/* The style MU_COMMAND_TEXT is drawn in - see ui_set_text_style() below for
 * why, unlike button_style, this is never reset per frame. */
static ui_text_style_t text_style;

/* The transform every command is mapped through before it is drawn, and
 * whether it is one draw_command() may actually use - see ui_set_transform()
 * below for why an invalid one is remembered rather than rejected outright. */
static ui_transform_t transform;
static bool            transform_valid;

/* See ui.h's own comment above ui_layout_generation() for what this counts
 * and, more importantly, what it is not. Bumped in the same branch of
 * ui_set_transform() below that already calls ui_invalidate() - a genuine
 * transform change is the one and only thing that increments it. */
static uint32_t layout_generation;

/* What a mu_Font actually points at - see ui_set_font()'s comment below
 * for why the scale rides inside the font rather than a separate
 * render-time setting. */
typedef struct {
    const gfx_font_t *font;
    int                scale;
} ui_font_scaled_t;

/* Every (font, scale) pair anyone has asked for, so the same pair always
 * yields the same address - see intern_font_scaled() below for why that
 * stability is load-bearing. Small and never cleared: one shell, one
 * mu_Context, realistically a handful of roles at a handful of scales
 * for the app's whole lifetime, not a per-screen or per-frame set. */
#define UI_FONT_SCALED_MAX 8
static ui_font_scaled_t font_scaled_table[UI_FONT_SCALED_MAX];
static int              font_scaled_count;

/* Returns the SAME address for the same (font, scale) every time, which is
 * what lets hash_canvas() (below) notice a scale change on its own, the
 * same way it already does for a font change - see ui_set_font()'s
 * comment. */
static const ui_font_scaled_t *intern_font_scaled(const gfx_font_t *font, int scale)
{
    for (int i = 0; i < font_scaled_count; i++) {
        if (font_scaled_table[i].font == font && font_scaled_table[i].scale == scale) {
            return &font_scaled_table[i];
        }
    }
    if (font_scaled_count < UI_FONT_SCALED_MAX) {
        font_scaled_table[font_scaled_count] = (ui_font_scaled_t){ font, scale };
        return &font_scaled_table[font_scaled_count++];
    }
    /* Full: hand back the default pair rather than recycling a slot.
     * Overwriting one retargets every mu_Font already pointing at it - slot 0
     * is the shell default - so the picture changes while the command list
     * keeps the same bytes, which is exactly what hash_canvas() cannot see.
     * Text at the wrong size is visible and recoverable; a canvas that skips
     * its repaint is neither. Raise UI_FONT_SCALED_MAX instead. */
    ESP_LOGW(TAG, "font/scale table full (%d entries) - falling back to the default",
             UI_FONT_SCALED_MAX);
    return &font_scaled_table[0];
}

/* mu_Font is NULL only before ui_init() has run - see resolve_font_scaled()
 * and measure_text_width()/height() below, which is where that matters. */
static ui_font_scaled_t resolve_font_scaled(mu_Font font)
{
    if (font) {
        return *(const ui_font_scaled_t *)font;
    }
    return (ui_font_scaled_t){ gfx_font_ui(), GFX_GLYPH_SCALE };
}

/* microui asks us for text metrics rather than measuring anything itself.
 * `font` is whatever ctx.style->font held when the widget that wants
 * metrics ran - see ui_set_font() in ui.h. Falling back rather than
 * dereferencing NULL means a widget measured before ui_init() gets a
 * sane answer instead of a crash. */
static int measure_text_width(mu_Font font, const char *str, int len)
{
    const ui_font_scaled_t fs = resolve_font_scaled(font);
    return gfx_font_text_width(fs.font, str, len, fs.scale);
}

static int measure_text_height(mu_Font font)
{
    const ui_font_scaled_t fs = resolve_font_scaled(font);
    return gfx_font_height(fs.font, fs.scale);
}

/*---------------------------------------------------------------------------
 * Styling
 *
 * Every frame microui draws - button, checkbox, slider, scrollbar, window
 * background - arrives here with a rect and a colour id. See ui_style.h for
 * what a style is and why it produces spans rather than painting.
 *
 * WHY THE PRESSED LOOK IS ON HOVER, NOT ONLY ON FOCUS
 *
 * On a mouse, hover means the pointer is near; focus means the button is
 * held. Touch has neither until contact, so hover IS contact. The pointer
 * now holds DOWN for the whole press, so focus covers most of a tap on its
 * own - but the one synthesized hover frame before DOWN lands has no focus
 * yet, so hover still has to key the sunken look too.
 *-------------------------------------------------------------------------*/

static bool is_button_frame(int colorid)
{
    return colorid == MU_COLOR_BUTTON ||
           colorid == MU_COLOR_BUTTONHOVER ||
           colorid == MU_COLOR_BUTTONFOCUS;
}

static void styled_draw_frame(mu_Context *c, mu_Rect rect, int colorid)
{
    if (button_style != UI_BUTTON_BEZEL || !is_button_frame(colorid)) {
        base_draw_frame(c, rect, colorid);
        return;
    }

    ui_span_t spans[UI_BEZEL_MAX_SPANS];
    const int n = ui_bezel_spans(rect, c->style->colors[colorid],
                                 colorid != MU_COLOR_BUTTON,
                                 spans, UI_BEZEL_MAX_SPANS);
    for (int i = 0; i < n; i++) {
        mu_draw_rect(c, spans[i].rect, spans[i].color);
    }
}

void ui_set_button_style(ui_button_style_t style)
{
    button_style = style;
}

/* WHY THIS NEEDS ui_invalidate() AND ui_set_button_style() DOES NOT: a
 * bezel is real mu_draw_rect() commands, so a style change is a content
 * change ui_end()'s hash sees. Text style applies at RENDER time inside
 * draw_command() - the command list is byte-identical either way, so
 * hash_canvas() can't see it and the repaint is skipped, leaving OLD
 * pixels under the new intent. Do NOT delete this call "for consistency"
 * - the two are not symmetric, and deleting it reintroduces the bug it
 * prevents. */
void ui_set_text_style(ui_text_style_t style)
{
    if (style != text_style) {
        text_style = style;
        ui_invalidate();
    }
}

/* WHY THIS NEEDS NO ui_invalidate(), UNLIKE ui_set_text_style() ABOVE:
 * mu_Font rides inside every mu_TextCommand, so a font (or scale) change
 * is different bytes and hash_canvas() sees it unaided. THAT property is
 * exactly why the scale lives here too rather than a render-time global -
 * a global would need invalidating on every size change, which a two-size
 * screen hits every frame, permanently defeating the repaint skip. */
void ui_set_font_scaled(const gfx_font_t *font, int scale)
{
    if (scale < 1) {
        scale = 1;
    }
    ctx.style->font = (mu_Font)intern_font_scaled(font ? font : gfx_font_ui(), scale);
}

void ui_set_font(const gfx_font_t *font)
{
    ui_set_font_scaled(font, GFX_GLYPH_SCALE);
}

static bool transforms_equal(ui_transform_t a, ui_transform_t b)
{
    return a.a == b.a && a.b == b.b && a.c == b.c &&
           a.d == b.d && a.tx == b.tx && a.ty == b.ty;
}

/* See ui.h for why a transform change must call ui_invalidate(). WHY AN
 * INVALID TRANSFORM IS REMEMBERED RATHER THAN REJECTED. ui_transform_t can
 * express more than this renderer can draw - see ui_transform.h. Logging at
 * set time, not render time, keeps this to one log line, not one per frame. */
void ui_set_transform(ui_transform_t t)
{
    if (transforms_equal(t, transform)) {
        return;
    }
    transform       = t;
    transform_valid = ui_transform_is_axis_preserving(t);
    /* Past transforms_equal()'s early return, so this IS a genuine change -
     * see ui_layout_generation()'s comment in ui.h for what counts as one
     * and why this is the only place that gets to bump it. */
    layout_generation++;
    if (!transform_valid) {
        ESP_LOGE(TAG, "ui_set_transform: transform is not a rotation by a "
                 "multiple of 90 degrees, translation or scale - this "
                 "renderer cannot draw it, so identity will be used until a "
                 "valid transform is set");
    }
    ui_invalidate();
}

/* What draw_command(), feed_input() and ui_width()/ui_height() actually use:
 * the transform in force, or identity for as long as it fails
 * ui_transform_is_axis_preserving() - see ui_set_transform() above. */
static ui_transform_t effective_transform(void)
{
    return transform_valid ? transform : ui_transform_identity();
}

mu_Context *ui_context(void)
{
    return &ctx;
}

uint32_t ui_layout_generation(void)
{
    return layout_generation;
}

void ui_invalidate(void)
{
    invalidated = true;
}

void ui_init(void)
{
    mu_init(&ctx);
    ctx.text_width  = measure_text_width;
    ctx.text_height = measure_text_height;
    font_scaled_count = 0;
    ui_set_font(gfx_font_ui());

    base_draw_frame = ctx.draw_frame;
    ctx.draw_frame  = styled_draw_frame;
    button_style    = UI_BUTTON_FLAT;
    text_style      = UI_TEXT_PLAIN;
    transform       = ui_transform_identity();
    transform_valid = true;
    /* Explicit, not left to a zeroed static's implicit value - see
     * ui_layout_generation()'s comment in ui.h. 0 is simply the first value
     * a monotonic counter can have; nothing reads meaning into it beyond
     * "no genuine transform change has happened yet this run". */
    layout_generation = 0;

    /* Palette. Deliberately dark: this is an OLED, so black pixels are off
     * pixels - it costs less power and looks better than a grey chrome. */
    ctx.style->colors[MU_COLOR_WINDOWBG]    = (mu_Color){ 0x0A, 0x0C, 0x14, 255 };
    ctx.style->colors[MU_COLOR_TEXT]        = (mu_Color){ 0xE6, 0xEA, 0xF2, 255 };
    ctx.style->colors[MU_COLOR_BUTTON]      = (mu_Color){ 0x16, 0x1A, 0x28, 255 };
    ctx.style->colors[MU_COLOR_BUTTONHOVER] = (mu_Color){ 0x23, 0x2A, 0x40, 255 };
    ctx.style->colors[MU_COLOR_BUTTONFOCUS] = (mu_Color){ 0x3D, 0xDC, 0x97, 255 };
    ctx.style->padding      = 12;
    ctx.style->spacing      = UI_ROW_GAP;
    ctx.style->indent       = 0;
    ctx.style->title_height = UI_TITLE_HEIGHT;

    memset(canvas_hash, 0, sizeof(canvas_hash));
    invalidated = true;
}

/*---------------------------------------------------------------------------
 * Touch to mouse
 *
 * This is the one place where touch and microui genuinely disagree, so it is
 * worth spelling out. mu_update_control() only establishes hover on a frame
 * where the button is NOT held:
 *
 *     if (mouseover && !ctx->mouse_down) { ctx->hover = id; }
 *     if (ctx->hover == id) { if (ctx->mouse_pressed) { set_focus(id); } }
 *
 * and a control only submits once it has focus. That encodes the mouse
 * sequence "point at it, then click": hover on one frame, press on the next.
 *
 * A touchscreen has no such sequence - the pointer does not exist until a
 * finger is already down. Sending move and press together means hover is never
 * set, focus is never taken, and the button never fires.
 *
 * So we synthesise the missing frame: on the press, deliver only the position
 * and hold the button-down for the following frame. That costs one frame of
 * latency, imperceptible even at 25 fps, and makes a tap register every time.
 *
 * TOUCH ARRIVES IN PHYSICAL COORDINATES, MICROUI WANTS LOGICAL ONES
 *
 * `input->x/y` are where the finger actually is on the glass - physical
 * panel coordinates. Every control microui knows about, though, was laid out
 * in LOGICAL coordinates and drawn through the forward transform (see
 * draw_command() below), so a tap has to go through this function's inverse
 * before it means anything to mu_input_mouse*() - otherwise, under any
 * transform but identity, a control would be hit where it was laid out
 * rather than where it now visibly is. This is the one place touch enters
 * microui, which is exactly why it is also the one place this mapping needs
 * to happen. */
static ui_pointer_t pointer;

/* Also where ui_pointer_step()'s off-screen park point (-1, -1) gets mapped:
 * under a translating transform, logical "off-screen" is not necessarily
 * (-1, -1) either, so the park needs the same inverse as a real touch to
 * stay outside whatever the logical canvas currently is. */
static void to_logical(int x, int y, int *lx, int *ly)
{
    ui_transform_t inv;
    if (!ui_transform_invert(effective_transform(), &inv)) {
        /* Unreachable in practice: effective_transform() is always identity
         * or something ui_transform_is_axis_preserving() accepted, and
         * every transform that passes that check has a nonzero determinant,
         * hence an inverse. Fall back to an unmapped point rather than
         * garbage if that invariant is ever broken. */
        *lx = x;
        *ly = y;
        return;
    }
    ui_transform_point(inv, x, y, lx, ly);
}

/* One ui_pointer_t event, mapped to logical and replayed into microui. The
 * policy itself - hover, then hold down until the real release, park
 * off-screen when idle - lives in ui_pointer_step(); this only translates
 * and dispatches what it returns. */
static void replay_pointer_event(const ui_pointer_event_t *e)
{
    int lx, ly;
    to_logical(e->x, e->y, &lx, &ly);

    switch (e->kind) {
    case UI_POINTER_MOVE:
        mu_input_mousemove(&ctx, lx, ly);
        break;
    case UI_POINTER_DOWN:
        mu_input_mousedown(&ctx, lx, ly, MU_MOUSE_LEFT);
        break;
    case UI_POINTER_UP:
        mu_input_mouseup(&ctx, lx, ly, MU_MOUSE_LEFT);
        break;
    }
}

static void feed_input(const input_t *input)
{
    ui_pointer_event_t events[UI_POINTER_MAX_EVENTS];
    const int n = ui_pointer_step(&pointer, input, events, UI_POINTER_MAX_EVENTS);
    for (int i = 0; i < n; i++) {
        replay_pointer_event(&events[i]);
    }
}

void ui_begin(const input_t *input)
{
    /* Reset before the caller can state its own - see ui.h on why style does
     * not persist across frames. */
    button_style = UI_BUTTON_FLAT;
    feed_input(input);
    mu_begin(&ctx);
}

/* See ui.h: the physical viewport mapped through the inverse transform. Both
 * go through one shared computation since a rect's width and height are just
 * as entangled by a quarter turn as its x and y are - deriving them
 * separately would mean inverting the transform twice for one answer. */
static mu_Rect logical_viewport(void)
{
    ui_transform_t inv;
    if (!ui_transform_invert(effective_transform(), &inv)) {
        /* See to_logical()'s identical fallback above: unreachable while
         * effective_transform() only ever returns identity or a transform
         * that already passed ui_transform_is_axis_preserving(), both of
         * which are invertible by construction. */
        return (mu_Rect){ 0, 0, GFX_WIDTH, GFX_HEIGHT };
    }
    return ui_transform_rect(inv, (mu_Rect){ 0, 0, GFX_WIDTH, GFX_HEIGHT });
}

int ui_width(void)
{
    return logical_viewport().w;
}

int ui_height(void)
{
    return logical_viewport().h;
}

int ui_measure_text(const char *str)
{
    const ui_font_scaled_t fs = resolve_font_scaled(ctx.style->font);
    return gfx_font_text_width(fs.font, str, -1, fs.scale);
}

void ui_draw_bitmap(mu_Context *c, mu_Rect r, const uint16_t *bitmap, mu_Color color)
{
    icon_rect_t blocks[UI_DRAW_BITMAP_MAX_BLOCKS];
    const int n = icon_bitmap_blocks(bitmap, r.w, r.h, blocks, UI_DRAW_BITMAP_MAX_BLOCKS);

    for (int i = 0; i < n; i++) {
        mu_draw_rect(c, mu_rect(r.x + blocks[i].x, r.y + blocks[i].y,
                                blocks[i].w, blocks[i].h), color);
    }
}

/* See ui.h. `value`'s own address (not what it points to, same idiom
 * mu_slider_ex() uses) gives each call site a stable id with no string
 * needed. MU_OPT_HOLDFOCUS keeps a drag updating once it leaves the knob. */
bool ui_slider_int(mu_Context *c, int *value, int lo, int hi, int step)
{
    const mu_Id id = mu_get_id(c, &value, sizeof(value));
    const mu_Rect track = mu_layout_next(c);
    mu_update_control(c, id, track, MU_OPT_HOLDFOCUS);

    int v = *value;
    if (c->focus == id && (c->mouse_down | c->mouse_pressed) == MU_MOUSE_LEFT) {
        v = ui_slider_value_at_x(track, lo, hi, UI_SLIDER_KNOB_W, step, c->mouse_pos.x);
    }
    v = mu_clamp(v, lo, hi);
    const bool changed = (v != *value);
    *value = v;

    ui_span_t panel[UI_PANEL_MAX_SPANS];
    const int pn = ui_panel_spans(track, c->style->colors[MU_COLOR_BASE],
                                  c->style->colors[MU_COLOR_BORDER],
                                  panel, UI_PANEL_MAX_SPANS);
    if (pn > 0) {
        /* Face, then the filled portion, then the border last - so the
         * border still frames the whole track rather than the fill
         * painting over it where the two overlap. */
        mu_draw_rect(c, panel[0].rect, panel[0].color);
        mu_draw_rect(c, ui_slider_fill_rect(track, lo, hi, v, UI_SLIDER_KNOB_W),
                     c->style->colors[MU_COLOR_BUTTONFOCUS]);
        for (int i = 1; i < pn; i++) {
            mu_draw_rect(c, panel[i].rect, panel[i].color);
        }

        ui_span_t knob[UI_BEZEL_MAX_SPANS];
        const mu_Rect knob_rect = ui_slider_knob_rect(track, lo, hi, v, UI_SLIDER_KNOB_W);
        const int kn = ui_bezel_spans(knob_rect, c->style->colors[MU_COLOR_BUTTON],
                                      false, knob, UI_BEZEL_MAX_SPANS);
        for (int i = 0; i < kn; i++) {
            mu_draw_rect(c, knob[i].rect, knob[i].color);
        }
    }

    return changed;
}

/* See ui.h for the full argument. Short version: mu_begin_window_ex()
 * only seeds cnt->rect the FIRST time a title is opened, remembering it
 * forever after - correct for a desktop window manager, wrong here,
 * where a rect must track ui_width()/ui_height() every frame. So this
 * always passes the current logical canvas, then checks what the
 * container got. A stale rect from an earlier orientation is
 * force-corrected here, and ui_invalidate() runs so THIS frame repaints
 * with it, not next frame. */
int ui_begin_screen(mu_Context *ctx, const char *title, int opt)
{
    const mu_Rect r = mu_rect(0, 0, ui_width(), ui_height());
    const int open = mu_begin_window_ex(ctx, title, r, opt);

    if (open) {
        mu_Container *cnt = mu_get_current_container(ctx);
        if (cnt->rect.x != r.x || cnt->rect.y != r.y ||
            cnt->rect.w != r.w || cnt->rect.h != r.h) {
            cnt->rect = r;
            ui_invalidate();
        }
    }

    return open;
}

/*---------------------------------------------------------------------------
 * Painting
 *
 * Every command's geometry is mapped through the transform in force before
 * it reaches gfx - see ui_transform.h for what that buys, and ui_set_transform()
 * above for why an invalid one renders as identity rather than being rejected
 * at the point it was set.
 *-------------------------------------------------------------------------*/

static void draw_command(const mu_Command *cmd)
{
    const ui_transform_t t = effective_transform();

    switch (cmd->type) {

    case MU_COMMAND_RECT: {
        const mu_Color c = cmd->rect.color;
        /* Fully transparent rects are microui's way of drawing nothing; we
         * have no blending, so skip them rather than paint black. */
        if (c.a == 0) {
            break;
        }
        const mu_Rect r = ui_transform_rect(t, cmd->rect.rect);
        gfx_fill_rect(r.x, r.y, r.w, r.h,
                      gfx_rgb(((uint32_t)c.r << 16) |
                              ((uint32_t)c.g << 8)  | c.b));
        break;
    }

    case MU_COMMAND_TEXT: {
        const mu_Color ink  = cmd->text.color;
        const mu_Color halo = ui_text_halo(ink);
        const ui_font_scaled_t fs = resolve_font_scaled(cmd->text.font);
        const gfx_font_t *font = fs.font;
        const int scale = fs.scale;
        const int quarter = ui_transform_quarter(t);

        /* WHY THE WHOLE STRING'S BOX IS MAPPED, NOT ITS ORIGIN: every
         * other command maps its rect through ui_transform_rect(), proven
         * exact under any quarter turn. This used to map only the origin
         * and walk per-glyph steps from there - a point doesn't commute
         * with "walk N glyphs, take the far edge", so at quarter 1/3 the
         * string landed a glyph cell off, at quarter 2 off both axes
         * (rotated text drifting off-centre). Now measures the LOGICAL
         * box - same one used to size it - and maps that, like the
         * others. */
        const int tw = gfx_font_text_width(font, cmd->text.str, -1, scale);
        const int th = gfx_font_height(font, scale);
        const mu_Rect box = ui_transform_rect(
            t, (mu_Rect){ cmd->text.pos.x, cmd->text.pos.y, tw, th });

        /* gfx_text_font()'s (x, y) is the FIRST GLYPH's cell, not a
         * corner of the box - see ui_text_glyph0_origin()'s own comment
         * (ui_transform.h) for the full derivation. Extracted there, not
         * kept inline, so it's testable against a synthetic proportional
         * font on a host - a port of app_sand/palette.c's
         * palette_label_origin() solving the same problem, ported rather
         * than called directly because ui/ sits below apps/, so pulling
         * in apps/sand/ would be a backwards layering dependency. */
        int mx, my;
        ui_text_glyph0_origin(font, box, quarter, scale, &mx, &my);

        ui_text_pass_t passes[UI_TEXT_MAX_PASSES];
        const int n = ui_text_passes(text_style, passes, UI_TEXT_MAX_PASSES);
        for (int i = 0; i < n; i++) {
            const mu_Color c = passes[i].ink ? ink : halo;
            const gfx_color_t color = gfx_rgb(((uint32_t)c.r << 16) |
                                              ((uint32_t)c.g << 8)  | c.b);

            /* THE HALO OFFSET IS ADDED AFTER THE MAPPING, NOT BEFORE.
             * passes[i].dx/dy is a SCREEN-SPACE offset - see ui_style.h's
             * OUTLINED/SHADOWED passes, which sit a halo a fixed pixel
             * count from the glyph on the panel. (mx, my) already IS a
             * screen position. Transforming (dx, dy) itself would instead
             * rotate the halo with the glyph: a shadow meant to fall
             * down-and-right on screen would fall down-and-right in
             * LOGICAL space instead - a different physical direction once
             * turn is nonzero. */
            gfx_text_font(mx + passes[i].dx, my + passes[i].dy, cmd->text.str,
                          color, scale, quarter, font);
        }
        break;
    }

    case MU_COMMAND_ICON: {
        /* microui's icons are close/check/collapsed/expanded. MU_ICON_CHECK
         * is real artwork (icons.h's icon_check()) because two callers need
         * it: the diagnostics app's mu_checkbox() toggles, and
         * app_sand.c's palette spawn badge. The other three stay a small
         * centred-square placeholder - a deliberate gap, not an oversight,
         * because nothing in this shell closes a window or collapses a
         * tree yet to ask for them. */
        const mu_Color c = cmd->icon.color;
        const mu_Rect r = ui_transform_rect(t, cmd->icon.rect);
        const gfx_color_t color = gfx_rgb(((uint32_t)c.r << 16) |
                                          ((uint32_t)c.g << 8)  | c.b);
        if (cmd->icon.id == MU_ICON_CHECK) {
            icon_check(r.x, r.y, r.w, r.h, color);
        } else {
            gfx_fill_rect(r.x + r.w / 3, r.y + r.h / 3, r.w / 3, r.h / 3,
                          color);
        }
        break;
    }

    case MU_COMMAND_CLIP: {
        const mu_Rect r = ui_transform_rect(t, cmd->clip.rect);
        gfx_set_clip(r.x, r.y, r.w, r.h);
        break;
    }

    default:
        break;
    }
}

/* One canvas's commands.
 *
 * Walked directly rather than through mu_next_command(), which follows the
 * jump chain across every container - the entire point here is to paint one
 * container and leave the others alone. */
static void paint_canvas(const mu_Container *cnt)
{
    const char *p   = (const char *)cnt->head + cnt->head->base.size;
    const char *end = (const char *)cnt->tail;

    while (p < end) {
        const mu_Command *cmd = (const mu_Command *)p;
        if (cmd->base.size <= 0) {
            break;      /* corrupt list: stop rather than spin */
        }
        draw_command(cmd);
        p += cmd->base.size;
    }

    gfx_clear_clip();
}

/* FNV-1a. Cheap, and only ever run over the few hundred bytes a canvas's
 * commands occupy - the buffer is 8 KiB but almost none of it is used. */
static uint64_t hash_canvas(const mu_Container *cnt)
{
    const unsigned char *p   = (const unsigned char *)cnt->head + cnt->head->base.size;
    const unsigned char *end = (const unsigned char *)cnt->tail;

    uint64_t h = 1469598103934665603ull;
    while (p < end) {
        h ^= *p++;
        h *= 1099511628211ull;
    }
    return h;
}

static bool rects_overlap(mu_Rect a, mu_Rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

/* cnt->rect is LOGICAL - ui_begin_screen() seeds it from
 * ui_width()/ui_height() - but every use below needs the PHYSICAL
 * footprint, same as draw_command() gets via ui_transform_rect(). Under
 * an odd quarter (GFX_WIDTH != GFX_HEIGHT) the rects differ in shape: an
 * unrotated (0,0,448,368) clipped onto a 368x448 framebuffer covers only
 * 368 of 448 rows, leaving 80 rows out of the background clear. Reported
 * on hardware as previous-frame pieces stuck after rotating Portrait to
 * Landscape. */
static mu_Rect canvas_physical_rect(const mu_Container *cnt)
{
    return ui_transform_rect(effective_transform(), cnt->rect);
}

/* Which canvases changed. Also repaint one whose bands are already dirty:
 * something has drawn underneath it this frame, so its pixels are gone
 * however unchanged its own description is. */
static void mark_changed_canvases(int n, bool *repaint)
{
    for (int i = 0; i < n && i < MU_ROOTLIST_SIZE; i++) {
        const mu_Container *cnt = ctx.root_list.items[i];
        const int slot = (int)(cnt - ctx.containers);
        const uint64_t h = hash_canvas(cnt);
        const mu_Rect phys = canvas_physical_rect(cnt);

        if (invalidated ||
            slot < 0 || slot >= MU_CONTAINERPOOL_SIZE ||
            h != canvas_hash[slot] ||
            gfx_region_dirty(phys.x, phys.y, phys.w, phys.h)) {
            repaint[i] = true;
        }
    }
}

/* Painter's order: repainting a canvas erases whatever was drawn on top of
 * it, so everything above it that overlaps has to go again too. root_list is
 * sorted back to front by mu_end(). */
static void propagate_repaint_over_overlaps(int n, bool *repaint)
{
    for (int i = 0; i < n && i < MU_ROOTLIST_SIZE; i++) {
        if (!repaint[i]) {
            continue;
        }
        for (int j = i + 1; j < n && j < MU_ROOTLIST_SIZE; j++) {
            if (!repaint[j] &&
                rects_overlap(canvas_physical_rect(ctx.root_list.items[i]),
                              canvas_physical_rect(ctx.root_list.items[j]))) {
                repaint[j] = true;
            }
        }
    }
}

static bool repaint_marked_canvases(int n, const bool *repaint,
                                    uint32_t background_rgb)
{
    bool drew = false;

    for (int i = 0; i < n && i < MU_ROOTLIST_SIZE; i++) {
        if (!repaint[i]) {
            continue;
        }
        const mu_Container *cnt = ctx.root_list.items[i];

        /* Clear only this canvas's rect, not the screen. Everything gfx draws
         * marks its own bands, so nothing else needs marking here. */
        if (background_rgb != UI_NO_BACKGROUND) {
            const mu_Rect phys = canvas_physical_rect(cnt);
            gfx_fill_rect(phys.x, phys.y, phys.w, phys.h,
                          gfx_rgb(background_rgb));
        }

        paint_canvas(cnt);

        const int slot = (int)(cnt - ctx.containers);
        if (slot >= 0 && slot < MU_CONTAINERPOOL_SIZE) {
            canvas_hash[slot] = hash_canvas(cnt);
        }
        drew = true;
    }
    return drew;
}

bool ui_end(uint32_t background_rgb)
{
    mu_end(&ctx);

    const int n = ctx.root_list.idx;
    bool repaint[MU_ROOTLIST_SIZE] = { false };

    mark_changed_canvases(n, repaint);
    propagate_repaint_over_overlaps(n, repaint);
    const bool drew = repaint_marked_canvases(n, repaint, background_rgb);

    invalidated = false;
    return drew;
}
