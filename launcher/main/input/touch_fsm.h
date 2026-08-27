/*=============================================================================
 * touch_fsm - turns raw contact reports into press/release events.
 *
 * Deliberately free of hardware: no I2C, no FreeRTOS, and time arrives as an
 * argument rather than being read from a clock. That is what lets the whole
 * state machine be tested on the host in microseconds, including the timing
 * behaviour, which would otherwise need real waiting on real hardware.
 *
 * touch.c supplies the samples; this decides what they mean.
 *===========================================================================*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app.h"

/* How long the controller must report nothing before the finger counts as
 * lifted.
 *
 * The FT5x06's INT line signals "there is fresh data", not "a finger is
 * present", and it drops briefly during a touch. Treating the first quiet
 * sample as a release makes a held finger flicker between pressed and
 * released. Requiring a quiet period debounces that; the cost is a few
 * milliseconds of extra latency on release, which is imperceptible. */
#define TOUCH_RELEASE_QUIET_US (60 * 1000)

typedef struct {
    bool    down;
    int     x, y;
    int     press_x, press_y;
    bool    pressed;         /* latched until taken */
    bool    released;        /* latched until taken */
    int64_t last_contact_us;
} touch_fsm_t;

void touch_fsm_init(touch_fsm_t *fsm);

/* Feed one sample. `have_point` is whether the controller reported a contact;
 * x and y are only meaningful when it did. */
void touch_fsm_update(touch_fsm_t *fsm, bool have_point,
                      int x, int y, int64_t now_us);

/* Copy the current state out and consume the latched edges, so each press and
 * release is reported to exactly one caller. */
void touch_fsm_take(touch_fsm_t *fsm, input_t *out);
