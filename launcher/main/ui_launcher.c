/*=============================================================================
 * ui_launcher - the home screen.
 *
 * Only the UI description lives here. Everything reusable - the microui
 * context, touch translation, painting and the repaint-only-what-changed
 * logic - is in ui.c, so an app can build its own UI the same way.
 *===========================================================================*/

#include "ui_launcher.h"

#include "app.h"
#include "gfx.h"
#include "ui.h"

#define COL_BACKGROUND 0x0A0C14

void ui_launcher_init(void)
{
    ui_init();
}

int ui_launcher_frame(const input_t *input)
{
    int chosen = -1;

    mu_Context *ctx = ui_context();

    ui_begin(input);

    /* Bezelled buttons. On a screen whose only affordance is that a rectangle
     * is slightly lighter than the black around it, a lit edge is what says
     * "this is a thing you press" - and it inverts under a finger, so the
     * press is visible before the app has finished starting. Stated every
     * frame because style does not persist: see ui.h. */
    ui_set_button_style(UI_BUTTON_BEZEL);

    /* One full-screen window with no chrome: this is a home screen, not a
     * desktop, so the frame, title bar and close button would be noise. */
    if (mu_begin_window_ex(ctx, "Launcher",
                           mu_rect(0, 0, GFX_WIDTH, GFX_HEIGHT),
                           MU_OPT_NOTITLE | MU_OPT_NORESIZE |
                           MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {

        /* The banner. Claimed from the layout and left blank: mu_layout_next()
         * hands back the rect and advances past it, which is how an
         * immediate-mode UI reserves space without a widget in it. When there
         * is status to show, it is drawn into that rect and nothing below
         * moves. */
        mu_layout_row(ctx, 1, (int[]){ -1 }, UI_BANNER_HEIGHT);
        mu_layout_next(ctx);

        for (int i = 0; i < app_list_count(); i++) {
            mu_layout_row(ctx, 1, (int[]){ -1 }, UI_ROW_HEIGHT);
            if (mu_button(ctx, app_list()[i]->name)) {
                chosen = i;
            }
        }

        mu_end_window(ctx);
    }

    /* Repaints only if the menu actually looks different from what is already
     * on screen - so a home screen nobody is touching costs no bus time at
     * all, rather than resending 322 KiB of identical pixels every frame. */
    ui_end(COL_BACKGROUND);

    return chosen;
}
