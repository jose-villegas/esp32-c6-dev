/*=============================================================================
 * Portable suite: sand_ui - the falling-sand app's UI state machine.
 *
 * Four of these tests each pin a bug that shipped to hardware before this
 * logic could be host-tested at all - see sand_ui.h's own top comment for
 * the shape they share. Those are marked below with the commit that fixed
 * them (or, for the one still unfixed on this branch until now, the commit
 * that reported it). The rest exercise the ordinary behaviour a refactor
 * this close to four shipped bugs cannot afford to get wrong either.
 *===========================================================================*/

#include <string.h>

#include "unity.h"
#include "suites.h"

#include "sand_ui.h"
#include "palette.h"

/* --- fixture --------------------------------------------------------------
 *
 * A small stub brush table, independent of app_sand.c's real one, but built
 * from the same real materials so material_can_emit() exercises the real
 * eligibility rule rather than a fake one: MAT_SAND and MAT_WATER are
 * KIND_POWDER/KIND_LIQUID (emit-capable), MAT_STONE is KIND_STATIC (not) -
 * see material.c. */
#define STUB_BRUSH_COUNT 4
static const cell_t stub_brushes[STUB_BRUSH_COUNT] = {
    CELL_MAKE(MAT_SAND, 0),    /* 0: emits */
    CELL_MAKE(MAT_STONE, 0),   /* 1: static, cannot emit */
    CELL_MAKE(MAT_WATER, 0),   /* 2: emits */
    CELL_MAKE(MAT_STONE, 0),   /* 3: static, cannot emit */
};

static uint8_t stub_modes[STUB_BRUSH_COUNT];

static void fixture(sand_ui_t *ui)
{
    memset(stub_modes, BRUSH_POUR, sizeof stub_modes);
    *ui = (sand_ui_t){
        .brushes      = stub_brushes,
        .modes        = stub_modes,
        .brush_count  = STUB_BRUSH_COUNT,
        .screen       = SAND_UI_RUNNING,
        .brush        = 0,
        .erasing      = false,
        .swallow_release = false,
        .opened_brush = 0,
        .opened_mode  = BRUSH_POUR,
    };
}

/* A frame with nothing happening - every edge false, touch up. Tests build
 * on this rather than zero-initialising input_t themselves, so a field
 * neither this suite nor input_t itself has thought about yet still starts
 * from an explicit, known value. */
static input_t no_input(void)
{
    const input_t in = { 0 };
    return in;
}

/* Where tile `index` of the stub panel is, in the middle of its own rect -
 * so a test can aim a touch at a specific tile without hand-deriving
 * PALETTE_TILE geometry itself. Uses the real palette_hit()/palette_tile_rect()
 * pair, so a test tapping "tile 2" is exercising the same arithmetic
 * app_sand.c's real panel uses, at STUB_BRUSH_COUNT rather than BRUSH_COUNT. */
static void tile_center(int index, int *cx, int *cy)
{
    int x, y, w, h;
    palette_tile_rect(index, STUB_BRUSH_COUNT, &x, &y, &w, &h);
    *cx = x + w / 2;
    *cy = y + h / 2;
}

/* A point guaranteed to hit no tile at all - see palette_hit()'s own
 * contract for negative coordinates. */
static void outside_every_tile(int *px, int *py)
{
    *px = -5;
    *py = -5;
}

/* =====================================================================
 * The four shipped bugs
 * ===================================================================== */

/* Bug: a BOOT hold also cycled the brush, because cycling sat on
 * `.pressed` (and therefore fired again on every frame the hold's own
 * `.pressed` had already latched true, before `.held` even existed to
 * name the difference). sand_ui_step() never reads input->boot.held at
 * all in SAND_UI_RUNNING - see its own comment - so a hold is
 * structurally unable to change anything here, however long it lasts. */
static void test_a_boot_hold_in_running_changes_nothing_at_all(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.brush = 2;
    ui.erasing = true;

    input_t in = no_input();
    in.boot.held = true;   /* released and pressed both false, as button_fsm
                            * guarantees once a press has become a hold */

    const unsigned actions = sand_ui_step(&ui, &in);

    TEST_ASSERT_EQUAL_UINT(0, actions);
    TEST_ASSERT_EQUAL_INT(SAND_UI_RUNNING, ui.screen);
    TEST_ASSERT_EQUAL_INT(2, ui.brush);
    TEST_ASSERT_TRUE(ui.erasing);
}

/* Bug (commit faad9bb): closing the palette on `.pressed` split one
 * physical BOOT press across two screens - the panel closed on the press
 * edge, and the matching release arrived a frame later with screen already
 * back to RUNNING, where the (since-removed) cycling code read it as a
 * request to advance the brush. The fix, still in force here, is that
 * closing only ever happens on `.released` - a `.pressed` while the panel
 * is open must do nothing at all, and the brush must come through a close
 * completely untouched regardless. */
