/*=============================================================================
 * Portable suite: ui_pointer driving REAL microui.
 *
 * WHY THIS EXISTS, AND WHY suite_ui_pointer.c WAS NOT ENOUGH
 *
 * That suite asserts the event LIST ui_pointer_step() produces. It was green
 * for a build in which no button in the shell could be pressed at all: every
 * app became unreachable from the launcher, because the events were fed to
 * microui in an order microui cannot resolve a click from. A list of events
 * is not a click; only microui decides that.
 *
 * The mechanism, so nobody re-derives it from scratch: mu_mouse_over() needs
 * in_hover_root(), and mu_begin() copies hover_root from the PREVIOUS frame's
 * next_hover_root. mu_update_control() then marks a control hovered only when
 * the mouse is over it AND mouse_down is clear, and focus is only taken from
 * a control that is already hovered. So a DOWN fed before hover has settled
 * lands with nothing hovered, nothing takes focus, and mu_button() renders a
 * pressed frame while returning 0 forever.
 *
 * microui.c is plain C over stdio/stdlib/string, so it links here unchanged -
 * this is the only suite that links it, and the reason the "nothing here
 * links microui.c" note in run_tests.sh no longer holds.
 *===========================================================================*/

#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "microui.h"
#include "ui/ui_pointer.h"

#define CANVAS_W 368
#define CANVAS_H 448

/* A button big enough that no rounding puts the touch point outside it. */
#define BTN_X 40
#define BTN_Y 80
#define BTN_W 280
#define BTN_H 64

/* Heap, not a file-scope object: a mu_Context is 10,744 bytes, and the
 * diagnostics build links every suite into firmware, where the
 * framebuffer plus one sand grid still have to fit (check_static_ram.py).
 * A second context in .bss fails that gate outright - which host tests,
 * with a laptop's memory behind them, cannot notice. Allocated once and
 * reset per test rather than per-test malloc/free: the runner has no
 * teardown hook to free it in. */
static mu_Context *ctx;
static ui_pointer_t pointer;

/* microui measures text through the context; the real shell hands it a font
 * atlas, and nothing here cares how wide a glyph is. */
static int stub_text_width(mu_Font font, const char *str, int len)
{
    (void)font;
    return (len < 0 ? (int)strlen(str) : len) * 8;
}

static int stub_text_height(mu_Font font)
{
    (void)font;
    return 8;
}

static void fixture(void)
{
    if (ctx == NULL) {
        ctx = malloc(sizeof *ctx);
        TEST_ASSERT_NOT_NULL(ctx);
    }
    memset(ctx, 0, sizeof *ctx);
    memset(&pointer, 0, sizeof pointer);
    mu_init(ctx);
    ctx->text_width  = stub_text_width;
    ctx->text_height = stub_text_height;
}

/* One frame of the real bridge: translate input_t exactly as ui.c's
 * feed_input() does, then build a full-screen window holding one button.
 * Returns whether the button submitted this frame. */
static bool frame(bool down, bool pressed, bool released, int x, int y)
{
    input_t in = { 0 };
    in.down = down;
    in.pressed = pressed;
    in.released = released;
    in.x = x;
    in.y = y;

    ui_pointer_event_t ev[UI_POINTER_MAX_EVENTS];
    const int n = ui_pointer_step(&pointer, &in, ev, UI_POINTER_MAX_EVENTS);
    for (int i = 0; i < n; i++) {
        switch (ev[i].kind) {
        case UI_POINTER_MOVE:
            mu_input_mousemove(ctx, ev[i].x, ev[i].y);
            break;
        case UI_POINTER_DOWN:
            mu_input_mousedown(ctx, ev[i].x, ev[i].y, MU_MOUSE_LEFT);
            break;
        case UI_POINTER_UP:
            mu_input_mouseup(ctx, ev[i].x, ev[i].y, MU_MOUSE_LEFT);
            break;
        }
    }

    bool submitted = false;
    mu_begin(ctx);
    if (mu_begin_window_ex(ctx, "screen", mu_rect(0, 0, CANVAS_W, CANVAS_H),
                           MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE |
                           MU_OPT_NOFRAME)) {
        mu_layout_set_next(ctx, mu_rect(BTN_X, BTN_Y, BTN_W, BTN_H), 0);
        if (mu_button(ctx, "GO")) {
            submitted = true;
        }
        mu_end_window(ctx);
    }
    mu_end(ctx);
    return submitted;
}

/* Frames with nothing touching, so the pointer parks off-screen exactly as
 * it does whenever a finger is not on the glass. */
static void idle_frames(int count)
{
    for (int i = 0; i < count; i++) {
        frame(false, false, false, 0, 0);
    }
}

/* A tap as touch_fsm actually delivers one: a pressed edge, some frames of
 * being held, then a released edge. */
static int taps_counted(int held_frames)
{
    const int cx = BTN_X + BTN_W / 2;
    const int cy = BTN_Y + BTN_H / 2;
    int submits = 0;

    if (frame(true, true, false, cx, cy)) { submits++; }
    for (int i = 0; i < held_frames; i++) {
        if (frame(true, false, false, cx, cy)) { submits++; }
    }
    if (frame(false, false, true, cx, cy)) { submits++; }
    return submits;
}

/*---------------------------------------------------------------------------
 * The regression this suite was written for.
 *-------------------------------------------------------------------------*/

