/*=============================================================================
 * Device suite: the UI layer's canvas model.
 *
 * The claim under test is the one an app relies on: a UI that has not changed
 * costs nothing, and a UI made of several windows repaints only the windows
 * that actually changed.
 *
 * Device-only because it needs the real framebuffer and gfx's band tracking -
 * "which parts of the screen would be sent" is the thing being measured, and
 * that is not a question a host can answer.
 *
 * Windows here are deliberately non-overlapping. Overlapping ones must repaint
 * together (painter's order), which is correct but would hide the independence
 * these tests exist to prove.
 *===========================================================================*/

#include "unity.h"
#include "suites.h"

#include "gfx/gfx.h"
#include "ui/ui.h"

/* Two separate canvases, well apart, each inside its own band. */
#define TOP_Y      0
#define TOP_H      64
#define BOTTOM_Y   256
#define BOTTOM_H   64

static input_t no_touch;

/* Builds both windows. `bottom_label` is what varies between frames. */
static void build(const char *bottom_label)
{
    mu_Context *ctx = ui_context();

    ui_begin(&no_touch);

    if (mu_begin_window_ex(ctx, "top", mu_rect(0, TOP_Y, GFX_WIDTH, TOP_H),
                           MU_OPT_NOTITLE | MU_OPT_NORESIZE |
                           MU_OPT_NOCLOSE  | MU_OPT_NOFRAME)) {
        mu_layout_row(ctx, 1, (int[]){ -1 }, 0);
        mu_text(ctx, "steady");
        mu_end_window(ctx);
    }

    if (mu_begin_window_ex(ctx, "bottom",
                           mu_rect(0, BOTTOM_Y, GFX_WIDTH, BOTTOM_H),
                           MU_OPT_NOTITLE | MU_OPT_NORESIZE |
                           MU_OPT_NOCLOSE  | MU_OPT_NOFRAME)) {
        mu_layout_row(ctx, 1, (int[]){ -1 }, 0);
        mu_text(ctx, bottom_label);
        mu_end_window(ctx);
    }
}

/* Paint both windows and get the screen to a known, fully-sent state. */
static void settle(const char *bottom_label)
{
    build(bottom_label);
    ui_end(0x000000);
    gfx_present();          /* clears every dirty band */
}

static void fixture(void)
{
    no_touch = (input_t){ 0 };
    ui_init();
    gfx_clear_clip();
}

static void test_an_unchanged_ui_is_not_repainted(void)
{
    fixture();
    settle("one");

    build("one");
    const bool drew = ui_end(0x000000);

    TEST_ASSERT_FALSE_MESSAGE(drew,
        "an immediate-mode UI is REBUILT every frame but not necessarily "
        "CHANGED - repainting one that looks identical throws away the whole "
        "point of tracking dirty bands");
    TEST_ASSERT_FALSE_MESSAGE(gfx_region_dirty(0, 0, GFX_WIDTH, GFX_HEIGHT),
        "and nothing may be queued for the panel either");
}

static void test_only_the_canvas_that_changed_repaints(void)
{
    fixture();
    settle("one");

    build("two");                    /* only the bottom window differs */
    const bool drew = ui_end(0x000000);

    TEST_ASSERT_TRUE_MESSAGE(drew, "a changed UI must repaint");
    TEST_ASSERT_TRUE_MESSAGE(gfx_region_dirty(0, BOTTOM_Y, GFX_WIDTH, BOTTOM_H),
        "the window whose contents changed must be repainted");
    TEST_ASSERT_FALSE_MESSAGE(gfx_region_dirty(0, TOP_Y, GFX_WIDTH, TOP_H),
        "the window that did not change must be left alone - this is what "
        "separate canvases buy, and without it a live readout would force a "
        "static toolbar to repaint with it");
}

static void test_a_canvas_is_repainted_when_something_draws_over_it(void)
{
    fixture();
    settle("one");

    /* An app drawing underneath an overlay. The UI's description has not
     * changed, but its pixels are gone. */
    gfx_fill_rect(0, TOP_Y, GFX_WIDTH, TOP_H, gfx_rgb(0x804020));

    build("one");
    const bool drew = ui_end(UI_NO_BACKGROUND);

    TEST_ASSERT_TRUE_MESSAGE(drew,
        "a UI whose pixels were overwritten must repaint even though its own "
        "description is unchanged");
}

