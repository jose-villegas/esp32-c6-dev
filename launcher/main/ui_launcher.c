/*=============================================================================
 * ui_launcher - the home screen, built with microui.
 *
 * microui is immediate-mode and does no drawing itself: each frame it turns
 * the UI description below into a list of rectangles, text and icons, and we
 * walk that list painting into the shared framebuffer.
 *
 * That command-list model is why microui suits this device. A retained-mode
 * toolkit wants to own the display and the refresh cycle, which fights an app
 * like the cube that owns its own framebuffer. Here the shell just renders a
 * list of primitives whenever it likes, into whatever it likes.
 *===========================================================================*/

#include <string.h>

#include "ui_launcher.h"

#include "app.h"
#include "gfx.h"
#include "microui.h"

/* Palette. Deliberately dark: this is an OLED, so black pixels are off
 * pixels - it costs less power and looks better than a grey chrome. */
#define COL_BACKGROUND 0x0A0C14
#define COL_SURFACE    0x161A28
#define COL_SURFACE_HI 0x232A40
#define COL_ACCENT     0x3DDC97
#define COL_TEXT       0xE6EAF2
#define COL_TEXT_DIM   0x8A93A8

#define TITLE_HEIGHT   56
#define ROW_HEIGHT     64
#define ROW_GAP        8
#define MARGIN         16

static mu_Context ctx;

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

void ui_launcher_init(void)
{
    mu_init(&ctx);
    ctx.text_width  = measure_text_width;
    ctx.text_height = measure_text_height;

    /* Restyle microui's defaults to the palette above. */
    ctx.style->colors[MU_COLOR_WINDOWBG]    = (mu_Color){ 0x0A, 0x0C, 0x14, 255 };
    ctx.style->colors[MU_COLOR_TEXT]        = (mu_Color){ 0xE6, 0xEA, 0xF2, 255 };
    ctx.style->colors[MU_COLOR_BUTTON]      = (mu_Color){ 0x16, 0x1A, 0x28, 255 };
    ctx.style->colors[MU_COLOR_BUTTONHOVER] = (mu_Color){ 0x23, 0x2A, 0x40, 255 };
    ctx.style->colors[MU_COLOR_BUTTONFOCUS] = (mu_Color){ 0x3D, 0xDC, 0x97, 255 };
    ctx.style->padding      = 12;
    ctx.style->spacing      = ROW_GAP;
    ctx.style->indent       = 0;
    ctx.style->title_height = TITLE_HEIGHT;
}

/* Translate touch into the mouse events microui expects.
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
 * latency (~40 ms, imperceptible) and makes a tap register every time. */
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

/* Paint one microui command list into the framebuffer. */
static void render_commands(void)
{
    mu_Command *cmd = NULL;

    while (mu_next_command(&ctx, &cmd)) {
        switch (cmd->type) {

        case MU_COMMAND_RECT: {
            const mu_Color c = cmd->rect.color;
            /* Fully transparent rects are microui's way of drawing nothing;
             * we have no blending, so skip them rather than paint black. */
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
            /* microui's icons are close/check/collapsed/expanded. The
             * launcher uses none of them, so draw a small marker rather than
             * pull in glyph artwork we would not otherwise need. */
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

    gfx_clear_clip();
}

int ui_launcher_frame(const input_t *input)
{
    int chosen = -1;

    feed_input(input);

    mu_begin(&ctx);

    /* One full-screen window with no chrome: this is a home screen, not a
     * desktop, so the frame, title bar and close button would be noise. */
    if (mu_begin_window_ex(&ctx, "Launcher",
                           mu_rect(0, 0, GFX_WIDTH, GFX_HEIGHT),
                           MU_OPT_NOTITLE | MU_OPT_NORESIZE |
                           MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {

        mu_layout_row(&ctx, 1, (int[]){ -1 }, gfx_text_height() + 8);
        mu_text(&ctx, "APPS");

        for (int i = 0; i < app_list_count(); i++) {
            mu_layout_row(&ctx, 1, (int[]){ -1 }, ROW_HEIGHT);
            if (mu_button(&ctx, app_list()[i]->name)) {
                chosen = i;
            }
        }

        mu_end_window(&ctx);
    }

    mu_end(&ctx);

    gfx_clear(gfx_rgb(COL_BACKGROUND));
    render_commands();

    return chosen;
}
