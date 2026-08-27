#include "input/button_fsm.h"

void button_fsm_reset(button_fsm_t *b)
{
    b->stable        = false;
    b->candidate     = false;
    b->candidate_us  = 0;
    b->primed        = false;
    b->down_since_us = 0;
    b->hold_fired    = false;
    b->pressed       = false;
    b->released      = false;
    b->held          = false;
}

void button_fsm_update(button_fsm_t *b, bool raw_down, int64_t now_us)
{
    if (!b->primed) {
        /* Adopt the first sample rather than assuming released. A button held
         * down as the app starts would otherwise fire a phantom press the
         * moment it was let go. */
        b->stable        = raw_down;
        b->candidate     = raw_down;
        b->candidate_us  = now_us;
        b->primed        = true;
        b->down_since_us = now_us;
        /* Same reasoning as the phantom-press decision above: a button
         * already held at startup should not fire a phantom hold either, so
         * treat it as a press that has already delivered its `held` edge -
         * there just wasn't a debounced press edge to time it from. */
        b->hold_fired    = raw_down;
        return;
    }

    /* A button held continuously down sits right here: candidate == stable,
     * so every early return below this point takes it and reports "nothing
     * changing" - which is correct for pressed/released, but would silently
     * swallow `held` forever if the check lived after them. It must run
     * before all of those early returns instead, even though "after the
     * other checks" looks like the natural place to put it. */
    if (b->stable && !b->hold_fired &&
        now_us - b->down_since_us >= BUTTON_HOLD_US) {
        b->held       = true;
        b->hold_fired = true;
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
        b->pressed       = true;
        b->down_since_us = now_us;
        b->hold_fired    = false;
    } else if (!b->hold_fired) {
        /* Only a press that stayed short gets a release edge. One that grew
         * into a hold already told the caller everything via `held`; a
         * trailing `released` would be a second event for one physical
         * press, and the whole point of this contract is that callers never
         * have to guard against that themselves. */
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

bool button_fsm_take_held(button_fsm_t *b)
{
    const bool edge = b->held;
    b->held = false;
    return edge;
}
