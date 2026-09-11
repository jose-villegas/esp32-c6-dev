/*
 * Portable suite: sand_ui - the falling-sand app's UI state machine.
 *
 * Four of these tests each pin a bug that shipped to hardware because
 * this logic could not be host-tested before - see sand_ui.h's own top
 * comment for the shape they share; several are marked below with the
 * commit that fixed them. The rest exercise the ordinary behaviour a
 * refactor this close to four shipped bugs cannot afford to get wrong
 * either.
 */

#include <string.h>

#include "unity.h"
#include "suites.h"

#include "sand_ui.h"

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
        .mode         = SAND_MODE_PAINT,
        .swallow_release = false,
        .opened_brush = 0,
        .opened_mode  = BRUSH_POUR,
        .radius_px = { 0 },
        .opened_sand_mode = SAND_MODE_PAINT,
        .opened_radius = 0,
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

/* An out-of-bounds point, named so the test below reads as "a tap,
 * somewhere" rather than two magic numbers - see its own comment for why
 * the exact coordinates don't matter. */
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
    ui.mode = SAND_MODE_ERASE;

    input_t in = no_input();
    in.boot.held = true;   /* released and pressed both false, as button_fsm
                            * guarantees once a press has become a hold */

    const unsigned actions = sand_ui_step(&ui, &in);

    TEST_ASSERT_EQUAL_UINT(0, actions);
    TEST_ASSERT_EQUAL_INT(SAND_UI_RUNNING, ui.screen);
    TEST_ASSERT_EQUAL_INT(2, ui.brush);
    TEST_ASSERT_EQUAL_INT(SAND_MODE_ERASE, ui.mode);
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

    /* microui has already done its own hit-test by the time anything calls
     * this (see sand_ui.h's "WHO HIT-TESTS AND WHO DECIDES"), so this
     * drives sand_ui_tile_clicked() directly rather than feeding
     * sand_ui_step() a touch release at tile 2's coordinates. */
    const unsigned tap_actions = sand_ui_tile_clicked(&ui, 2);

    TEST_ASSERT_TRUE(tap_actions & SAND_UI_REDRAW_PALETTE);
    TEST_ASSERT_EQUAL_INT(2, ui.brush);
}

/* The case the swallow guard exists FOR: a pour already in progress when
 * BOOT is released leaves that touch's own release still outstanding, and
 * without swallowing it, it would land on whatever tile happens to be
 * under the finger the instant the panel appears - see
 * open_palette()/sand_ui_tile_clicked()'s own comments in sand_ui.c. */
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

    /* The dangling touch is still down, and the guard is still armed - a
     * click landing squarely on a tile during that window must still
     * select nothing. This is sand_ui_tile_clicked()'s own half of the
     * guard: it ignores the click outright while swallow_release is set,
     * never mind which tile it names. */
    const unsigned click_actions = sand_ui_tile_clicked(&ui, 3);
    TEST_ASSERT_EQUAL_UINT(0, click_actions);
    TEST_ASSERT_EQUAL_INT(0, ui.brush);

    /* The touch finally lifts - a genuine release, no finger down any more.
     * That is sand_ui_step()'s own half of the guard: disarming it. */
    input_t lift = no_input();
    lift.down = false;
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

    /* The dangling touch is still armed against - a click landing squarely
     * on tile 1 while the guard is up must be consumed here, and only
     * here: ignored, not read as a selection. */
    const unsigned click_actions = sand_ui_tile_clicked(&ui, 1);
    TEST_ASSERT_EQUAL_UINT(0, click_actions);
    TEST_ASSERT_EQUAL_INT(0, ui.brush);

    /* Frame 3: the touch's own release finally arrives, now that the
     * screen is SAND_UI_PALETTE - a genuine lift, which disarms the guard
     * for whatever tap comes next. */
    input_t release = no_input();
    release.down = false;
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
    ui.mode = SAND_MODE_ERASE;     /* selecting a material must clear this */
    ui.modes[2] = BRUSH_SPAWN;     /* tile 2's own remembered mode */

    const unsigned actions = sand_ui_tile_clicked(&ui, 2);

    TEST_ASSERT_TRUE(actions & SAND_UI_REDRAW_PALETTE);
    TEST_ASSERT_EQUAL_INT(2, ui.brush);
    TEST_ASSERT_EQUAL_UINT8(BRUSH_SPAWN, ui.modes[2]);   /* untouched */
    TEST_ASSERT_EQUAL_INT(SAND_MODE_PAINT, ui.mode);
}

