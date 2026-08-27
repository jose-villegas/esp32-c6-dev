#include "input/gesture.h"

bool gesture_is_home_swipe(const input_t *input, int screen_height)
{
    if (!input->down) {
        return false;
    }

    const bool started_at_bottom =
        input->press_y >= (screen_height - GESTURE_HOME_ZONE_HEIGHT);

    /* Positive when the finger has moved up the screen, since y grows
     * downward. Horizontal travel is ignored: fingers do not swipe in
     * straight lines, and only the vertical component carries intent. */
    const int travelled_up = input->press_y - input->y;

    return started_at_bottom && travelled_up >= GESTURE_HOME_SWIPE_DIST;
}
