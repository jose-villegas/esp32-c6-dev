#pragma once

#include <stdbool.h>

#include "app.h"
#include "input/gesture.h"

typedef enum {
    SYSTEM_SCREEN_LAUNCHER,
    SYSTEM_SCREEN_CONTROL_CENTER,
} system_screen_t;

typedef struct {
    system_screen_t screen;
} system_navigation_t;

void system_navigation_init(system_navigation_t* navigation);
bool system_navigation_step(system_navigation_t* navigation, const input_t* input, gesture_edge_t open_edge,
                            gesture_edge_t close_edge, int screen_width, int screen_height);
