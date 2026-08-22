#include "button_fsm.h"

void button_fsm_reset(button_fsm_t *b)
{
    b->stable       = false;
    b->candidate    = false;
    b->candidate_us = 0;
    b->primed       = false;
    b->pressed      = false;
    b->released     = false;
}

void button_fsm_update(button_fsm_t *b, bool raw_down, int64_t now_us)
{
    if (!b->primed) {
        /* Adopt the first sample rather than assuming released. A button held
         * down as the app starts would otherwise fire a phantom press the
         * moment it was let go. */
        b->stable       = raw_down;
        b->candidate    = raw_down;
        b->candidate_us = now_us;
        b->primed       = true;
        return;
    }

    if (raw_down != b->candidate) {
        /* A new level. Start its clock; it is not believed yet. */
        b->candidate    = raw_down;
        b->candidate_us = now_us;
        return;
    }

    if (b->candidate == b->stable) {
        return;                     /* nothing changing */
    }

    if (now_us - b->candidate_us < BUTTON_DEBOUNCE_US) {
        return;                     /* held, but not yet long enough */
    }

    /* The new level has outlasted any plausible bounce. */
    b->stable = b->candidate;
    if (b->stable) {
        b->pressed = true;
    } else {
        b->released = true;
    }
}

bool button_fsm_is_down(const button_fsm_t *b)
{
    return b->stable;
}

bool button_fsm_take_pressed(button_fsm_t *b)
{
    const bool edge = b->pressed;
    b->pressed = false;
    return edge;
}

bool button_fsm_take_released(button_fsm_t *b)
{
    const bool edge = b->released;
    b->released = false;
    return edge;
}