static void test_a_tap_submits_the_button_underneath_it(void)
{
    fixture();
    idle_frames(2);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, taps_counted(4),
        "a tap must submit the button under it exactly once - this is the "
        "assertion a held-DOWN policy broke while every event-list test "
        "stayed green, leaving no app reachable from the launcher");
}

/* However long the finger rests, one press is one click. Holding must not
 * re-fire the control it is resting on. */
static void test_holding_does_not_resubmit(void)
{
    fixture();
    idle_frames(2);

    TEST_ASSERT_EQUAL_INT(1, taps_counted(40));
}

/* A press and release arriving in the SAME frame cannot click anything, and
 * that is a property of microui rather than a bug here: hover_root only
 * exists from the frame after the pointer first moves somewhere, so the
 * very first frame at a position can never resolve a control. The old
 * same-frame-release policy had this hole too - it is not something the
 * hover frames introduced.
 *
 * Pinned rather than left undiscovered: ui_pointer_step() still emits the
 * full move/down/up (suite_ui_pointer.c asserts that), so nothing is left
 * dangling, the click is simply not resolvable. touch_fsm only produces
 * this if a whole TOUCH_RELEASE_QUIET_US (60ms) of silence fits inside one
 * frame, so it needs a frame longer than the release debounce. If it ever
 * shows up in practice, the fix is to stage the tap across the hover frames
 * and emit the UP after the DOWN rather than with it. */
static void test_a_one_frame_tap_cannot_resolve_a_control(void)
{
    fixture();
    idle_frames(2);

    const int cx = BTN_X + BTN_W / 2;
    const int cy = BTN_Y + BTN_H / 2;
    TEST_ASSERT_FALSE_MESSAGE(frame(true, true, true, cx, cy),
        "microui has no hover_root for a position it is seeing for the "
        "first time, so nothing can take focus on that frame");
}

static void test_a_tap_outside_the_button_submits_nothing(void)
{
    fixture();
    idle_frames(2);

    int submits = 0;
    if (frame(true, true, false, 10, 400)) { submits++; }
    for (int i = 0; i < 4; i++) {
        if (frame(true, false, false, 10, 400)) { submits++; }
    }
    if (frame(false, false, true, 10, 400)) { submits++; }

    TEST_ASSERT_EQUAL_INT(0, submits);
}

/*---------------------------------------------------------------------------
 * The capability the held pointer exists for.
 *-------------------------------------------------------------------------*/

/* A slider needs mouse_down to persist ACROSS frames - microui only tracks
 * its value while (mouse_down | mouse_pressed) is set. This is what the
 * whole hold policy was introduced for, and it must keep working alongside
 * the hover frames the fix added. */
static void test_a_drag_moves_a_slider_microui_would_not_track_on_a_tap(void)
{
    fixture();
    idle_frames(2);

    mu_Real value = 0;
    const int track_x = BTN_X;
    const int track_w = BTN_W;

    /* Same shape as frame() above, but the control is a slider so the drag
     * has something that only responds while genuinely held. */
    int x = track_x + 10;
    for (int f = 0; f < 12; f++) {
        input_t in = { 0 };
        in.down = true;
        in.pressed = (f == 0);
        in.x = x;
        in.y = BTN_Y + BTN_H / 2;

        ui_pointer_event_t ev[UI_POINTER_MAX_EVENTS];
        const int n = ui_pointer_step(&pointer, &in, ev, UI_POINTER_MAX_EVENTS);
        for (int i = 0; i < n; i++) {
            if (ev[i].kind == UI_POINTER_MOVE) {
                mu_input_mousemove(ctx, ev[i].x, ev[i].y);
            } else if (ev[i].kind == UI_POINTER_DOWN) {
                mu_input_mousedown(ctx, ev[i].x, ev[i].y, MU_MOUSE_LEFT);
            } else {
                mu_input_mouseup(ctx, ev[i].x, ev[i].y, MU_MOUSE_LEFT);
            }
        }

        mu_begin(ctx);
        if (mu_begin_window_ex(ctx, "screen", mu_rect(0, 0, CANVAS_W, CANVAS_H),
                               MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE |
                               MU_OPT_NOFRAME)) {
            mu_layout_set_next(ctx, mu_rect(track_x, BTN_Y, track_w, BTN_H), 0);
            mu_slider(ctx, &value, 0, 100);
            mu_end_window(ctx);
        }
        mu_end(ctx);

        /* Start dragging only once the press has actually landed, so the
         * movement is a drag and not a series of separate taps. */
        if (f >= UI_POINTER_HOVER_FRAMES) {
            x += 15;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(value > 0,
        "dragging must move a slider - microui only tracks one while the "
        "mouse stays down, which is the reason the pointer holds DOWN at all");
}

void run_ui_pointer_microui_suite(void)
{
    RUN_TEST(test_a_tap_submits_the_button_underneath_it);
    RUN_TEST(test_holding_does_not_resubmit);
    RUN_TEST(test_a_one_frame_tap_cannot_resolve_a_control);
    RUN_TEST(test_a_tap_outside_the_button_submits_nothing);
    RUN_TEST(test_a_drag_moves_a_slider_microui_would_not_track_on_a_tap);
}

SUITE_REGISTER(run_ui_pointer_microui_suite);
