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

#include "ui.h"

#include <string.h>

#include "gfx.h"

/* Per-canvas hash of the last painted output, indexed by microui's container
 * pool slot, which is stable for as long as a window keeps being used. */
static uint64_t canvas_hash[MU_CONTAINERPOOL_SIZE];

static mu_Context ctx;
static bool       invalidated = true;

/* microui asks us for text metrics rather than measuring anything itself. */
static int measure_text_width(mu_Font font, const char *str, int len)
{
    (void)font;
    return gfx_text_width(str, len);
}

static int measure_text_height(mu_Font font)
{
    (void)font;
    return gfx_text_height();
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
 *-------------------------------------------------------------------------*/

static bool press_pending;
static int  press_x, press_y;

static void feed_input(const input_t *input)
{
    if (input->pressed) {
        /* Frame 1 of the tap: position only, so hover resolves. */
        press_pending = true;
        press_x = input->x;
        press_y = input->y;
        mu_input_mousemove(&ctx, press_x, press_y);
        return;
    }

    if (press_pending) {
        /* Frame 2: now the press itself lands on a hovered control. */
        press_pending = false;
        mu_input_mousemove(&ctx, press_x, press_y);
        mu_input_mousedown(&ctx, press_x, press_y, MU_MOUSE_LEFT);
        /* Release immediately. Holding is not meaningful for these controls,
         * and it keeps a lifted finger from leaving the button stuck down if
         * the release edge arrives while we are still mid-tap. */
        mu_input_mouseup(&ctx, press_x, press_y, MU_MOUSE_LEFT);
        return;
    }

    if (input->down) {
        mu_input_mousemove(&ctx, input->x, input->y);
    } else {
        /* Park the pointer off-screen so nothing sits in a hover state while
         * no finger is touching. */
        mu_input_mousemove(&ctx, -1, -1);
    }
}

void ui_begin(const input_t *input)
{
    feed_input(input);
    mu_begin(&ctx);
}

/*---------------------------------------------------------------------------
 * Painting
 *-------------------------------------------------------------------------*/

static void draw_command(const mu_Command *cmd)
{
    switch (cmd->type) {

    case MU_COMMAND_RECT: {
        const mu_Color c = cmd->rect.color;
        /* Fully transparent rects are microui's way of drawing nothing; we
         * have no blending, so skip them rather than paint black. */
        if (c.a == 0) {
            break;
        }
        gfx_fill_rect(cmd->rect.rect.x, cmd->rect.rect.y,
                      cmd->rect.rect.w, cmd->rect.rect.h,
                      gfx_rgb(((uint32_t)c.r << 16) |
                              ((uint32_t)c.g << 8)  | c.b));
        break;
    }

    case MU_COMMAND_TEXT: {
        const mu_Color c = cmd->text.color;
        gfx_text(cmd->text.pos.x, cmd->text.pos.y, cmd->text.str,
                 gfx_rgb(((uint32_t)c.r << 16) |
                         ((uint32_t)c.g << 8)  | c.b));
        break;
    }

    case MU_COMMAND_ICON: {
        /* microui's icons are close/check/collapsed/expanded. Nothing here
         * uses them, so draw a small marker rather than pull in glyph artwork
         * we would not otherwise need. */
        const mu_Color c = cmd->icon.color;
        const mu_Rect r = cmd->icon.rect;
        gfx_fill_rect(r.x + r.w / 3, r.y + r.h / 3, r.w / 3, r.h / 3,
                      gfx_rgb(((uint32_t)c.r << 16) |
                              ((uint32_t)c.g << 8)  | c.b));
        break;
    }

    case MU_COMMAND_CLIP:
        gfx_set_clip(cmd->clip.rect.x, cmd->clip.rect.y,
                     cmd->clip.rect.w, cmd->clip.rect.h);
        break;

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
