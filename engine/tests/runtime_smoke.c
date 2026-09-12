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
    free(pixels);
    return portrait && landscape ? 0 : 1;
}
