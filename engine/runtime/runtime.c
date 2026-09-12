#include "engine/runtime.h"

#include "gfx/gfx.h"
#include "microui.h"

static mu_Context ui_context;
static bool initialized;

bool
engine_runtime_init(void) {
    if (initialized) {
        return true;
    }
    if (!gfx_init()) {
        return false;
    }

    mu_init(&ui_context);
    initialized = true;
    return true;
}
