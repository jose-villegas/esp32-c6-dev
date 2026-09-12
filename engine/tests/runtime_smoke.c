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
    free(pixels);
    return portrait && landscape && authored && invalid_rejected ? 0 : 1;
}
