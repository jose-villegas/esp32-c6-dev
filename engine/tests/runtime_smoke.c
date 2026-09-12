#include "engine/runtime.h"

#include <stdint.h>
#include <stdlib.h>

int
main(void) {
    if (!engine_runtime_init()) {
        return 1;
    }

    const int portrait_width = 368;
    const int portrait_height = 448;
    const int landscape_width = portrait_height;
    const int landscape_height = portrait_width;
    uint16_t* pixels = malloc((size_t)landscape_width * (size_t)portrait_height * sizeof(*pixels));
    if (!pixels) {
        return 1;
    }

    const bool portrait = engine_runtime_render_launcher(pixels, portrait_width, portrait_height);
    const bool landscape = engine_runtime_render_launcher(pixels, landscape_width, landscape_height);
    const bool control_center = engine_runtime_render_control_center(pixels, portrait_width, portrait_height);
    engine_launcher_layout_t layout = {
        .canvas_width = landscape_width,
        .canvas_height = landscape_height,
        .rects =
            {
                {12, 12, 424, 34},
                {16, 98, 124, 173},
                {153, 98, 125, 173},
                {291, 98, 125, 173},
                {200, 279, 48, 8},
            },
    };
    const bool authored = engine_runtime_render_launcher_layout(pixels, landscape_width, landscape_height, &layout);
    layout.rects[1].width = landscape_width;
    const bool invalid_rejected =
        !engine_runtime_render_launcher_layout(pixels, landscape_width, landscape_height, &layout);
    engine_control_center_layout_t control_layout = {
        .canvas_width = portrait_width,
        .canvas_height = portrait_height,
        .rects =
            {
                {150, 10, 68, 4},
                {16, 26, 336, 20},
                {16, 54, 160, 78},
                {192, 54, 160, 78},
                {16, 144, 336, 58},
                {16, 214, 160, 52},
                {192, 214, 160, 52},
                {16, 280, 336, 18},
                {16, 308, 336, 58},
                {16, 378, 336, 58},
            },
    };
    const bool authored_control =
        engine_runtime_render_control_center_layout(pixels, portrait_width, portrait_height, &control_layout);
    free(pixels);
    return portrait && landscape && control_center && authored && authored_control && invalid_rejected ? 0 : 1;
}