/* The same reset, but starting from DETONATE rather than ERASE - the third
 * leg of the cycle needs its own check rather than trusting that "resets
 * to PAINT" generalises from the ERASE case above, since handle_brush_input()
 * cycles all three but handle_palette_input() only ever assigns PAINT
 * directly - nothing here proves it does that from EVERY starting mode
 * until it is actually exercised from each one. */
static void test_selecting_a_tile_while_detonating_resets_to_paint(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_PALETTE;
    ui.brush = 0;
    ui.mode = SAND_MODE_DETONATE;

    /* microui has already done its own hit-test by the time anything calls
     * this - see sand_ui.h's "WHO HIT-TESTS AND WHO DECIDES" comment, and
     * test_opening_with_no_finger_down_then_tapping_a_tile_selects_that_tile
     * above for the same pattern - so this drives the entry point a real
     * tap would resolve to directly, by tile index, rather than synthesizing
     * a touch release at some tile's own pixel centre. */
    const unsigned actions = sand_ui_tile_clicked(&ui, 2);

    TEST_ASSERT_TRUE(actions & SAND_UI_REDRAW_PALETTE);
    TEST_ASSERT_EQUAL_INT(2, ui.brush);
    TEST_ASSERT_EQUAL_INT(SAND_MODE_PAINT, ui.mode);
}

static void test_tapping_the_selected_tile_toggles_pour_and_spawn(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_PALETTE;
    ui.brush = 0;                  /* MAT_SAND: emit-capable */
    ui.mode = SAND_MODE_ERASE;     /* a toggle is not a selection - must stay */
    ui.modes[0] = BRUSH_POUR;

    const unsigned actions = sand_ui_tile_clicked(&ui, 0);

    TEST_ASSERT_TRUE(actions & SAND_UI_REDRAW_PALETTE);
    TEST_ASSERT_EQUAL_INT(0, ui.brush);
    TEST_ASSERT_EQUAL_UINT8(BRUSH_SPAWN, ui.modes[0]);
    TEST_ASSERT_EQUAL_INT(SAND_MODE_ERASE, ui.mode);  /* untouched by a toggle */

    /* And back again. */
    const unsigned actions2 = sand_ui_tile_clicked(&ui, 0);
    TEST_ASSERT_TRUE(actions2 & SAND_UI_REDRAW_PALETTE);
    TEST_ASSERT_EQUAL_UINT8(BRUSH_POUR, ui.modes[0]);
}

/* Same toggle, checked from DETONATE too - the same reasoning as
 * test_selecting_a_tile_while_detonating_resets_to_paint above: a toggle
 * leaving ERASE alone does not by itself prove it leaves DETONATE alone,
 * since handle_palette_input()'s toggle branch never touches `mode` at all
 * and that has to be checked from each starting mode, not assumed. */
static void test_tapping_the_selected_tile_is_untouched_by_detonate(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_PALETTE;
    ui.brush = 0;                  /* MAT_SAND: emit-capable */
    ui.mode = SAND_MODE_DETONATE;
    ui.modes[0] = BRUSH_POUR;

    /* Same reasoning as test_selecting_a_tile_while_detonating_resets_to_
     * paint just above - the tile index goes straight to sand_ui_tile_
     * clicked(), not through a synthesized touch release. */
    const unsigned actions = sand_ui_tile_clicked(&ui, 0);

    TEST_ASSERT_TRUE(actions & SAND_UI_REDRAW_PALETTE);
    TEST_ASSERT_EQUAL_UINT8(BRUSH_SPAWN, ui.modes[0]);
    TEST_ASSERT_EQUAL_INT(SAND_MODE_DETONATE, ui.mode);
}

