/*
 * ui_launcher - the home screen.
 *
 * Only the UI description lives here. Everything reusable - the microui
 * context, touch translation, painting and the repaint-only-what-changed
 * logic - is in ui.c, so an app can build its own UI the same way.
 */

#include "ui/ui_launcher.h"

#include "app.h"
#include "gfx/gfx.h"
#include "ui/launcher_layout_generated.h"
#include "ui/ui.h"

#define COL_BACKGROUND 0x0A0C14

static mu_Rect
launcher_mu_rect(const launcher_layout_rect_t* rect) {
    return mu_rect(rect->x, rect->y, rect->width, rect->height);
}

static const launcher_layout_t*
launcher_layout(void) {
    if (ui_width() == launcher_layout_landscape.canvas_width
        && ui_height() == launcher_layout_landscape.canvas_height) {
        return &launcher_layout_landscape;
    }
    return &launcher_layout_portrait;
}

void
ui_launcher_init(void) {
    ui_init();
}

int
ui_launcher_frame_layout(const input_t* input, const launcher_layout_t* layout) {
    int chosen = -1;

    mu_Context* ctx = ui_context();

    ui_begin(input);

    /* The lit edge is the button affordance on this dark screen and inverts
     * immediately under a finger. Style does not persist between frames. */
    ui_set_button_style(UI_BUTTON_BEZEL);

    /* The chrome-free window uses the transformed logical canvas. Its width
     * and height swap under a quarter turn, unlike the physical panel. */
    if (ui_begin_screen(ctx, "Launcher", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {

        /* The status bar and page indicator are authored slots. They stay
         * blank until their widgets land, without changing any card geometry. */
        mu_layout_set_next(ctx, launcher_mu_rect(&layout->rects[LAUNCHER_ELEMENT_STATUS_BAR]), 0);
        mu_layout_next(ctx);

        int visible_apps = app_list_count();
        if (visible_apps > 3) {
            visible_apps = 3;
        }
        for (int i = 0; i < visible_apps; i++) {
            launcher_element_id_t element = (launcher_element_id_t)(LAUNCHER_ELEMENT_LAST_PLAYED + i);
            mu_layout_set_next(ctx, launcher_mu_rect(&layout->rects[element]), 0);
            if (mu_button(ctx, app_list()[i]->name)) {
                chosen = i;
            }
        }

        mu_layout_set_next(ctx, launcher_mu_rect(&layout->rects[LAUNCHER_ELEMENT_PAGE_INDICATOR]), 0);
        mu_layout_next(ctx);

        mu_end_window(ctx);
    }

    /* Repaints only if the menu actually looks different from what is already
     * on screen - so a home screen nobody is touching costs no bus time at
     * all, rather than resending 322 KiB of identical pixels every frame. */
    ui_end(COL_BACKGROUND);

    return chosen;
}

int
ui_launcher_frame(const input_t* input) {
    return ui_launcher_frame_layout(input, launcher_layout());
}
