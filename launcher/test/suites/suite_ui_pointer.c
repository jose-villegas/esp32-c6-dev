/*=============================================================================
 * Portable suite: ui_pointer - input_t to move/down/up events, held not
 * tapped.
 *
 * ui.c's own feed_input() has no host coverage - suite_ui.c is device-only -
 * so this is the first place the touch-to-mouse bridge's policy is actually
 * asserted rather than eyeballed on a screenshot.
 *===========================================================================*/

#include <string.h>

#include "suites.h"
#include "unity.h"

#include "ui/ui_pointer.h"

static ui_pointer_t p;
static ui_pointer_event_t ev[UI_POINTER_MAX_EVENTS];

static void
fixture(void) {
    memset(&p, 0, sizeof(p));
    memset(ev, 0, sizeof(ev));
}

static input_t
make_input(bool down, bool pressed, bool released, int x, int y) {
    input_t in = {0};
    in.down = down;
    in.pressed = pressed;
    in.released = released;
    in.x = x;
    in.y = y;
    return in;
}

static int
step(bool down, bool pressed, bool released, int x, int y) {
    input_t in = make_input(down, pressed, released, x, y);
    return ui_pointer_step(&p, &in, ev, UI_POINTER_MAX_EVENTS);
}

/* Walk a press through every hover frame, leaving the NEXT step() as the
 * one that carries the DOWN. Written against UI_POINTER_HOVER_FRAMES rather
 * than a hardcoded count so a change to the policy shows up as one failing
 * assertion about the policy, not as several tests silently testing the
 * wrong thing. */
static void
press_through_hover(int x, int y) {
    for (int frame = 0; frame < UI_POINTER_HOVER_FRAMES; frame++) {
        step(true, frame == 0, false, x, y);
    }
}

/*-----------------------------------------------------------------------------
 * The synthesized hover frame - load-bearing, see ui.h's touch-to-mouse
 * comment. Lost, a touchscreen tap could never resolve into a click at all.
 *---------------------------------------------------------------------------*/

static void
test_a_tap_hovers_two_frames_before_pressing(void) {
    fixture();

    /* Both hover frames are MOVE-only, and both matter: the first tells
     * microui which window the finger is in (hover_root, which mu_begin()
     * copies from the previous frame), the second is the first frame that
     * can mark a control hovered. A DOWN before that lands with nothing
     * hovered, so nothing takes focus and no button ever submits - which
     * is exactly what shipped and made every app unreachable. */
    for (int frame = 0; frame < UI_POINTER_HOVER_FRAMES; frame++) {
        const int n = step(true, frame == 0, false, 10, 20);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, n, "a hover frame carries a move and nothing else");
        TEST_ASSERT_EQUAL_INT(UI_POINTER_MOVE, ev[0].kind);
        TEST_ASSERT_EQUAL_INT(10, ev[0].x);
        TEST_ASSERT_EQUAL_INT(20, ev[0].y);
    }

    const int n = step(true, false, false, 10, 20);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, n, "the down follows the hover frames, with its own move");
    TEST_ASSERT_EQUAL_INT(UI_POINTER_MOVE, ev[0].kind);
    TEST_ASSERT_EQUAL_INT(UI_POINTER_DOWN, ev[1].kind);
    TEST_ASSERT_EQUAL_INT(10, ev[1].x);
    TEST_ASSERT_EQUAL_INT(20, ev[1].y);
}

/*-----------------------------------------------------------------------------
 * Holding, not releasing - the whole point of this module.
 *---------------------------------------------------------------------------*/

static void
test_a_drag_stays_down_across_moves_then_lifts_once(void) {
    fixture();

    press_through_hover(10, 20);
    int n = step(true, false, false, 10, 20); /* the down */
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_INT(UI_POINTER_DOWN, ev[1].kind);

    n = step(true, false, false, 15, 25);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, n, "a held drag must move, not press again or let go");
    TEST_ASSERT_EQUAL_INT(UI_POINTER_MOVE, ev[0].kind);
    TEST_ASSERT_EQUAL_INT(15, ev[0].x);
    TEST_ASSERT_EQUAL_INT(25, ev[0].y);

    n = step(true, false, false, 30, 40);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(UI_POINTER_MOVE, ev[0].kind);
    TEST_ASSERT_EQUAL_INT(30, ev[0].x);
    TEST_ASSERT_EQUAL_INT(40, ev[0].y);

    n = step(false, false, true, 30, 40);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, n, "the lift must carry a move and an up");
    TEST_ASSERT_EQUAL_INT(UI_POINTER_MOVE, ev[0].kind);
    TEST_ASSERT_EQUAL_INT(UI_POINTER_UP, ev[1].kind);
}

