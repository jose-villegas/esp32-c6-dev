#include "ui/ui_control_center.h"

#include <string.h>

#include "ui/ui.h"

#define COL_BACKGROUND 0x071923

static mu_Rect
control_rect(const control_center_layout_t* layout, control_center_element_id_t element) {
    const control_center_layout_rect_t* rect = &layout->rects[element];
    return mu_rect(rect->x, rect->y, rect->width, rect->height);
}

static const control_center_layout_t*
active_layout(void) {
    if (ui_width() == control_center_layout_landscape.canvas_width
        && ui_height() == control_center_layout_landscape.canvas_height) {
        return &control_center_layout_landscape;
    }
    return &control_center_layout_portrait;
}

static void
draw_text(mu_Context* ctx, mu_Rect rect, const char* text) {
    mu_draw_text(ctx, ctx->style->font, text, -1, mu_vec2(rect.x, rect.y), ctx->style->colors[MU_COLOR_TEXT]);
}

static void
draw_connectivity_card(mu_Context* ctx, mu_Rect panel, const char* title, const char* status) {
    const mu_Id id = mu_get_id(ctx, title, (int)strlen(title));
    mu_update_control(ctx, id, panel, 0);
    mu_draw_control_frame(ctx, id, panel, MU_COLOR_BUTTON, 0);
    draw_text(ctx, mu_rect(panel.x + 12, panel.y + 20, panel.w - 24, 16), title);
    draw_text(ctx, mu_rect(panel.x + 12, panel.y + 42, panel.w - 24, 16), status);
}

static void
draw_slider(mu_Context* ctx, mu_Rect panel, const char* label, int* value) {
    draw_text(ctx, mu_rect(panel.x, panel.y, panel.w, 16), label);
    mu_layout_set_next(ctx, mu_rect(panel.x, panel.y + 22, panel.w, panel.h - 22), 0);
    ui_slider_int(ctx, value, 0, 100, 5);
}

void
ui_control_center_frame_layout(const input_t* input, const control_center_layout_t* layout) {
    static int volume = 65;
    static int brightness = 80;
    mu_Context* ctx = ui_context();
    ui_begin(input);
    ui_set_button_style(UI_BUTTON_BEZEL);

    if (ui_begin_screen(ctx, "Control Center", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        mu_draw_rect(ctx, control_rect(layout, CONTROL_CENTER_ELEMENT_GRABBER),
                     ctx->style->colors[MU_COLOR_BUTTONFOCUS]);
        draw_text(ctx, control_rect(layout, CONTROL_CENTER_ELEMENT_HEADER), "CONTROL CENTER");

        draw_connectivity_card(ctx, control_rect(layout, CONTROL_CENTER_ELEMENT_WIFI), "WI-FI", "STUDIO-5G");
        draw_connectivity_card(ctx, control_rect(layout, CONTROL_CENTER_ELEMENT_BLUETOOTH), "BLUETOOTH", "CONTROLLER");
        draw_connectivity_card(ctx, control_rect(layout, CONTROL_CENTER_ELEMENT_LINK), "LINK", "USB READY");

        draw_slider(ctx, control_rect(layout, CONTROL_CENTER_ELEMENT_VOLUME), "VOLUME", &volume);
        draw_slider(ctx, control_rect(layout, CONTROL_CENTER_ELEMENT_BRIGHTNESS), "BRIGHTNESS", &brightness);

        draw_text(ctx, control_rect(layout, CONTROL_CENTER_ELEMENT_NOTIFICATIONS_HEADER), "NOTIFICATIONS");
        mu_layout_set_next(ctx, control_rect(layout, CONTROL_CENTER_ELEMENT_LIBRARY_NOTIFICATION), 0);
        mu_button(ctx, "LIBRARY SCAN COMPLETE");
        mu_layout_set_next(ctx, control_rect(layout, CONTROL_CENTER_ELEMENT_CONTROLLER_NOTIFICATION), 0);
        mu_button(ctx, "CONTROLLER CONNECTED");
        mu_end_window(ctx);
    }
    ui_end(COL_BACKGROUND);
}

void
ui_control_center_frame(const input_t* input) {
    ui_control_center_frame_layout(input, active_layout());
}