static void test_tapping_the_selected_tile_when_it_cannot_emit_does_nothing(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_PALETTE;
    ui.brush = 1;                  /* MAT_STONE: KIND_STATIC, cannot emit */
    ui.modes[1] = BRUSH_POUR;

    const unsigned actions = sand_ui_tile_clicked(&ui, 1);

    TEST_ASSERT_EQUAL_UINT(0, actions);
    TEST_ASSERT_EQUAL_INT(1, ui.brush);
    TEST_ASSERT_EQUAL_UINT8(BRUSH_POUR, ui.modes[1]);
}

/* draw_palette()'s mu_button() per tile never returns true for a point
 * outside every tile (see sand_ui.h's "WHO HIT-TESTS AND WHO DECIDES"), so
 * sand_ui_tile_clicked() is never called for one. What this test pins:
 * sand_ui_step() never touches brush/erasing for anything but a BOOT edge
 * while the palette is open - selection only changes through
 * sand_ui_tile_clicked(). Driving it through sand_ui_step() with an
 * ordinary touch frame still exercises that guarantee; the coordinates
 * themselves do no work. */
static void test_tapping_outside_every_tile_does_nothing(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_PALETTE;
    ui.brush = 1;
    ui.mode = SAND_MODE_ERASE;

    int px, py;
    outside_every_tile(&px, &py);
    input_t tap = no_input();
    tap.released = true;
    tap.x = px;
    tap.y = py;

    const unsigned actions = sand_ui_step(&ui, &tap);

    TEST_ASSERT_EQUAL_UINT(0, actions);
    TEST_ASSERT_EQUAL_INT(1, ui.brush);
    TEST_ASSERT_EQUAL_INT(SAND_MODE_ERASE, ui.mode);
}

/* PWR used to cycle PAINT -> ERASE -> DETONATE -> PAINT directly (see git
 * history for that version of this test). It now opens the brush screen
 * instead, and leaves `mode` exactly as it found it - the segments decide
 * mode now, not PWR. */
static void test_pwr_from_running_opens_the_brush_screen_and_does_not_cycle_the_mode(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_RUNNING;
    ui.mode = SAND_MODE_PAINT;

    input_t in = no_input();
    in.power.pressed = true;

    const unsigned actions = sand_ui_step(&ui, &in);

    TEST_ASSERT_TRUE(actions & SAND_UI_OPEN_BRUSH);
    TEST_ASSERT_EQUAL_INT(SAND_UI_BRUSH, ui.screen);
    TEST_ASSERT_EQUAL_INT(SAND_MODE_PAINT, ui.mode);
}

/* The hazard this file's top comment warns about: PWR opens and closes on
 * the SAME kind of edge (buttons.h - PWR has no release), so the open must
 * not also resolve as a close within the very call that produced it. */
static void test_the_pwr_press_that_opens_the_brush_screen_does_not_also_close_it(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_RUNNING;

    input_t in = no_input();
    in.power.pressed = true;

    const unsigned actions = sand_ui_step(&ui, &in);

    TEST_ASSERT_TRUE(actions & SAND_UI_OPEN_BRUSH);
    TEST_ASSERT_FALSE(actions & SAND_UI_CLOSE_BRUSH);
    TEST_ASSERT_EQUAL_INT(SAND_UI_BRUSH, ui.screen);   /* survives the frame */
}

static void test_a_later_pwr_press_closes_the_brush_screen(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_BRUSH;
    ui.mode = SAND_MODE_ERASE;
    ui.opened_sand_mode = SAND_MODE_ERASE;
    ui.opened_radius = ui.radius_px[SAND_MODE_ERASE];

    input_t in = no_input();
    in.power.pressed = true;

    const unsigned actions = sand_ui_step(&ui, &in);

    TEST_ASSERT_TRUE(actions & SAND_UI_CLOSE_BRUSH);
    TEST_ASSERT_EQUAL_INT(SAND_UI_RUNNING, ui.screen);
}

