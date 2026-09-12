#include "engine/runtime.h"

#include <stddef.h>

#include "app.h"
#include "gfx/gfx.h"
#include "ui/ui.h"
#include "ui/ui_launcher.h"
#include "ui/ui_transform.h"

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

bool
engine_runtime_render_launcher(uint16_t* pixels, int width, int height) {
    if (!pixels || !engine_runtime_init()) {
        return false;
    }

    const bool portrait = width == GFX_WIDTH && height == GFX_HEIGHT;
    const bool landscape = width == GFX_HEIGHT && height == GFX_WIDTH;
    if (!portrait && !landscape) {
        return false;
    }

    const ui_transform_t transform =
        landscape ? ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT) : ui_transform_identity();
    const input_t input = {0};
    ui_set_transform(transform);
    ui_launcher_frame(&input);

    const gfx_color_t* framebuffer = gfx_framebuffer();
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const size_t source =
                portrait ? (size_t)y * GFX_WIDTH + (size_t)x : (size_t)x * GFX_WIDTH + (size_t)(GFX_WIDTH - 1 - y);
            pixels[(size_t)y * (size_t)width + (size_t)x] = native_rgb565(framebuffer[source]);
        }
    }
    return true;
}
