/*=============================================================================
 * Portable suite: gfx_dirty - the grid/leaf dirty-region tracker.
 *
 * None of this was reachable from a host before gfx_dirty.h existed - it
 * lived inside gfx.c, which unconditionally includes ESP-IDF SPI headers.
 * suite_gfx.c (device-only) still covers whether the design is actually
 * cheaper to send; this suite covers whether the geometry and bitmask
 * logic underneath it is correct in the first place, which a timing
 * comparison on real hardware can never directly show. Off-by-one at a
 * LEAF_W/LEAF_H boundary is the likely bug class here, so several of these
 * lean on the exact boundary pixel rather than a value safely in the
 * middle of a leaf.
 *===========================================================================*/

#include "unity.h"
#include "suites.h"

#include "gfx_dirty.h"

/* gfx_dirty.h's state is static, so this file gets its own private copy -
 * exactly like a real device build's gfx.c does, and exactly what makes it
 * possible to reset and inspect directly here. */
static void fixture(void)
{
    cell_dirty = 0;
    all_dirty = false;
    for (int i = 0; i < CELL_COUNT; i++) {
        const int col = i % GRID_COLS;
        const int row = i / GRID_COLS;
        cell_x0[i] = (col + 1) * COL_WIDTH;
        cell_x1[i] = col * COL_WIDTH;
        cell_y0[i] = row * STRIP_HEIGHT + STRIP_HEIGHT;
        cell_y1[i] = row * STRIP_HEIGHT;
    }
    for (int i = 0; i < STRIP_COUNT * LEAF_SUB; i++) {
        leaf_dirty[i] = 0;
    }
}

/* --- leaf boundary math ---------------------------------------------------- */

static void test_mark_leaves_stays_in_the_leaf_before_a_boundary(void)
{
    fixture();
    dirty_mark(LEAF_W - 1, 0, 1, 1);   /* the last pixel of leaf column 0 */

    TEST_ASSERT_EQUAL_HEX16(0x0001, leaf_dirty[0]);
}

static void test_mark_leaves_moves_to_the_next_leaf_at_a_boundary(void)
{
    fixture();
    dirty_mark(LEAF_W, 0, 1, 1);       /* the first pixel of leaf column 1 */

    TEST_ASSERT_EQUAL_HEX16(0x0002, leaf_dirty[0]);
}

static void test_mark_leaves_sets_every_leaf_a_wide_box_spans(void)
{
    fixture();
    /* Spans leaf columns 0, 1 and part of 2 (0..49, LEAF_W=23: cols
     * [0,23) [23,46) [46,69)). */
    dirty_mark(0, 0, 50, 1);

    TEST_ASSERT_EQUAL_HEX16(0x0007, leaf_dirty[0]);
}

static void test_mark_leaves_spans_a_cell_boundary_too(void)
{
    fixture();
    /* COL_WIDTH=92=LEAF_W*4, so a box straddling x=92 crosses from the
     * last leaf of cell 0 (column 3) into the first leaf of cell 1
     * (column 4) - a boundary between LEAF_SUB groups, not just between
     * two leaves within one. */
    dirty_mark(COL_WIDTH - 1, 0, 2, 1);

    TEST_ASSERT_EQUAL_HEX16(0x0018, leaf_dirty[0]);   /* bits 3 and 4 */
}

/* --- eligibility ------------------------------------------------------------ */

static void test_run_is_leaf_eligible_when_every_cell_is_tight(void)
{
    fixture();
    dirty_mark(5, 0, 10, 10);
    dirty_mark(COL_WIDTH + 5, 0, 10, 10);

    TEST_ASSERT_TRUE(run_is_leaf_eligible(0, 0, 2));
}

static void test_run_is_leaf_eligible_false_if_any_cell_in_the_run_is_coarse(void)
{
    fixture();
    /* Cell 0's box exactly fills its own full extent - treated the same as
     * a mark_band() touch, per dirty_mark()'s own documented invariant. */
    dirty_mark(0, 0, COL_WIDTH, STRIP_HEIGHT);
    dirty_mark(COL_WIDTH + 5, 0, 10, 10);

    TEST_ASSERT_FALSE_MESSAGE(run_is_leaf_eligible(0, 0, 2),
        "one coarse cell must disable refinement for the whole run, not "
        "just that cell");
}

/* --- collect_runs_from_mask -------------------------------------------------
 *
 * Shared by the cell-level and leaf-level run finders in gfx.c - these
 * exercise it directly, at the bit-manipulation level, rather than only
 * indirectly through whichever caller happens to reach it. */

static void test_collect_runs_from_mask_finds_nothing_in_an_empty_mask(void)
{
    int s[4], e[4];

    TEST_ASSERT_EQUAL_INT(0, collect_runs_from_mask(0, 8, s, e, 4));
}

static void test_collect_runs_from_mask_finds_one_contiguous_run(void)
{
    int s[4], e[4];
    const int n = collect_runs_from_mask(0x3C /* 0b00111100 */, 8, s, e, 4);

    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(2, s[0]);
    TEST_ASSERT_EQUAL_INT(6, e[0]);
}