static void test_invalidate_forces_a_repaint(void)
{
    fixture();
    settle("one");

    ui_invalidate();

    build("one");
    TEST_ASSERT_TRUE_MESSAGE(ui_end(0x000000),
        "ui_invalidate is how the shell says the framebuffer no longer holds "
        "this UI - returning to the launcher after an app has been running");
}

/*---------------------------------------------------------------------------
 * ui_layout_generation()
 *
 * Device-only for the same reason the rest of this suite is: the counter
 * lives inside ui_set_transform(), in ui.c, which pulls in gfx.h and cannot
 * link on a host - see ui.h's own comment above ui_layout_generation() and
 * this task's report on why suite_ui_transform.c (host, pure ui_transform_t
 * math) is not where this belongs. fixture() above already gives each test
 * a freshly ui_init()'d context, transform reset to identity and the
 * generation reset to its defined starting value - these tests read deltas
 * off that rather than hard-coding the starting value itself, so they do
 * not need to know or care what it is.
 *-------------------------------------------------------------------------*/

static void test_layout_generation_unchanged_by_a_repeated_equal_transform(void)
{
    fixture();
    ui_set_transform(ui_transform_identity());
    const uint32_t gen = ui_layout_generation();

    ui_set_transform(ui_transform_identity());
    ui_set_transform(ui_transform_identity());

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(gen, ui_layout_generation(),
        "setting the transform already in force must not bump the generation "
        "- this is the same transforms_equal() check ui_set_transform() uses "
        "to decide whether to call ui_invalidate()");
}

static void test_layout_generation_increments_by_one_per_genuine_change(void)
{
    fixture();
    ui_set_transform(ui_transform_identity());
    const uint32_t before = ui_layout_generation();

    ui_set_transform(ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT));

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before + 1, ui_layout_generation(),
        "a transform that genuinely differs from the one in force must bump "
        "the generation by exactly one");
}

static void test_layout_generation_counts_a_sequence_of_genuine_changes(void)
{
    fixture();
    ui_set_transform(ui_transform_identity());
    const uint32_t start = ui_layout_generation();

    /* Four calls, three of which are genuine changes - the repeat of turn 1
     * must not double-count, the same property the first test above checks
     * in isolation, now inside a longer sequence. */
    ui_set_transform(ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT));
    ui_set_transform(ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT));
    ui_set_transform(ui_transform_quarter_turn(2, GFX_WIDTH, GFX_HEIGHT));
    ui_set_transform(ui_transform_quarter_turn(3, GFX_WIDTH, GFX_HEIGHT));

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(start + 3, ui_layout_generation(),
        "three genuine changes among four calls must bump the generation "
        "exactly three times, not four and not fewer");
}

/*---------------------------------------------------------------------------
 * ui_begin_screen()
 *
 * The bug this reproduces: mu_begin_window_ex() only trusts its rect
 * argument the FIRST time a given window (by title) is ever opened -
 * `if (cnt->rect.w == 0) { cnt->rect = rect; }` in microui.c - and remembers
 * it forever after, which is correct for a desktop window manager and wrong
 * here, where every full-screen window in this shell is meant to always BE
 * (0, 0, ui_width(), ui_height()) for the frame currently being built. Once
 * ui_width()/ui_height() can change at runtime (a quarter-turn transform),
 * a window opened once in one orientation and revisited in a different,
 * larger one keeps the SMALLER, stale rect - which is exactly why
 * repaint_marked_canvases() in ui.c then clears too little of the screen
 * and leaves a previous app's pixels sitting in whatever the old rect
 * didn't cover.
 *
 * Device-only for the same reason the rest of this suite is: it needs a real
 * mu_Container living in the real container pool across multiple frames and
 * multiple ui_set_transform() calls, which is exactly the kind of stateful,
 * multi-frame behaviour suite_ui_transform.c's pure ui_transform_rect() math
 * is not shaped to exercise.
 *-------------------------------------------------------------------------*/

/* Opens and immediately closes one full-screen window via ui_begin_screen(),
 * simulating one visit to it in whatever orientation `quarter` selects
 * (0 = identity/portrait-native, 1 = a quarter turn - see
 * ui_transform_quarter_turn()'s own comment for the domain/viewport split). */
static void visit_screen(const char *title, int quarter)
{
    mu_Context *ctx = ui_context();

    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
    ui_begin(&no_touch);
    if (ui_begin_screen(ctx, title, MU_OPT_NOTITLE | MU_OPT_NORESIZE |
                                    MU_OPT_NOCLOSE  | MU_OPT_NOFRAME)) {
        mu_end_window(ctx);
    }
    ui_end(0x000000);
}

