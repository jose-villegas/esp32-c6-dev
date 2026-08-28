/*=============================================================================
 * ui_launcher - the home screen.
 *
 * Only the UI description lives here. Everything reusable - the microui
 * context, touch translation, painting and the repaint-only-what-changed
 * logic - is in ui.c, so an app can build its own UI the same way.
 *===========================================================================*/

#include "ui/ui_launcher.h"

#include "app.h"
#include "gfx/gfx.h"
#include "ui/ui.h"

#define COL_BACKGROUND 0x0A0C14

/* Sized to the longest current app name plus margin, the same way
 * MENU_BTN_W (app_sand.c) is sized to its longest quality label: "Falling
 * Sand" is the longest of the three registered app names (app_cube.c,
 * app_diagnostics.c, app_sand.c) at 12 characters, which at GFX_CHAR_W (16
 * px, see gfx.h) is 192 px of text. mu_draw_control_text() in microui.c
 * centers a button's label and clips it to the button's own rect rather
 * than wrapping or shrinking it, so a label wider than its button is
 * chopped off at both ends with no warning and no crash. 240 leaves 64 px
 * margins either side of the panel at its narrower dimension (GFX_WIDTH
 * 368) and 48 px of slack around the longest label - if a future app name
 * needs a longer label than this, check its width against this number
 * before assuming it will fit. */
#define LAUNCHER_BTN_W 240

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
     * desktop, so the frame, title bar and close button would be noise.
     *
     * Sized from ui_width()/ui_height(), not GFX_WIDTH/GFX_HEIGHT: those two
     * are the logical canvas, which is the physical panel mapped through the
     * inverse of the current transform, and they swap under a quarter turn.
     * A window hardcoded to the panel's own dimensions would still claim the
     * un-rotated size after such a turn and overflow the rotated canvas. */
    if (ui_begin_screen(ctx, "Launcher",
                        MU_OPT_NOTITLE | MU_OPT_NORESIZE |
                        MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {

        /* The banner. Claimed from the layout and left blank: mu_layout_next()
         * hands back the rect and advances past it, which is how an
         * immediate-mode UI reserves space without a widget in it. When there
         * is status to show, it is drawn into that rect and nothing below
         * moves. */
        mu_layout_row(ctx, 1, (int[]){ -1 }, UI_BANNER_HEIGHT);
        mu_layout_next(ctx);

        /* Fixed-width, centred rather than filled - see ui_centered_rect()
         * in ui.h. A small number of large tap targets, the same as the
         * sand boot menu's START/QUALITY pair, so they should stay
         * LAUNCHER_BTN_W wide and centre rather than stretch edge to edge
         * on the wider 448px canvas a quarter turn produces.
         *
         * `y` is tracked by hand rather than left to mu_layout_row()'s
         * automatic stacking, because mu_layout_set_next()'s ABSOLUTE mode
         * (the `0` below) bypasses that stacking entirely - see its own
         * comment in microui.c. Starting at UI_BANNER_HEIGHT + UI_ROW_GAP
         * and advancing by UI_ROW_HEIGHT + UI_ROW_GAP reproduces exactly
         * where the old mu_layout_row()-driven rows landed, so only the
         * horizontal fill-to-fixed change is visible here - the list stays
         * anchored to the top, immediately below the banner, not centred
         * as a block the way the boot menu's two-button set is. */
        int y = UI_BANNER_HEIGHT + UI_ROW_GAP;
        for (int i = 0; i < app_list_count(); i++) {
            mu_layout_set_next(ctx,
                               ui_centered_rect(ui_width(), LAUNCHER_BTN_W,
                                                UI_ROW_HEIGHT, y),
                               0);
            if (mu_button(ctx, app_list()[i]->name)) {
                chosen = i;
            }
            y += UI_ROW_HEIGHT + UI_ROW_GAP;
        }

        mu_end_window(ctx);
    }

    /* Repaints only if the menu actually looks different from what is already
     * on screen - so a home screen nobody is touching costs no bus time at
     * all, rather than resending 322 KiB of identical pixels every frame. */
    ui_end(COL_BACKGROUND);

    return chosen;
}
