/*=============================================================================
 * touch - reads the touch panel on its own schedule.
 *
 * Sampling is deliberately decoupled from rendering. A frame takes ~40 ms
 * (the panel blit alone is 25 ms), and a quick tap can be shorter than that,
 * so polling once per frame drops taps entirely. This runs at TOUCH_POLL_HZ
 * and latches press/release edges, so an event that happens between two frames
 * is still delivered to the next one.
 *===========================================================================*/
#pragma once

#include "app.h"

/* Fast enough that a brief tap is sampled several times, cheap enough to be
 * irrelevant next to rendering (one small I2C read per poll, and only when
 * the controller says it has data). */
#define TOUCH_POLL_HZ 100

/* Starts the polling task. Safe to call if the panel is missing: reads simply
 * report nothing rather than failing. */
void touch_start(void);

/* Copies the accumulated state into `out` and clears the latched edges, so
 * each press and release is reported exactly once. */
void touch_read(input_t *out);
