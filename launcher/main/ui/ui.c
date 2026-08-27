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
#include "gfx/icons.h"

static const char *TAG = "ui";

/* Per-canvas hash of the last painted output, indexed by microui's container
 * pool slot, which is stable for as long as a window keeps being used. */
static uint64_t canvas_hash[MU_CONTAINERPOOL_SIZE];

static mu_Context ctx;
static bool       invalidated = true;

/* The style in force for the rest of this frame, and microui's own frame
 * painter, kept so UI_BUTTON_FLAT and every non-button frame stay exactly what
 * upstream draws. Captured from the context rather than reimplemented here:
 * microui's draw_frame() is static to microui.c, and a hand-copied twin of it
 * would be one more thing to keep in step across a version bump. */
static ui_button_style_t  button_style;
static void             (*base_draw_frame)(mu_Context *, mu_Rect, int);

/* The style MU_COMMAND_TEXT is drawn in - see ui_set_text_style() below for
 * why, unlike button_style, this is never reset per frame. */
static ui_text_style_t text_style;

/* The transform every command is mapped through before it is drawn, and
 * whether it is one draw_command() may actually use - see ui_set_transform()
 * below for why an invalid one is remembered rather than rejected outright. */
static ui_transform_t transform;
static bool            transform_valid;

/* microui asks us for text metrics rather than measuring anything itself.
 * `font` is whatever ctx.style->font held when the widget that wants
 * metrics ran - see ui_set_font() in ui.h for how it gets there. It is
 * NULL only before ui_init() has run (mu_init() leaves it that way, and
 * ui_init() is what first sets it); fall back to gfx_default_font() rather
 * than deref a NULL, so a widget measured before ui_init() gets a sane
 * answer instead of a crash. */
static int measure_text_width(mu_Font font, const char *str, int len)
{
    const gfx_font_t *f = font ? (const gfx_font_t *)font : gfx_default_font();
    return gfx_font_text_width(f, str, len, GFX_GLYPH_SCALE);
}