static void test_closing_the_palette_leaves_brush_exactly_as_it_was(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.brush = 2;
    ui.screen = SAND_UI_PALETTE;
    ui.opened_brush = 2;
    ui.opened_mode = BRUSH_POUR;

    /* A BOOT press, not a release, while the panel is open: must close
     * nothing - this is the exact edge faad9bb's bug closed on. */
    input_t press = no_input();
    press.boot.pressed = true;
    const unsigned press_actions = sand_ui_step(&ui, &press);

    TEST_ASSERT_EQUAL_UINT(0, press_actions);
    TEST_ASSERT_EQUAL_INT(SAND_UI_PALETTE, ui.screen);
    TEST_ASSERT_EQUAL_INT(2, ui.brush);

    /* Only the matching release actually closes it, and the brush is
     * exactly what it was - close_palette() never assigns to ui->brush. */
    input_t release = no_input();
    release.boot.released = true;
    const unsigned close_actions = sand_ui_step(&ui, &release);

    TEST_ASSERT_TRUE(close_actions & SAND_UI_CLOSE_PALETTE);
    TEST_ASSERT_EQUAL_INT(SAND_UI_RUNNING, ui.screen);
    TEST_ASSERT_EQUAL_INT(2, ui.brush);
}

/* Bug (commit eef97e4): the swallow-release guard armed unconditionally,
 * so with no finger down when the panel opened there was nothing to
 * swallow - and the flag ate the player's first genuine tap on a tile
 * instead, leaving the panel silently unresponsive until a second tap.
 * Opening with the finger already UP must leave a tap free to select. */
static void test_opening_with_no_finger_down_then_tapping_a_tile_selects_that_tile(void)
{
    sand_ui_t ui;
    fixture(&ui);

    input_t boot_release = no_input();
    boot_release.boot.released = true;
    boot_release.down = false;     /* no finger on the glass as BOOT lifts */
    const unsigned open_actions = sand_ui_step(&ui, &boot_release);

    TEST_ASSERT_TRUE(open_actions & SAND_UI_OPEN_PALETTE);
    TEST_ASSERT_EQUAL_INT(SAND_UI_PALETTE, ui.screen);
    TEST_ASSERT_FALSE(ui.swallow_release);

    int cx, cy;
    tile_center(2, &cx, &cy);
    input_t tap = no_input();
    tap.released = true;
    tap.x = cx;
    tap.y = cy;
    const unsigned tap_actions = sand_ui_step(&ui, &tap);

    TEST_ASSERT_TRUE(tap_actions & SAND_UI_REDRAW_PALETTE);
    TEST_ASSERT_EQUAL_INT(2, ui.brush);
}

/* The case the swallow guard exists FOR: a pour already in progress when
 * BOOT is released leaves that touch's own release still outstanding, and
 * without swallowing it, it would land on whatever tile happens to be
 * under the finger the instant the panel appears - see
 * open_palette()/handle_palette_input()'s own comments in sand_ui.c. */
static void test_opening_with_a_finger_already_down_then_lifting_selects_nothing(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.brush = 0;

    input_t boot_release = no_input();
    boot_release.boot.released = true;
    boot_release.down = true;      /* a pour is in progress as BOOT lifts */
    const unsigned open_actions = sand_ui_step(&ui, &boot_release);

    TEST_ASSERT_TRUE(open_actions & SAND_UI_OPEN_PALETTE);
    TEST_ASSERT_TRUE(ui.swallow_release);

    /* The dangling touch's own release, landing squarely on a tile - it
     * must still select nothing. */
    int cx, cy;
    tile_center(3, &cx, &cy);
    input_t lift = no_input();
    lift.released = true;
    lift.x = cx;
    lift.y = cy;
    const unsigned lift_actions = sand_ui_step(&ui, &lift);

    TEST_ASSERT_EQUAL_UINT(0, lift_actions);
    TEST_ASSERT_EQUAL_INT(0, ui.brush);
    TEST_ASSERT_FALSE(ui.swallow_release);   /* consumed, not still armed */
}

/* The family invariant every one of the above is a special case of: a press
 * whose release arrives after the screen has already changed underneath it
 * must be consumed exactly once, by the state that owns it - never left for
 * a second state to also act on, and never silently duplicated. Here the
 * touch starts in SAND_UI_RUNNING (which has no release consumer of its own
 * to begin with) and its release lands in SAND_UI_PALETTE, where the
 * swallow guard is the sole consumer. */
