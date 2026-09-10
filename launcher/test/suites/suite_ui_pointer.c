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

/*-----------------------------------------------------------------------------
 * The synthesized hover frame - load-bearing, see ui.h's touch-to-mouse
 * comment. Lost, a touchscreen tap could never resolve into a click at all.
 *---------------------------------------------------------------------------*/

static void
test_a_tap_hovers_one_frame_before_pressing(void) {
    fixture();

    int n = step(true, true, false, 10, 20);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(UI_POINTER_MOVE, ev[0].kind);
    TEST_ASSERT_EQUAL_INT(10, ev[0].x);
    TEST_ASSERT_EQUAL_INT(20, ev[0].y);

    n = step(true, false, false, 10, 20);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, n, "the frame after a press must carry a move and a down, nothing more");
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

    step(true, true, false, 10, 20);          /* hover frame */
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
    RUN_TEST(test_a_tap_hovers_one_frame_before_pressing);
    RUN_TEST(test_a_drag_stays_down_across_moves_then_lifts_once);
    RUN_TEST(test_exactly_one_up_comes_out_of_one_press);
    RUN_TEST(test_a_same_frame_tap_still_yields_move_down_up);
    RUN_TEST(test_a_finger_already_down_at_open_synthesizes_no_press);
    RUN_TEST(test_idle_parks_the_pointer_off_screen);
}

SUITE_REGISTER(run_ui_pointer_suite);