static int measure_text_height(mu_Font font)
{
    const gfx_font_t *f = font ? (const gfx_font_t *)font : gfx_default_font();
    return gfx_font_height(f, GFX_GLYPH_SCALE);
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
 * On a mouse, hover means "the pointer is near" and focus means "the button is
 * held". A touchscreen has no such distinction: the pointer does not exist
 * until a finger is already on the glass, so hover IS contact. Following
 * feed_input()'s sequence below, a tap renders MU_COLOR_BUTTONHOVER for every
 * frame the finger is down and MU_COLOR_BUTTONFOCUS for the single frame the
 * press lands on. Sinking the bezel only on focus would therefore flash it for
 * one frame out of a press that lasts dozens, which reads as a glitch rather
 * than as a button going in.
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

/* WHY THIS ONE HAS TO CALL ui_invalidate() AND ui_set_button_style() DOES NOT
 *
 * ui_set_button_style() gets away with just storing a value because a bezel
 * is drawn as real mu_draw_rect() commands (see styled_draw_frame() above):
 * changing the style changes the bytes ui_end() hashes, so a style change
 * IS a content change as far as the repaint skip is concerned, and it just
 * works.
 *
 * Text has no such hook to piggyback on. mu_Context offers a draw_frame
 * function pointer to intercept, but no draw_text equivalent, so there is
 * nowhere to emit extra commands from - a text style can only be applied
 * here, at render time, inside draw_command(), reading whatever text_style
 * currently holds. That means the command list is byte-identical whether
 * text_style is UI_TEXT_PLAIN or UI_TEXT_OUTLINED; hash_canvas() cannot see
 * the difference, so mark_changed_canvases() would find no change and skip
 * the repaint, leaving the OLD style's pixels on screen under the new
 * style's (unpainted) intent.
 *
 * So the invalidation this style needs has to be done by hand, right here,
 * exactly when the value actually changes. This line looks redundant next
 * to ui_set_button_style() above it and will look like it too, to whoever
 * next reads these two functions side by side - it is not: the two styles
 * are not symmetric, because only one of them produces bytes the hash can
 * see. Deleting this call "for consistency" reintroduces the very bug it
 * exists to prevent. */
void ui_set_text_style(ui_text_style_t style)
{
    if (style != text_style) {
        text_style = style;
        ui_invalidate();
    }
}

/* WHY THIS ONE DOES NOT NEED ui_invalidate(), UNLIKE ui_set_text_style() ABOVE
 *
 * This is the third time this question has come up in this file, and the
 * first time the answer is no. The other two - ui_set_text_style() and
 * ui_set_transform() below - apply their effect at RENDER time, inside
 * draw_command(), so the command list ui_end() hashes is byte-identical
 * whether or not the setting changed; without a manual ui_invalidate() the
 * repaint would be skipped and the old look would stay on screen.
 *
 * A font change is different because mu_Font is not read only at render
 * time - it is baked into the command list itself. ctx.style->font rides
 * along inside every mu_TextCommand (see mu_draw_text() in microui.c), and
 * both metric callbacks above take it too, so a font change moves layout:
 * different metrics mean different positions and sizes for every control
 * that measured text this frame, not just different pixels for the same
 * geometry. Those are different bytes, so hash_canvas() sees the change on
 * its own and mark_changed_canvases() repaints without being told to. See
 * ui_set_text_style()'s comment above for the full argument this mirrors. */
void ui_set_font(const gfx_font_t *font)
{
    ctx.style->font = (mu_Font)(font ? font : gfx_default_font());
}

static bool transforms_equal(ui_transform_t a, ui_transform_t b)
{
    return a.a == b.a && a.b == b.b && a.c == b.c &&
           a.d == b.d && a.tx == b.tx && a.ty == b.ty;
}

/* See ui.h's comment above this declaration for why a transform change has to
 * call ui_invalidate() by hand, the same as ui_set_text_style() above it.
 *
 * WHY AN INVALID TRANSFORM IS REMEMBERED RATHER THAN REJECTED
 *
 * ui_transform_t can express more than this renderer can draw - see
 * ui_transform.h's contract comment. Rather than have this setter reject a
 * non-axis-preserving transform outright, it accepts it, logs once, right
 * here, and leaves `transform_valid` false so every render this frame and
 * every frame after - until a valid transform is set - draws with identity
 * instead (see effective_transform() below). Logging at set time rather than
 * at render time is what keeps this to ONE log line rather than one per
 * frame: draw_command() runs per command, many times a frame, and a log at
 * that rate would itself blow the frame budget it is trying to protect. */
void ui_set_transform(ui_transform_t t)
{
    if (transforms_equal(t, transform)) {
        return;
    }
    transform       = t;
    transform_valid = ui_transform_is_axis_preserving(t);
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

void ui_invalidate(void)
{
    invalidated = true;
}

void ui_init(void)
{
    mu_init(&ctx);
    ctx.text_width  = measure_text_width;
    ctx.text_height = measure_text_height;
    ctx.style->font = (mu_Font)gfx_default_font();

    base_draw_frame = ctx.draw_frame;
    ctx.draw_frame  = styled_draw_frame;
    button_style    = UI_BUTTON_FLAT;
    text_style      = UI_TEXT_PLAIN;
    transform       = ui_transform_identity();
    transform_valid = true;

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
static bool press_pending;
static int  press_x, press_y; /* physical; mapped to logical at each use */

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

static void feed_input(const input_t *input)
{
    int lx, ly;

    if (input->pressed) {
        /* Frame 1 of the tap: position only, so hover resolves. */
        press_pending = true;
        press_x = input->x;
        press_y = input->y;
        to_logical(press_x, press_y, &lx, &ly);
        mu_input_mousemove(&ctx, lx, ly);
        return;
    }

    if (press_pending) {
        /* Frame 2: now the press itself lands on a hovered control. */
        press_pending = false;
        to_logical(press_x, press_y, &lx, &ly);
        mu_input_mousemove(&ctx, lx, ly);
        mu_input_mousedown(&ctx, lx, ly, MU_MOUSE_LEFT);
        /* Release immediately. Holding is not meaningful for these controls,
         * and it keeps a lifted finger from leaving the button stuck down if
         * the release edge arrives while we are still mid-tap. */
        mu_input_mouseup(&ctx, lx, ly, MU_MOUSE_LEFT);
        return;
    }

    if (input->down) {
        to_logical(input->x, input->y, &lx, &ly);
        mu_input_mousemove(&ctx, lx, ly);
    } else {
        /* Park the pointer off-screen so nothing sits in a hover state while
         * no finger is touching. Mapped like every other point here rather
         * than passed straight through: under a translating transform, the
         * logical origin's "off-screen" neighbourhood is not necessarily
         * (-1, -1) any more, and mapping keeps this parked outside whatever
         * the logical canvas currently is. */
        to_logical(-1, -1, &lx, &ly);
        mu_input_mousemove(&ctx, lx, ly);
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
        const gfx_font_t *font = cmd->text.font ?
            (const gfx_font_t *)cmd->text.font : gfx_default_font();

        /* The command's position is mapped once; the quarter turn it maps
         * through decides which gfx entry point can even draw a rotated
         * glyph. Under identity this is quarter 0 and mx/my equal the
         * command's own position exactly (see ui_transform_rect()'s Q16.16
         * exactness note in ui_transform.h). gfx_text_font() takes the
         * quarter turn directly and draws quarter 0 exactly as gfx_text()
         * did, so calling it unconditionally is not an approximation of
         * the old two-branch behaviour - it IS that behaviour, collapsed
         * into the one entry point that already handled both cases
         * underneath. */
        int mx, my;
        ui_transform_point(t, cmd->text.pos.x, cmd->text.pos.y, &mx, &my);
        const int quarter = ui_transform_quarter(t);

        ui_text_pass_t passes[UI_TEXT_MAX_PASSES];
        const int n = ui_text_passes(text_style, passes, UI_TEXT_MAX_PASSES);
        for (int i = 0; i < n; i++) {
            const mu_Color c = passes[i].ink ? ink : halo;
            const gfx_color_t color = gfx_rgb(((uint32_t)c.r << 16) |
                                              ((uint32_t)c.g << 8)  | c.b);

            /* THE HALO OFFSET IS ADDED AFTER THE MAPPING, NOT BEFORE.
             *
             * passes[i].dx/dy is a SCREEN-SPACE offset - see ui_style.h's
             * UI_TEXT_OUTLINED/SHADOWED passes, which exist to sit a halo a
             * fixed number of pixels from the glyph on the panel, regardless
             * of where that glyph came from. (mx, my) already IS a screen
             * position, so adding the offset here is adding it in the space
             * it was designed for. Transforming (dx, dy) itself - e.g.
             * mapping cmd->text.pos + dx/dy as one point - would instead
             * rotate the halo along with the glyph's position and put it on
             * the wrong side of a turned glyph: a shadow that is meant to
             * always fall down-and-right on screen would instead fall
             * down-and-right in LOGICAL space, which is some other physical
             * direction entirely once turn is nonzero. */
            gfx_text_font(mx + passes[i].dx, my + passes[i].dy, cmd->text.str,
                          color, GFX_GLYPH_SCALE, quarter, font);
        }
        break;
    }

    case MU_COMMAND_ICON: {
        /* microui's icons are close/check/collapsed/expanded. MU_ICON_CHECK
         * is real artwork (icons.h's icon_check()) because two callers now
         * need it: the diagnostics app's two mu_checkbox() toggles, and
         * app_sand.c's palette spawn badge, which draws the same shape
         * directly rather than through a command. The other three stay a
         * small centred-square placeholder - a deliberate gap, not an
         * oversight, because nothing in this shell closes a window or
         * collapses a tree yet to ask for them. */
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

/* Which canvases changed. Also repaint one whose bands are already dirty:
 * something has drawn underneath it this frame, so its pixels are gone
 * however unchanged its own description is. */
static void mark_changed_canvases(int n, bool *repaint)
{
    for (int i = 0; i < n && i < MU_ROOTLIST_SIZE; i++) {
        const mu_Container *cnt = ctx.root_list.items[i];
        const int slot = (int)(cnt - ctx.containers);
        const uint64_t h = hash_canvas(cnt);

        if (invalidated ||
            slot < 0 || slot >= MU_CONTAINERPOOL_SIZE ||
            h != canvas_hash[slot] ||
            gfx_region_dirty(cnt->rect.x, cnt->rect.y,
                             cnt->rect.w, cnt->rect.h)) {
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
                rects_overlap(ctx.root_list.items[i]->rect,
                              ctx.root_list.items[j]->rect)) {
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
            gfx_fill_rect(cnt->rect.x, cnt->rect.y, cnt->rect.w, cnt->rect.h,
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