static void
test_exactly_one_up_comes_out_of_one_press(void) {
    fixture();

    int ups = 0;
    int n;

    n = step(true, true, false, 5, 5);
    for (int i = 0; i < n; i++) {
        ups += ev[i].kind == UI_POINTER_UP;
    }
    n = step(true, false, false, 5, 5);
    for (int i = 0; i < n; i++) {
        ups += ev[i].kind == UI_POINTER_UP;
    }
    n = step(true, false, false, 6, 6);
    for (int i = 0; i < n; i++) {
        ups += ev[i].kind == UI_POINTER_UP;
    }
    n = step(false, false, true, 6, 6);
    for (int i = 0; i < n; i++) {
        ups += ev[i].kind == UI_POINTER_UP;
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, ups, "one press must yield one up, never zero or two");
}

/*-----------------------------------------------------------------------------
 * A tap fast enough to resolve inside one poll - the old bridge could never
 * lose this because it released the same frame it pressed; the new held
 * policy can, so it has to be handled explicitly.
 *---------------------------------------------------------------------------*/

static void
test_a_same_frame_tap_still_yields_move_down_up(void) {
    fixture();

    int n = step(true, true, true, 10, 20);
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, n, "a tap that presses and releases within one frame must not be swallowed");
    TEST_ASSERT_EQUAL_INT(UI_POINTER_MOVE, ev[0].kind);
    TEST_ASSERT_EQUAL_INT(UI_POINTER_DOWN, ev[1].kind);
    TEST_ASSERT_EQUAL_INT(UI_POINTER_UP, ev[2].kind);
}

/*-----------------------------------------------------------------------------
 * No phantom press for a finger that was already on the glass before the
 * UI ever asked about it.
 *---------------------------------------------------------------------------*/

static void
test_a_finger_already_down_at_open_synthesizes_no_press(void) {
    fixture();

    int n = step(true, false, false, 50, 60);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT_MESSAGE(UI_POINTER_MOVE, ev[0].kind,
                                  "a finger never seen pressed must not synthesize a down");

    n = step(true, false, false, 51, 61);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(UI_POINTER_MOVE, ev[0].kind);
}

static void
test_a_finger_already_down_at_open_then_released_emits_no_up(void) {
    fixture();

    step(true, false, false, 50, 60); /* already down at open: no DOWN was ever emitted */

    int n = step(false, false, true, 50, 60);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT_MESSAGE(UI_POINTER_MOVE, ev[0].kind,
                                  "a DOWN that was never emitted must not get a matching UP");
}

/*-----------------------------------------------------------------------------
 * The header promises never more than `max` - a too-small buffer must be
 * rejected outright, not partially filled, and must not touch state either
 * (a caller with a short buffer must not silently eat a press edge).
 *---------------------------------------------------------------------------*/

static void
test_a_too_small_buffer_returns_zero_and_leaves_state_untouched(void) {
    fixture();

    input_t in = make_input(true, true, false, 10, 20);
    int n = ui_pointer_step(&p, &in, ev, UI_POINTER_MAX_EVENTS - 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, n, "a buffer smaller than UI_POINTER_MAX_EVENTS must be rejected");

    /* Rejected, not partially applied: the same press replayed with a
     * proper buffer must still play out its normal hover-then-down. */
    press_through_hover(10, 20);

    n = step(true, false, false, 10, 20);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, n, "the DOWN must still follow, proving the rejected call left press_stage untouched");
    TEST_ASSERT_EQUAL_INT(UI_POINTER_DOWN, ev[1].kind);
}

static void
test_idle_parks_the_pointer_off_screen(void) {
    fixture();

    int n = step(false, false, false, 0, 0);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(UI_POINTER_MOVE, ev[0].kind);
    TEST_ASSERT_EQUAL_INT(-1, ev[0].x);
    TEST_ASSERT_EQUAL_INT(-1, ev[0].y);
}

void
run_ui_pointer_suite(void) {
    RUN_TEST(test_a_tap_hovers_two_frames_before_pressing);
    RUN_TEST(test_a_drag_stays_down_across_moves_then_lifts_once);
    RUN_TEST(test_exactly_one_up_comes_out_of_one_press);
    RUN_TEST(test_a_same_frame_tap_still_yields_move_down_up);
    RUN_TEST(test_a_finger_already_down_at_open_synthesizes_no_press);
    RUN_TEST(test_a_finger_already_down_at_open_then_released_emits_no_up);
    RUN_TEST(test_a_too_small_buffer_returns_zero_and_leaves_state_untouched);
    RUN_TEST(test_idle_parks_the_pointer_off_screen);
}

SUITE_REGISTER(run_ui_pointer_suite);
