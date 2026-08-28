/*=============================================================================
 * gesture - recognising touch gestures from input state.
 *
 * Pure logic, deliberately free of hardware and of any screen it might be
 * running on, so it can be tested on the host. The screen dimensions are
 * passed in rather than taken from gfx.h for the same reason.
 *
 * Which edge carries the home gesture is also a parameter, not a constant:
 * this module has no idea the board rotates, where the USB connector is, or
 * which edge the shell currently wants - that mapping is a piece of
 * shell-integration knowledge that belongs in main.c, not here. All this
 * module knows is "an edge", expressed as one of the four values below.
 *===========================================================================*/
#pragma once

#include <stdbool.h>

#include "app.h"

/* Which physical edge of the screen the home gesture currently lives on.
 * The caller decides this - see main.c's exit_edge_for_quarter() - by
 * working out which edge is opposite the USB connector for the board's
 * current orientation. */
typedef enum {
    GESTURE_EDGE_TOP,
    GESTURE_EDGE_BOTTOM,
    GESTURE_EDGE_LEFT,
    GESTURE_EDGE_RIGHT,
} gesture_edge_t;

/* How far into the screen from the target edge a home swipe may begin.
 * Generous, because a fingertip landing "at the edge" is not precise. Named
 * for depth rather than height now that this applies to left/right edges
 * too, but the tuned value is unchanged. */
#define GESTURE_HOME_ZONE_DEPTH 64

/* How far it must travel away from that edge, toward the centre, to count.
 * Large enough that a tap wobbling near the edge cannot trigger it by
 * accident. */
#define GESTURE_HOME_SWIPE_DIST 90

/* True while a finger that started near the given edge has travelled far
 * enough toward the centre of the screen.
 *
 * Requires the finger to still be down, so it fires partway through the swipe
 * rather than on release - waiting for the lift feels sluggish. That also means
 * it must not match on stale coordinates once contact ends. */
bool gesture_is_home_swipe(const input_t *input, gesture_edge_t edge,
                            int screen_w, int screen_h);