static void test_a_release_arriving_after_the_screen_changed_is_consumed_exactly_once(void)
{
    sand_ui_t ui;
    fixture(&ui);

    /* Frame 1: a touch begins while RUNNING. RUNNING has nothing that
     * reacts to a touch press or release at all - only PWR and BOOT are
     * read there - so this is silently ignored, as it must be. */
    input_t press = no_input();
    press.down = true;
    press.pressed = true;
    const unsigned press_actions = sand_ui_step(&ui, &press);
    TEST_ASSERT_EQUAL_UINT(0, press_actions);
    TEST_ASSERT_EQUAL_INT(SAND_UI_RUNNING, ui.screen);

    /* Frame 2: BOOT is released with that same touch still down - the
     * screen changes out from under the touch, and the guard arms because
     * a release is now genuinely owed. */
    input_t boot_release = no_input();
    boot_release.down = true;
    boot_release.boot.released = true;
    const unsigned open_actions = sand_ui_step(&ui, &boot_release);
    TEST_ASSERT_TRUE(open_actions & SAND_UI_OPEN_PALETTE);
    TEST_ASSERT_EQUAL_INT(SAND_UI_PALETTE, ui.screen);
    TEST_ASSERT_TRUE(ui.swallow_release);

    /* Frame 3: the touch's own release finally arrives, now that the
     * screen is SAND_UI_PALETTE. It is consumed here - and only here. */
    input_t release = no_input();
    release.released = true;
    int cx, cy;
    tile_center(1, &cx, &cy);
    release.x = cx;
    release.y = cy;
    const unsigned release_actions = sand_ui_step(&ui, &release);

    TEST_ASSERT_EQUAL_UINT(0, release_actions);   /* swallowed, not a selection */
    TEST_ASSERT_FALSE(ui.swallow_release);        /* consumed exactly once */
    TEST_ASSERT_EQUAL_INT(0, ui.brush);           /* RUNNING never saw it either */
}

/* =====================================================================
 * Ordinary behaviour
 * ===================================================================== */

static void test_tapping_a_different_tile_selects_it_and_preserves_its_mode(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_PALETTE;
    ui.brush = 0;
    ui.erasing = true;             /* selecting a material must clear this */
    ui.modes[2] = BRUSH_SPAWN;     /* tile 2's own remembered mode */

    int cx, cy;
    tile_center(2, &cx, &cy);
    input_t tap = no_input();
    tap.released = true;
    tap.x = cx;
    tap.y = cy;

    const unsigned actions = sand_ui_step(&ui, &tap);

    TEST_ASSERT_TRUE(actions & SAND_UI_REDRAW_PALETTE);
    TEST_ASSERT_EQUAL_INT(2, ui.brush);
    TEST_ASSERT_EQUAL_UINT8(BRUSH_SPAWN, ui.modes[2]);   /* untouched */
    TEST_ASSERT_FALSE(ui.erasing);
}

static void test_tapping_the_selected_tile_toggles_pour_and_spawn(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_PALETTE;
    ui.brush = 0;                  /* MAT_SAND: emit-capable */
    ui.erasing = true;             /* a toggle is not a selection - must stay */
    ui.modes[0] = BRUSH_POUR;

    int cx, cy;
    tile_center(0, &cx, &cy);
    input_t tap = no_input();
    tap.released = true;
    tap.x = cx;
    tap.y = cy;

    const unsigned actions = sand_ui_step(&ui, &tap);

    TEST_ASSERT_TRUE(actions & SAND_UI_REDRAW_PALETTE);
    TEST_ASSERT_EQUAL_INT(0, ui.brush);
    TEST_ASSERT_EQUAL_UINT8(BRUSH_SPAWN, ui.modes[0]);
    TEST_ASSERT_TRUE(ui.erasing);  /* untouched by a toggle */

    /* And back again. */
    const unsigned actions2 = sand_ui_step(&ui, &tap);
    TEST_ASSERT_TRUE(actions2 & SAND_UI_REDRAW_PALETTE);
    TEST_ASSERT_EQUAL_UINT8(BRUSH_POUR, ui.modes[0]);
}

static void test_tapping_the_selected_tile_when_it_cannot_emit_does_nothing(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_PALETTE;
    ui.brush = 1;                  /* MAT_STONE: KIND_STATIC, cannot emit */
    ui.modes[1] = BRUSH_POUR;

    int cx, cy;
    tile_center(1, &cx, &cy);
    input_t tap = no_input();
    tap.released = true;
    tap.x = cx;
    tap.y = cy;

    const unsigned actions = sand_ui_step(&ui, &tap);

    TEST_ASSERT_EQUAL_UINT(0, actions);
    TEST_ASSERT_EQUAL_INT(1, ui.brush);
    TEST_ASSERT_EQUAL_UINT8(BRUSH_POUR, ui.modes[1]);
}

