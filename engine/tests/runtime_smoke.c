#include "engine/runtime.h"

int
main(void) {
    if (!engine_runtime_init()) {
        return 1;
    }
    return engine_runtime_init() ? 0 : 1;
}