static void test_ui_begin_screen_corrects_a_stale_rect_from_a_prior_orientation(void)
{
    fixture();

    /* First-ever visit to this title, in one orientation - this is the call
     * that would seed cnt->rect under plain mu_begin_window_ex(), and pin it
     * there for good. */
    visit_screen("Reused Screen", 0);
    const int portrait_w = ui_width();
    const int portrait_h = ui_height();

    /* Same title, different orientation - a quarter turn swaps ui_width()/
     * ui_height(). Without ui_begin_screen()'s correction, the container's
     * rect would still read the portrait dimensions from the first visit. */
    visit_screen("Reused Screen", 1);
    const int landscape_w = ui_width();
    const int landscape_h = ui_height();

    TEST_ASSERT_NOT_EQUAL_MESSAGE(portrait_w, landscape_w,
        "the test needs a real dimension swap between visits, or it proves "
        "nothing");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(portrait_h, landscape_h,
        "same check for height - a quarter turn must swap both");

    mu_Context *ctx = ui_context();
    mu_Container *cnt = mu_get_container(ctx, "Reused Screen");

    TEST_ASSERT_EQUAL_MESSAGE(landscape_w, cnt->rect.w,
        "the container's rect must track THIS frame's logical canvas width, "
        "not whatever width was current the first time this title was ever "
        "opened");
    TEST_ASSERT_EQUAL_MESSAGE(landscape_h, cnt->rect.h,
        "same for height - a stale rect here is precisely what leaves part "
        "of a rotated screen uncleared");
}

/*---------------------------------------------------------------------------
 * repaint_marked_canvases() under an odd quarter
 *
 * A second, independent bug in the same neighbourhood as the stale-rect one
 * above, and easy to mistake for the same fix having missed a spot: even
 * with cnt->rect correctly tracking ui_width()/ui_height() every frame (see
 * ui_begin_screen() above), the background clear in repaint_marked_canvases()
 * used to pass that LOGICAL rect straight to gfx_fill_rect() with no
 * transform - unlike every command draw_command() paints, which all go
 * through ui_transform_rect() first. Under an odd quarter (Landscape or
 * Landscape upside down, where GFX_WIDTH != GFX_HEIGHT makes the logical and
 * physical canvases different shapes, not just different orientations) an
 * unrotated (0, 0, 448, 368) clipped onto a 368x448 physical framebuffer
 * covers only its first 368 of 448 rows. The remaining 80 physical rows
 * never got cleared - reported on hardware as pieces of the previous screen
 * stuck in place after rotating from Portrait to Landscape. */
static void test_repaint_clears_every_physical_row_under_an_odd_quarter(void)
{
    fixture();

    ui_set_transform(ui_transform_quarter_turn(1, GFX_WIDTH, GFX_HEIGHT));
    ui_begin(&no_touch);
    mu_Context *ctx = ui_context();
    if (ui_begin_screen(ctx, "Landscape Screen", MU_OPT_NOTITLE |
                                    MU_OPT_NORESIZE | MU_OPT_NOCLOSE |
                                    MU_OPT_NOFRAME)) {
        mu_end_window(ctx);
    }
    ui_end(0x000000);

    TEST_ASSERT_TRUE_MESSAGE(
        gfx_region_dirty(0, GFX_HEIGHT - 1, GFX_WIDTH, 1),
        "a full-screen window's background clear must reach the physical "
        "panel's last row too - clearing the untransformed logical rect "
        "instead silently stops 80 rows short of it under Landscape, "
        "leaving whatever the previous screen left there on screen");
}

void run_ui_suite(void)
{
    RUN_TEST(test_an_unchanged_ui_is_not_repainted);
    RUN_TEST(test_only_the_canvas_that_changed_repaints);
    RUN_TEST(test_a_canvas_is_repainted_when_something_draws_over_it);
    RUN_TEST(test_invalidate_forces_a_repaint);
    RUN_TEST(test_layout_generation_unchanged_by_a_repeated_equal_transform);
    RUN_TEST(test_layout_generation_increments_by_one_per_genuine_change);
    RUN_TEST(test_layout_generation_counts_a_sequence_of_genuine_changes);
    RUN_TEST(test_ui_begin_screen_corrects_a_stale_rect_from_a_prior_orientation);
    RUN_TEST(test_repaint_clears_every_physical_row_under_an_odd_quarter);
}

SUITE_REGISTER(run_ui_suite);
