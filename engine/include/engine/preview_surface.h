#ifndef ENGINE_PREVIEW_SURFACE_H
#define ENGINE_PREVIEW_SURFACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int width;
    int height;
    uint16_t* pixels;
} engine_preview_surface_t;

void engine_preview_render_transport_test(engine_preview_surface_t* surface);

#ifdef __cplusplus
}
#endif

#endif
