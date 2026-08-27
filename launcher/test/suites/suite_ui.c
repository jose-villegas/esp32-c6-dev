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

void run_ui_suite(void)
{
    RUN_TEST(test_an_unchanged_ui_is_not_repainted);
    RUN_TEST(test_only_the_canvas_that_changed_repaints);
    RUN_TEST(test_a_canvas_is_repainted_when_something_draws_over_it);
    RUN_TEST(test_invalidate_forces_a_repaint);
}

SUITE_REGISTER(run_ui_suite);
