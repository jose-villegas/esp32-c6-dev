#include "input/gesture.h"

bool gesture_is_home_swipe(const input_t *input, gesture_edge_t edge,
                            int screen_w, int screen_h)
{
    if (!input->down) {
        return false;
    }

    /* Which axis and coordinate matter depends on the edge: top/bottom read
     * y against screen_h, left/right read x against screen_w. In each case
     * "travelled" is measured toward the centre - away from that edge - in
     * whichever sign that means for it, since y grows downward and x grows
     * rightward. */
    bool started_in_zone = false;
    int travelled_toward_centre = 0;

    switch (edge) {
    case GESTURE_EDGE_TOP:
        started_in_zone = input->press_y <= GESTURE_HOME_ZONE_DEPTH;
        travelled_toward_centre = input->y - input->press_y;
        break;
    case GESTURE_EDGE_BOTTOM:
        started_in_zone = input->press_y >= (screen_h - GESTURE_HOME_ZONE_DEPTH);
        travelled_toward_centre = input->press_y - input->y;
        break;
    case GESTURE_EDGE_LEFT:
        started_in_zone = input->press_x <= GESTURE_HOME_ZONE_DEPTH;
        travelled_toward_centre = input->x - input->press_x;
        break;
    case GESTURE_EDGE_RIGHT:
        started_in_zone = input->press_x >= (screen_w - GESTURE_HOME_ZONE_DEPTH);
        travelled_toward_centre = input->press_x - input->x;
        break;
    }

    return started_in_zone && travelled_toward_centre >= GESTURE_HOME_SWIPE_DIST;
}