static void test_a_segment_tap_sets_the_mode_and_the_selected_segment_is_harmless(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_BRUSH;
    ui.mode = SAND_MODE_PAINT;

    const unsigned actions = sand_ui_mode_clicked(&ui, (int)SAND_MODE_ERASE);
    TEST_ASSERT_TRUE(actions & SAND_UI_REDRAW_BRUSH);
    TEST_ASSERT_EQUAL_INT(SAND_MODE_ERASE, ui.mode);

    /* Tapping the segment already lit again - nothing left to change. */
    const unsigned actions2 = sand_ui_mode_clicked(&ui, (int)SAND_MODE_ERASE);
    TEST_ASSERT_EQUAL_UINT(0, actions2);
    TEST_ASSERT_EQUAL_INT(SAND_MODE_ERASE, ui.mode);
}

static void test_radius_is_remembered_per_mode(void)
{
    sand_ui_t ui;
    fixture(&ui);

    ui.mode = SAND_MODE_PAINT;
    sand_ui_set_radius(&ui, 20);
    TEST_ASSERT_EQUAL_UINT8(20, sand_ui_radius(&ui));

    /* Switching modes must not disturb ERASE's own (still-default) radius. */
    ui.mode = SAND_MODE_ERASE;
    TEST_ASSERT_EQUAL_UINT8(0, sand_ui_radius(&ui));
    sand_ui_set_radius(&ui, 40);
    TEST_ASSERT_EQUAL_UINT8(40, sand_ui_radius(&ui));

    /* And PAINT's own radius is exactly as it was left. */
    ui.mode = SAND_MODE_PAINT;
    TEST_ASSERT_EQUAL_UINT8(20, sand_ui_radius(&ui));
}

static void test_radius_clamps_at_both_ends_rather_than_wrapping(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.mode = SAND_MODE_DETONATE;

    sand_ui_set_radius(&ui, 0);
    TEST_ASSERT_EQUAL_UINT8(SAND_UI_RADIUS_MIN, sand_ui_radius(&ui));

    sand_ui_set_radius(&ui, 255);
    TEST_ASSERT_EQUAL_UINT8(SAND_UI_RADIUS_MAX, sand_ui_radius(&ui));
}

/* The brush screen's own version of
 * test_opening_with_a_finger_already_down_then_lifting_selects_nothing
 * above - same guard, same reason, a different panel. */
static void test_opening_the_brush_screen_with_a_finger_already_down_then_lifting_selects_nothing(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_RUNNING;
    ui.mode = SAND_MODE_PAINT;

    input_t press = no_input();
    press.power.pressed = true;
    press.down = true;     /* a pour in progress as PWR fires */
    const unsigned open_actions = sand_ui_step(&ui, &press);

    TEST_ASSERT_TRUE(open_actions & SAND_UI_OPEN_BRUSH);
    TEST_ASSERT_TRUE(ui.swallow_release);

    const unsigned click_actions = sand_ui_mode_clicked(&ui, (int)SAND_MODE_ERASE);
    TEST_ASSERT_EQUAL_UINT(0, click_actions);
    TEST_ASSERT_EQUAL_INT(SAND_MODE_PAINT, ui.mode);

    input_t lift = no_input();
    lift.down = false;
    const unsigned lift_actions = sand_ui_step(&ui, &lift);

    TEST_ASSERT_EQUAL_UINT(0, lift_actions);
    TEST_ASSERT_FALSE(ui.swallow_release);
}

/* The two panels are siblings, never both open - BOOT belongs to the
 * palette and must be inert while the brush screen has the floor. */
