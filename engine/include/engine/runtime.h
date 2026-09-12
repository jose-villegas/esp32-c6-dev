#ifndef ENGINE_RUNTIME_H
#define ENGINE_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool engine_runtime_init(void);
bool engine_runtime_render_launcher(uint16_t* pixels, int width, int height);

#ifdef __cplusplus
}
#endif

#endif
