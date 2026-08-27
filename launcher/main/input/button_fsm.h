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

/* How long a button must stay continuously down, after the debounced press,
 * before that press counts as a HOLD rather than a tap.
 *
 * Long enough that a brisk, deliberate press never fires it by accident;
 * short enough that holding the button down does not feel like the device
 * has stopped responding. Must stay well clear of BUTTON_DEBOUNCE_US above -
 * it times from the debounced edge, not the first raw sample, so the two
 * windows do not compete. */
#define BUTTON_HOLD_US (600 * 1000)

/*-----------------------------------------------------------------------------
 * Contract
 *
 * `pressed`  fires on the debounced press edge, as always.
 * `held`     fires EXACTLY ONCE, the moment the button has been continuously
 *            down for BUTTON_HOLD_US since the debounced press.
 * `released` fires on the debounced release edge ONLY IF the press did not
 *            become a hold. A press that turned into a hold delivers no
 *            release edge at all - `held` already told the caller everything
 *            it needs to know, and a trailing `released` would just be a
 *            second event for the same physical press.
 *
 * That last rule is the point of putting this here rather than in the
 * caller: it makes short-press and long-press mutually exclusive in the
 * pure, tested layer, so a caller can write `if (released) cycle();` and
 * `if (held) open_panel();` side by side with no bookkeeping of its own and
 * no risk of both firing for one press.
 *---------------------------------------------------------------------------*/

typedef struct {
    bool    stable;        /* the level currently believed */
    bool    candidate;     /* a level seen recently but not yet believed */
    int64_t candidate_us;  /* when the candidate first appeared */
    bool    primed;

    int64_t down_since_us; /* when `stable` last became true - the hold clock */
    bool    hold_fired;    /* whether this press has already delivered `held` */

    /* Edges, true only on the update that produced them. Cleared by
     * button_fsm_take_*, so an edge is consumed by whoever reads it first. */
    bool    pressed;
    bool    released;
    bool    held;
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
bool button_fsm_take_held(button_fsm_t *b);
