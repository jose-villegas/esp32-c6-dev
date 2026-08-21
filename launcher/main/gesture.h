/*=============================================================================
 * gesture - recognising touch gestures from input state.
 *
 * Pure logic, deliberately free of hardware and of any screen it might be
 * running on, so it can be tested on the host. The screen height is passed in
 * rather than taken from gfx.h for the same reason.
 *===========================================================================*/
#pragma once

#include <stdbool.h>

#include "app.h"

/* How far up from the bottom edge a home swipe may begin. Generous, because a
 * fingertip landing "at the bottom" is not precise. */
#define GESTURE_HOME_ZONE_HEIGHT 64

/* How far it must travel upward to count. Large enough that a tap wobbling
 * near the bottom edge cannot trigger it by accident. */
#define GESTURE_HOME_SWIPE_DIST 90

/* True while a finger that started near the bottom edge has travelled far
 * enough upward.
 *
 * Requires the finger to still be down, so it fires partway through the swipe
 * rather than on release - waiting for the lift feels sluggish. That also means
 * it must not match on stale coordinates once contact ends. */
bool gesture_is_home_swipe(const input_t *input, int screen_height);