static void test_collect_runs_from_mask_separates_a_real_gap(void)
{
    int s[4], e[4];
    /* Bit 0 and bit 7 - opposite ends of the mask, nothing shared. */
    const int n = collect_runs_from_mask(0x81 /* 0b10000001 */, 8, s, e, 4);

    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_INT(0, s[0]);
    TEST_ASSERT_EQUAL_INT(1, e[0]);
    TEST_ASSERT_EQUAL_INT(7, s[1]);
    TEST_ASSERT_EQUAL_INT(8, e[1]);
}

static void test_collect_runs_from_mask_no_gap_is_one_run(void)
{
    int s[4], e[4];
    const int n = collect_runs_from_mask(0xFF, 8, s, e, 4);

    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(0, s[0]);
    TEST_ASSERT_EQUAL_INT(8, e[0]);
}

static void test_collect_runs_from_mask_fits_exactly_at_the_cap(void)
{
    int s[2], e[2];
    /* Two isolated single-bit runs, cap of exactly 2. */
    const int n = collect_runs_from_mask(0x05 /* 0b00000101 */, 8, s, e, 2);

    TEST_ASSERT_EQUAL_INT(2, n);
}

static void test_collect_runs_from_mask_gives_up_past_the_cap(void)
{
    int s[2], e[2];
    /* Four isolated single-bit runs, cap of 2 - the third one must trip
     * the "too fragmented" case. */
    const int n = collect_runs_from_mask(0x55 /* 0b01010101 */, 8, s, e, 2);

    TEST_ASSERT_EQUAL_INT(-1, n);
}

/* --- cell-level runs and boxes ----------------------------------------------
 *
 * collect_dirty_runs()/run_box() are the cell-granularity counterparts of
 * collect_runs_from_mask() above, and plan_run() is what decides whether a
 * run gets leaf-refined at all - exercised directly here rather than only
 * indirectly through gfx.c's send path, which a host build cannot reach. */

static void test_collect_dirty_runs_finds_two_separate_cell_runs(void)
{
    fixture();
    dirty_mark(5, 0, 10, 10);                 /* cell 0 */
    dirty_mark(3 * COL_WIDTH + 5, 0, 10, 10); /* cell 3 - opposite end */

    int run_start[GRID_COLS], run_end[GRID_COLS];
    const int n = collect_dirty_runs(0, run_start, run_end);

    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_INT(0, run_start[0]);
    TEST_ASSERT_EQUAL_INT(1, run_end[0]);
    TEST_ASSERT_EQUAL_INT(3, run_start[1]);
    TEST_ASSERT_EQUAL_INT(4, run_end[1]);
}

static void test_run_box_unions_every_cell_in_the_run(void)
{
    fixture();
    dirty_mark(5, 10, 10, 10);                  /* cell 0: x[5,15) y[10,20) */
    dirty_mark(COL_WIDTH + 20, 0, 5, 30);       /* cell 1: x[+20,+25) y[0,30) */

    int x0, x1, y0, y1;
    run_box(0, 0, 2, &x0, &x1, &y0, &y1);

    TEST_ASSERT_EQUAL_INT(5, x0);
    TEST_ASSERT_EQUAL_INT(COL_WIDTH + 25, x1);
    TEST_ASSERT_EQUAL_INT(0, y0);
    TEST_ASSERT_EQUAL_INT(30, y1);
}

static void test_plan_run_finds_a_real_gap_inside_one_cell(void)
{
    fixture();
    /* Two marks in the same cell, far enough apart to leave a real leaf
     * gap between them - the exact shape test_two_marks_in_one_cell_costs_
     * less_than_the_coarse_box exercises on real hardware in suite_gfx.c;
     * this is the same case at the logic level, host-side. */
    dirty_mark(5, 0, 5, 5);
    dirty_mark(70, 0, 5, 5);

    int sx0[LEAF_REFINE_MAX_RUNS], sx1[LEAF_REFINE_MAX_RUNS];
    const int n = plan_run(0, 0, 1, 0, 5, sx0, sx1);

    TEST_ASSERT_EQUAL_INT(2, n);
}

/* Mark width for test_plan_run_rejects_a_split_over_the_gather_budget:
 * derived from GATHER_MAX_PIXELS/STRIP_HEIGHT (plus a margin) rather than
 * hardcoded, so the "over budget" case it builds tracks whatever the
 * budget is tuned to. At today's 8192 this evaluates to 136 - the exact
 * value the test used before it was made parametric. Valid only while two
 * such marks, with a gap between them, still fit inside one 4-cell run -
 * the static assert below catches a GATHER_MAX_PIXELS large enough to
 * break that, rather than this test silently stopping to mean anything. */
#define OVER_BUDGET_MARK_W ((GATHER_MAX_PIXELS / STRIP_HEIGHT) + 8)

