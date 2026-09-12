#ifndef ENGINE_RUNTIME_H
#define ENGINE_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENGINE_LAUNCHER_ELEMENT_COUNT       5
#define ENGINE_CONTROL_CENTER_ELEMENT_COUNT 10

typedef struct {
    int x;
    int y;
    int width;
    int height;
} engine_launcher_rect_t;

typedef struct {
    int canvas_width;
    int canvas_height;
    engine_launcher_rect_t rects[ENGINE_LAUNCHER_ELEMENT_COUNT];
} engine_launcher_layout_t;

typedef struct {
    int canvas_width;
    int canvas_height;
    engine_launcher_rect_t rects[ENGINE_CONTROL_CENTER_ELEMENT_COUNT];
} engine_control_center_layout_t;

bool engine_runtime_init(void);
bool engine_runtime_render_launcher(uint16_t* pixels, int width, int height);
bool engine_runtime_render_launcher_layout(uint16_t* pixels, int width, int height,
                                           const engine_launcher_layout_t* layout);
bool engine_runtime_render_control_center(uint16_t* pixels, int width, int height);
bool engine_runtime_render_control_center_layout(uint16_t* pixels, int width, int height,
                                                 const engine_control_center_layout_t* layout);

#ifdef __cplusplus
}
#endif

#endif
