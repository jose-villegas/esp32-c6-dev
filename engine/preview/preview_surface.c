#include "engine/preview_surface.h"

#include "engine/runtime.h"

bool
engine_preview_render_launcher(engine_preview_surface_t* surface) {
    if (!surface) {
        return false;
    }
    return engine_runtime_render_launcher(surface->pixels, surface->width, surface->height);
}

bool
engine_preview_render_launcher_layout(engine_preview_surface_t* surface, const engine_launcher_layout_t* layout) {
    if (!surface || !layout) {
        return false;
    }
    return engine_runtime_render_launcher_layout(surface->pixels, surface->width, surface->height, layout);
}
