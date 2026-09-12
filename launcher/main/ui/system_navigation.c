#include "ui/system_navigation.h"

void
system_navigation_init(system_navigation_t* navigation) {
    navigation->screen = SYSTEM_SCREEN_LAUNCHER;
}

bool
system_navigation_step(system_navigation_t* navigation, const input_t* input, gesture_edge_t open_edge,
                       gesture_edge_t close_edge, int screen_width, int screen_height) {
    if (navigation->screen == SYSTEM_SCREEN_LAUNCHER
        && gesture_is_edge_swipe(input, open_edge, screen_width, screen_height)) {
        navigation->screen = SYSTEM_SCREEN_CONTROL_CENTER;
        return true;
    }
    if (navigation->screen == SYSTEM_SCREEN_CONTROL_CENTER
        && gesture_is_edge_swipe(input, close_edge, screen_width, screen_height)) {
        navigation->screen = SYSTEM_SCREEN_LAUNCHER;
        return true;
    }
    return false;
}
