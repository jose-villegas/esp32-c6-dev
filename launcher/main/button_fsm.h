/*=============================================================================
 * button_fsm - raw button samples to press and release events.
 *
 * Pure logic: no GPIO, no clock, no I2C. Samples and the current time are
 * passed in, which is what lets a 40 ms debounce be asserted instantly in a
 * test rather than by sleeping, and lets a bounce sequence be constructed that
 * would be awkward to produce by hand on real hardware.
 *
 * The board's two buttons are not the same kind of thing, and this handles the
 * one that needs handling:
 *
 *   BOOT is a plain GPIO, so it reports a LEVEL and it bounces - a mechanical
 *   contact chatters for a few milliseconds on both make and break. Reading it
 *   naively turns one press into several.
 *
 *   PWR is wired to the AXP2101, which debounces in hardware and reports a
 *   finished "short press" EVENT over I2C. Nothing here applies to it.
 *===========================================================================*/
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* How long a new level must hold before it is believed.
 *
 * Long enough to outlast contact chatter, short enough to stay well under the
 * ~100 ms at which a button starts feeling unresponsive. */
#define BUTTON_DEBOUNCE_US (25 * 1000)

typedef struct {
    bool    stable;        /* the level currently believed */
    bool    candidate;     /* a level seen recently but not yet believed */
    int64_t candidate_us;  /* when the candidate first appeared */
    bool    primed;

    /* Edges, true only on the update that produced them. Cleared by
     * button_fsm_take_*, so an edge is consumed by whoever reads it first. */
    bool    pressed;
    bool    released;
} button_fsm_t;

void button_fsm_reset(button_fsm_t *b);

/* Feed one raw sample. `now_us` is a monotonic microsecond clock. */
void button_fsm_update(button_fsm_t *b, bool raw_down, int64_t now_us);

bool button_fsm_is_down(const button_fsm_t *b);

/* Read and clear the pending edges. Reading consumes them, so a press cannot
 * be handled twice by two different readers - the same contract touch_fsm
 * uses, and for the same reason. */
bool button_fsm_take_pressed(button_fsm_t *b);
bool button_fsm_take_released(button_fsm_t *b);
