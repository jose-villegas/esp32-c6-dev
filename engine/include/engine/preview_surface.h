#ifndef ENGINE_PREVIEW_SURFACE_H
#define ENGINE_PREVIEW_SURFACE_H

#include <stdbool.h>
#include <stdint.h>

#include "engine/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int width;
    int height;
    uint16_t* pixels;
} engine_preview_surface_t;

bool engine_preview_render_launcher(engine_preview_surface_t* surface);
bool engine_preview_render_launcher_layout(engine_preview_surface_t* surface, const engine_launcher_layout_t* layout);

#ifdef __cplusplus
}
#endif

#endif