static void test_boot_while_the_brush_screen_is_open_does_nothing(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_BRUSH;
    ui.mode = SAND_MODE_ERASE;
    ui.brush = 2;

    input_t boot_press = no_input();
    boot_press.boot.pressed = true;
    const unsigned press_actions = sand_ui_step(&ui, &boot_press);
    TEST_ASSERT_EQUAL_UINT(0, press_actions);
    TEST_ASSERT_EQUAL_INT(SAND_UI_BRUSH, ui.screen);

    input_t boot_release = no_input();
    boot_release.boot.released = true;
    const unsigned release_actions = sand_ui_step(&ui, &boot_release);
    TEST_ASSERT_EQUAL_UINT(0, release_actions);
    TEST_ASSERT_EQUAL_INT(SAND_UI_BRUSH, ui.screen);
    TEST_ASSERT_EQUAL_INT(2, ui.brush);
}

static void test_closing_the_brush_screen_without_changing_anything_requests_no_label(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_BRUSH;
    ui.mode = SAND_MODE_ERASE;
    ui.radius_px[SAND_MODE_ERASE] = 16;
    ui.opened_sand_mode = SAND_MODE_ERASE;
    ui.opened_radius = 16;

    input_t close = no_input();
    close.power.pressed = true;
    const unsigned actions = sand_ui_step(&ui, &close);

    TEST_ASSERT_TRUE(actions & SAND_UI_CLOSE_BRUSH);
    TEST_ASSERT_FALSE(actions & SAND_UI_SHOW_LABEL);
}

static void test_closing_the_brush_screen_after_a_real_change_requests_the_label(void)
{
    sand_ui_t ui;
    fixture(&ui);
    ui.screen = SAND_UI_BRUSH;
    ui.mode = SAND_MODE_PAINT;
    ui.opened_sand_mode = SAND_MODE_PAINT;
    ui.opened_radius = ui.radius_px[SAND_MODE_PAINT];

    sand_ui_set_radius(&ui, 30);   /* the real change - radius, not mode */

    input_t close = no_input();
    close.power.pressed = true;
    const unsigned actions = sand_ui_step(&ui, &close);

    TEST_ASSERT_TRUE(actions & SAND_UI_CLOSE_BRUSH);
    TEST_ASSERT_TRUE(actions & SAND_UI_SHOW_LABEL);
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

    sand_ui_tile_clicked(&ui, 2);
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

    sand_ui_tile_clicked(&ui, 0);
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
    RUN_TEST(test_selecting_a_tile_while_detonating_resets_to_paint);
    RUN_TEST(test_tapping_the_selected_tile_toggles_pour_and_spawn);
    RUN_TEST(test_tapping_the_selected_tile_is_untouched_by_detonate);
    RUN_TEST(test_tapping_the_selected_tile_when_it_cannot_emit_does_nothing);
    RUN_TEST(test_tapping_outside_every_tile_does_nothing);
    RUN_TEST(test_closing_without_changing_anything_requests_no_label);
    RUN_TEST(test_closing_after_selecting_a_different_tile_requests_the_label);
    RUN_TEST(test_closing_after_toggling_the_selected_tiles_mode_requests_the_label);

    RUN_TEST(test_pwr_from_running_opens_the_brush_screen_and_does_not_cycle_the_mode);
    RUN_TEST(test_the_pwr_press_that_opens_the_brush_screen_does_not_also_close_it);
    RUN_TEST(test_a_later_pwr_press_closes_the_brush_screen);
    RUN_TEST(test_a_segment_tap_sets_the_mode_and_the_selected_segment_is_harmless);
    RUN_TEST(test_radius_is_remembered_per_mode);
    RUN_TEST(test_radius_clamps_at_both_ends_rather_than_wrapping);
    RUN_TEST(test_opening_the_brush_screen_with_a_finger_already_down_then_lifting_selects_nothing);
    RUN_TEST(test_boot_while_the_brush_screen_is_open_does_nothing);
    RUN_TEST(test_closing_the_brush_screen_without_changing_anything_requests_no_label);
    RUN_TEST(test_closing_the_brush_screen_after_a_real_change_requests_the_label);
}

SUITE_REGISTER(run_sand_ui_suite);
