#include "engine/runtime.h"

#include <stddef.h>

#include "app.h"
#include "gfx/gfx.h"
#include "ui/ui.h"
#include "ui/ui_control_center.h"
#include "ui/ui_launcher.h"
#include "ui/ui_transform.h"

_Static_assert(ENGINE_LAUNCHER_ELEMENT_COUNT == LAUNCHER_ELEMENT_COUNT, "launcher element count mismatch");
_Static_assert(ENGINE_CONTROL_CENTER_ELEMENT_COUNT == CONTROL_CENTER_ELEMENT_COUNT,
               "Control Center element count mismatch");

static bool initialized;

static const app_t preview_cube = {.name = "3D Cube", .summary = "Real-time 3D rendering"};
static const app_t preview_diagnostics = {.name = "Diagnostics", .summary = "Device status"};
static const app_t preview_sand = {.name = "Falling Sand", .summary = "Particle simulation"};

static const app_t* const preview_apps[] = {
    &preview_cube,
    &preview_diagnostics,
    &preview_sand,
};

const app_t* const*
app_list(void) {
    return preview_apps;
}

int
app_list_count(void) {
    return (int)(sizeof(preview_apps) / sizeof(preview_apps[0]));
}

bool
engine_runtime_init(void) {
    if (initialized) {
        return true;
    }
    if (!gfx_init()) {
        return false;
    }

    ui_launcher_init();
    initialized = true;
    return true;
}

static uint16_t
native_rgb565(gfx_color_t color) {
    return (uint16_t)((color >> 8) | (color << 8));
}

static bool
valid_authored_layout(const engine_launcher_rect_t* rects, int count, int canvas_width, int canvas_height, int width,
                      int height) {
    if (canvas_width != width || canvas_height != height) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        const engine_launcher_rect_t* rect = &rects[i];
        if (rect->x < 0 || rect->y < 0 || rect->width <= 0 || rect->height <= 0 || rect->x > width || rect->y > height
            || rect->width > width - rect->x || rect->height > height - rect->y) {
            return false;
        }
    }
    return true;
}

static bool
prepare_render(uint16_t* pixels, int width, int height, bool* portrait) {
    if (!pixels || !engine_runtime_init()) {
        return false;
    }
    *portrait = width == GFX_WIDTH && height == GFX_HEIGHT;
    return *portrait || (width == GFX_HEIGHT && height == GFX_WIDTH);
}

static void
copy_framebuffer(uint16_t* pixels, int width, int height, bool portrait) {
    const gfx_color_t* framebuffer = gfx_framebuffer();
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const size_t source =
                portrait ? (size_t)y * GFX_WIDTH + (size_t)x : (size_t)x * GFX_WIDTH + (size_t)(GFX_WIDTH - 1 - y);
            pixels[(size_t)y * (size_t)width + (size_t)x] = native_rgb565(framebuffer[source]);
        }
    }
}

static bool
render_launcher(uint16_t* pixels, int width, int height, const engine_launcher_layout_t* authored_layout) {
    bool portrait;
    if (!prepare_render(pixels, width, height, &portrait)) {
        return false;
    }
    const bool landscape = width == GFX_HEIGHT && height == GFX_WIDTH;

    const ui_transform_t transform =
        landscape ? ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT) : ui_transform_identity();
    const input_t input = {0};
    ui_set_transform(transform);

    if (authored_layout) {
        if (!valid_authored_layout(authored_layout->rects, ENGINE_LAUNCHER_ELEMENT_COUNT, authored_layout->canvas_width,
                                   authored_layout->canvas_height, width, height)) {
            return false;
        }
        launcher_layout_t layout = {
            .canvas_width = (int16_t)authored_layout->canvas_width,
            .canvas_height = (int16_t)authored_layout->canvas_height,
        };
        for (int i = 0; i < ENGINE_LAUNCHER_ELEMENT_COUNT; i++) {
            layout.rects[i] = (launcher_layout_rect_t){
                .x = (int16_t)authored_layout->rects[i].x,
                .y = (int16_t)authored_layout->rects[i].y,
                .width = (int16_t)authored_layout->rects[i].width,
                .height = (int16_t)authored_layout->rects[i].height,
            };
        }
        ui_launcher_frame_layout(&input, &layout);
    } else {
        ui_launcher_frame(&input);
    }

    copy_framebuffer(pixels, width, height, portrait);
    return true;
}

static bool
render_control_center(uint16_t* pixels, int width, int height, const engine_control_center_layout_t* authored_layout) {
    bool portrait;
    if (!prepare_render(pixels, width, height, &portrait)) {
        return false;
    }
    const bool landscape = !portrait;
    const ui_transform_t transform =
        landscape ? ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT) : ui_transform_identity();
    const input_t input = {0};
    ui_set_transform(transform);

    if (authored_layout) {
        if (!valid_authored_layout(authored_layout->rects, ENGINE_CONTROL_CENTER_ELEMENT_COUNT,
                                   authored_layout->canvas_width, authored_layout->canvas_height, width, height)) {
            return false;
        }
        control_center_layout_t layout = {
            .canvas_width = (int16_t)authored_layout->canvas_width,
            .canvas_height = (int16_t)authored_layout->canvas_height,
        };
        for (int i = 0; i < ENGINE_CONTROL_CENTER_ELEMENT_COUNT; i++) {
            layout.rects[i] = (control_center_layout_rect_t){
                .x = (int16_t)authored_layout->rects[i].x,
                .y = (int16_t)authored_layout->rects[i].y,
                .width = (int16_t)authored_layout->rects[i].width,
                .height = (int16_t)authored_layout->rects[i].height,
            };
        }
        ui_control_center_frame_layout(&input, &layout);
    } else {
        ui_control_center_frame(&input);
    }
    copy_framebuffer(pixels, width, height, portrait);
    return true;
}

bool
engine_runtime_render_control_center(uint16_t* pixels, int width, int height) {
    return render_control_center(pixels, width, height, NULL);
}

bool
engine_runtime_render_control_center_layout(uint16_t* pixels, int width, int height,
                                            const engine_control_center_layout_t* layout) {
    return render_control_center(pixels, width, height, layout);
}

bool
engine_runtime_render_launcher(uint16_t* pixels, int width, int height) {
    return render_launcher(pixels, width, height, NULL);
}

bool
engine_runtime_render_launcher_layout(uint16_t* pixels, int width, int height, const engine_launcher_layout_t* layout) {
    return render_launcher(pixels, width, height, layout);
}