static void test_plan_run_rejects_a_split_over_the_gather_budget(void)
{
    _Static_assert(OVER_BUDGET_MARK_W < (GRID_COLS * COL_WIDTH) / 2 - 20,
        "GATHER_MAX_PIXELS is too large for two over-budget marks to both "
        "fit, with a real gap, inside one 4-cell run - this test's "
        "construction needs rethinking above this budget, not a bigger "
        "mark");
    fixture();
    /* Two wide marks at opposite ends of a 4-cell run, full strip height -
     * each half, once split, is over GATHER_MAX_PIXELS. Must fall back to
     * 0 (use the coarse box) rather than hand back a split
     * gather_and_send()'s fixed-size buffer cannot hold. */
    dirty_mark(1, 0, OVER_BUDGET_MARK_W, STRIP_HEIGHT);
    dirty_mark(GRID_COLS * COL_WIDTH - 1 - OVER_BUDGET_MARK_W, 0,
               OVER_BUDGET_MARK_W, STRIP_HEIGHT);

    int sx0[LEAF_REFINE_MAX_RUNS], sx1[LEAF_REFINE_MAX_RUNS];
    const int n = plan_run(0, 0, 4, 0, STRIP_HEIGHT, sx0, sx1);

    TEST_ASSERT_EQUAL_INT(0, n);
}

/* --- per-row reset, once a row has been sent -------------------------------- */

static void test_row_sent_clears_only_that_rows_leaves(void)
{
    fixture();
    dirty_mark(0, 0, 10, 10);                /* row 0's leaves */
    dirty_mark(0, STRIP_HEIGHT, 10, 10);     /* row 1's leaves */

    TEST_ASSERT_NOT_EQUAL(0, leaf_dirty[0]);
    TEST_ASSERT_NOT_EQUAL(0, leaf_dirty[LEAF_SUB]);

    dirty_row_sent(0);

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0, leaf_dirty[0],
        "the row that was just sent must have its leaves cleared");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, leaf_dirty[LEAF_SUB],
        "a stale bit from a past frame would make a genuinely fully dirty "
        "cell look like it has a gap that is not real, silently dropping "
        "pixels that do need sending - a DIFFERENT row must be untouched");
}

static void test_row_sent_resets_cell_boxes_to_empty(void)
{
    fixture();
    dirty_mark(5, 0, 10, 10);

    dirty_row_sent(0);

    TEST_ASSERT_TRUE_MESSAGE(cell_x0[0] > cell_x1[0],
        "next frame's union has to start from nothing, not keep growing "
        "the previous frame's box forever");
}

/* --- the all_dirty fast path ------------------------------------------------ */

static void test_mark_all_claims_every_row(void)
{
    fixture();
    dirty_mark_all();

    TEST_ASSERT_TRUE(dirty_row_is_dirty(0));
    TEST_ASSERT_TRUE(dirty_row_is_dirty(STRIP_COUNT - 1));
}

static void test_dirty_mark_is_a_noop_once_everything_is_already_claimed(void)
{
    fixture();
    dirty_mark_all();
    cell_x0[0] = 12345;   /* a value dirty_mark() must not touch */

    dirty_mark(5, 5, 10, 10);

    TEST_ASSERT_EQUAL_INT_MESSAGE(12345, cell_x0[0],
        "once the whole screen is already claimed, a further tight mark "
        "can only ever repeat work already done - it must not run at all");
}

void run_gfx_dirty_suite(void)
{
    RUN_TEST(test_mark_leaves_stays_in_the_leaf_before_a_boundary);
    RUN_TEST(test_mark_leaves_moves_to_the_next_leaf_at_a_boundary);
    RUN_TEST(test_mark_leaves_sets_every_leaf_a_wide_box_spans);
    RUN_TEST(test_mark_leaves_spans_a_cell_boundary_too);

    RUN_TEST(test_run_is_leaf_eligible_when_every_cell_is_tight);
    RUN_TEST(test_run_is_leaf_eligible_false_if_any_cell_in_the_run_is_coarse);

    RUN_TEST(test_collect_runs_from_mask_finds_nothing_in_an_empty_mask);
    RUN_TEST(test_collect_runs_from_mask_finds_one_contiguous_run);
    RUN_TEST(test_collect_runs_from_mask_separates_a_real_gap);
    RUN_TEST(test_collect_runs_from_mask_no_gap_is_one_run);
    RUN_TEST(test_collect_runs_from_mask_fits_exactly_at_the_cap);
    RUN_TEST(test_collect_runs_from_mask_gives_up_past_the_cap);

    RUN_TEST(test_collect_dirty_runs_finds_two_separate_cell_runs);
    RUN_TEST(test_run_box_unions_every_cell_in_the_run);
    RUN_TEST(test_plan_run_finds_a_real_gap_inside_one_cell);
    RUN_TEST(test_plan_run_rejects_a_split_over_the_gather_budget);

    RUN_TEST(test_row_sent_clears_only_that_rows_leaves);
    RUN_TEST(test_row_sent_resets_cell_boxes_to_empty);

    RUN_TEST(test_mark_all_claims_every_row);
    RUN_TEST(test_dirty_mark_is_a_noop_once_everything_is_already_claimed);
}

SUITE_REGISTER(run_gfx_dirty_suite);