static void test_tapping_outside_every_tile_does_nothing(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_PALETTE;
    ui.brush = 1;
    ui.erasing = true;

    int px, py;
    outside_every_tile(&px, &py);
    input_t tap = no_input();
    tap.released = true;
    tap.x = px;
    tap.y = py;

    const unsigned actions = sand_ui_step(&ui, &tap);

    TEST_ASSERT_EQUAL_UINT(0, actions);
    TEST_ASSERT_EQUAL_INT(1, ui.brush);
    TEST_ASSERT_TRUE(ui.erasing);
}

static void test_pwr_toggles_erase_and_requests_the_label(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_RUNNING;
    ui.erasing = false;

    input_t in = no_input();
    in.power.pressed = true;

    const unsigned actions = sand_ui_step(&ui, &in);

    TEST_ASSERT_TRUE(actions & SAND_UI_SHOW_LABEL);
    TEST_ASSERT_TRUE(ui.erasing);

    const unsigned actions2 = sand_ui_step(&ui, &in);
    TEST_ASSERT_TRUE(actions2 & SAND_UI_SHOW_LABEL);
    TEST_ASSERT_FALSE(ui.erasing);
}

static void test_closing_without_changing_anything_requests_no_label(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.brush = 1;
    ui.modes[1] = BRUSH_POUR;
    ui.screen = SAND_UI_PALETTE;
    ui.opened_brush = 1;
    ui.opened_mode = BRUSH_POUR;

    input_t close = no_input();
    close.boot.released = true;

    const unsigned actions = sand_ui_step(&ui, &close);

    TEST_ASSERT_TRUE(actions & SAND_UI_CLOSE_PALETTE);
    TEST_ASSERT_FALSE(actions & SAND_UI_SHOW_LABEL);
}

static void test_closing_after_selecting_a_different_tile_requests_the_label(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.brush = 0;
    ui.screen = SAND_UI_PALETTE;
    ui.opened_brush = 0;
    ui.opened_mode = BRUSH_POUR;

    int cx, cy;
    tile_center(2, &cx, &cy);
    input_t tap = no_input();
    tap.released = true;
    tap.x = cx;
    tap.y = cy;
    sand_ui_step(&ui, &tap);
    TEST_ASSERT_EQUAL_INT(2, ui.brush);

    input_t close = no_input();
    close.boot.released = true;
    const unsigned actions = sand_ui_step(&ui, &close);

    TEST_ASSERT_TRUE(actions & SAND_UI_CLOSE_PALETTE);
    TEST_ASSERT_TRUE(actions & SAND_UI_SHOW_LABEL);
}

static void test_closing_after_toggling_the_selected_tiles_mode_requests_the_label(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.brush = 0;                  /* MAT_SAND: emit-capable */
    ui.modes[0] = BRUSH_POUR;
    ui.screen = SAND_UI_PALETTE;
    ui.opened_brush = 0;
    ui.opened_mode = BRUSH_POUR;

    int cx, cy;
    tile_center(0, &cx, &cy);
    input_t tap = no_input();
    tap.released = true;
    tap.x = cx;
    tap.y = cy;
    sand_ui_step(&ui, &tap);
    TEST_ASSERT_EQUAL_UINT8(BRUSH_SPAWN, ui.modes[0]);

    input_t close = no_input();
    close.boot.released = true;
    const unsigned actions = sand_ui_step(&ui, &close);

    TEST_ASSERT_TRUE(actions & SAND_UI_CLOSE_PALETTE);
    TEST_ASSERT_TRUE(actions & SAND_UI_SHOW_LABEL);
}

void run_sand_ui_suite(void)
{
    RUN_TEST(test_a_boot_hold_in_running_changes_nothing_at_all);
    RUN_TEST(test_closing_the_palette_leaves_brush_exactly_as_it_was);
    RUN_TEST(test_opening_with_no_finger_down_then_tapping_a_tile_selects_that_tile);
    RUN_TEST(test_opening_with_a_finger_already_down_then_lifting_selects_nothing);
    RUN_TEST(test_a_release_arriving_after_the_screen_changed_is_consumed_exactly_once);

    RUN_TEST(test_tapping_a_different_tile_selects_it_and_preserves_its_mode);
    RUN_TEST(test_tapping_the_selected_tile_toggles_pour_and_spawn);
    RUN_TEST(test_tapping_the_selected_tile_when_it_cannot_emit_does_nothing);
    RUN_TEST(test_tapping_outside_every_tile_does_nothing);
    RUN_TEST(test_pwr_toggles_erase_and_requests_the_label);
    RUN_TEST(test_closing_without_changing_anything_requests_no_label);
    RUN_TEST(test_closing_after_selecting_a_different_tile_requests_the_label);
    RUN_TEST(test_closing_after_toggling_the_selected_tiles_mode_requests_the_label);
}

SUITE_REGISTER(run_sand_ui_suite);
