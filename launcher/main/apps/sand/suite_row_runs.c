/*=============================================================================
 * Portable suite: row_runs - detecting and reconciling separate runs of
 * occupied cells within one row.
 *
 * The reconciliation half (row_runs_reconcile()) is the highest-risk part
 * of this whole module - getting it wrong means a real, visible bug
 * (stale pixels left on screen), not just a missed optimisation. These
 * tests lean adversarial on purpose: a blob splitting, two blobs merging,
 * a blob vanishing entirely, a new blob appearing in what used to be a
 * gap - the same shape of case that was previously only ever exercised by
 * whatever happened to fall out of a real pour on real hardware.
 *===========================================================================*/

#include <stdbool.h>

#include "unity.h"
#include "suites.h"

#include "row_runs.h"

#define EMPTY 0
#define FULL  1

/* --- row_runs_find -------------------------------------------------------- */

static void test_find_reports_one_run_for_one_contiguous_blob(void)
{
    uint8_t row[10] = { 0, 0, 1, 1, 1, 1, 0, 0, 0, 0 };
    int x0[ROW_MAX_RUNS], x1[ROW_MAX_RUNS];

    const int n = row_runs_find(row, 10, EMPTY, x0, x1);

    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(2, x0[0]);
    TEST_ASSERT_EQUAL_INT(6, x1[0]);
}

static void test_find_reports_two_runs_for_two_separate_blobs(void)
{
    uint8_t row[10] = { 1, 1, 0, 0, 0, 0, 1, 1, 1, 0 };
    int x0[ROW_MAX_RUNS], x1[ROW_MAX_RUNS];

    const int n = row_runs_find(row, 10, EMPTY, x0, x1);

    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_INT(0, x0[0]);
    TEST_ASSERT_EQUAL_INT(2, x1[0]);
    TEST_ASSERT_EQUAL_INT(6, x0[1]);
    TEST_ASSERT_EQUAL_INT(9, x1[1]);
}

static void test_find_reports_nothing_for_an_empty_row(void)
{
    uint8_t row[6] = { 0, 0, 0, 0, 0, 0 };
    int x0[ROW_MAX_RUNS], x1[ROW_MAX_RUNS];

    TEST_ASSERT_EQUAL_INT(0, row_runs_find(row, 6, EMPTY, x0, x1));
}

static void test_find_reports_one_run_for_a_fully_occupied_row(void)
{
    uint8_t row[6] = { 1, 1, 1, 1, 1, 1 };
    int x0[ROW_MAX_RUNS], x1[ROW_MAX_RUNS];

    const int n = row_runs_find(row, 6, EMPTY, x0, x1);

    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(0, x0[0]);
    TEST_ASSERT_EQUAL_INT(6, x1[0]);
}

static void test_find_gives_up_past_the_cap(void)
{
    /* ROW_MAX_RUNS + 1 separate single-cell blobs, each isolated by a gap -
     * one more than this module ever tries to track individually. Built
     * from ROW_MAX_RUNS rather than a hardcoded blob count, so this stays
     * the right boundary case regardless of the cap's tuned value. */
    const int width = 2 * (ROW_MAX_RUNS + 1) + 1;
    uint8_t row[2 * (ROW_MAX_RUNS + 1) + 1];
    for (int i = 0; i <= ROW_MAX_RUNS; i++) {
        row[2 * i]     = FULL;
        row[2 * i + 1] = EMPTY;
    }
    row[width - 1] = EMPTY;
    int x0[ROW_MAX_RUNS], x1[ROW_MAX_RUNS];

    TEST_ASSERT_EQUAL_INT(-1, row_runs_find(row, width, EMPTY, x0, x1));
}

/* --- row_runs_span_fallback ------------------------------------------------ */

static void test_span_fallback_covers_everything_non_empty(void)
{
    uint8_t row[10] = { 0, 1, 0, 0, 0, 0, 1, 0, 0, 0 };
    int x0, x1;

    row_runs_span_fallback(row, 10, EMPTY, &x0, &x1);

    TEST_ASSERT_EQUAL_INT(1, x0);
    TEST_ASSERT_EQUAL_INT(7, x1);
}

static void test_span_fallback_reports_empty_range_for_an_empty_row(void)
{
    uint8_t row[5] = { 0, 0, 0, 0, 0 };
    int x0, x1;

    row_runs_span_fallback(row, 5, EMPTY, &x0, &x1);

    TEST_ASSERT_EQUAL_INT(5, x0);
    TEST_ASSERT_EQUAL_INT(0, x1);
}

/* --- row_runs_reconcile ---------------------------------------------------- */

/* True if every index in [x0,x1) is covered by at least one of the n
 * [send_x0[i],send_x1[i]) ranges - the general correctness property that
 * actually matters: nothing that was occupied, before or after, may be
 * left outside every send range, or its pixels never reach the panel. */
static bool all_covered(int x0, int x1, const uint16_t *send_x0,
                        const uint16_t *send_x1, int n)
{
    for (int x = x0; x < x1; x++) {
        bool covered = false;
        for (int i = 0; i < n; i++) {
            if (x >= send_x0[i] && x < send_x1[i]) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            return false;
        }
    }
    return true;
}

static void test_reconcile_unions_a_current_run_with_the_overlapping_previous_one(void)
{
    /* The plain single-range case: what a single previous/current union
     * always did before per-run reporting existed. */
    const uint16_t cur_x0[]  = { 10 }, cur_x1[]  = { 20 };
    const uint16_t prev_x0[] = { 5 },  prev_x1[] = { 15 };
    uint16_t send_x0[2], send_x1[2];

    const int n = row_runs_reconcile(cur_x0, cur_x1, 1, prev_x0, prev_x1, 1,
                                     send_x0, send_x1);

    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(5, send_x0[0]);
    TEST_ASSERT_EQUAL_INT(20, send_x1[0]);
}

static void test_reconcile_keeps_two_non_overlapping_runs_separate(void)
{
    const uint16_t cur_x0[]  = { 0, 50 },  cur_x1[]  = { 5, 55 };
    const uint16_t prev_x0[] = { 0, 50 },  prev_x1[] = { 5, 55 };
    uint16_t send_x0[4], send_x1[4];

    const int n = row_runs_reconcile(cur_x0, cur_x1, 2, prev_x0, prev_x1, 2,
                                     send_x0, send_x1);

    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_INT(0, send_x0[0]);
    TEST_ASSERT_EQUAL_INT(5, send_x1[0]);
    TEST_ASSERT_EQUAL_INT(50, send_x0[1]);
    TEST_ASSERT_EQUAL_INT(55, send_x1[1]);
}

static void test_reconcile_still_sends_a_run_that_vanished_entirely(void)
{
    /* Nothing current at all - the row went completely empty. The old
     * material's pixels still have to be cleared, or they are stuck. */
    const uint16_t prev_x0[] = { 10 }, prev_x1[] = { 20 };
    uint16_t send_x0[1], send_x1[1];

    const int n = row_runs_reconcile(NULL, NULL, 0, prev_x0, prev_x1, 1,
                                     send_x0, send_x1);

    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(10, send_x0[0]);
    TEST_ASSERT_EQUAL_INT(20, send_x1[0]);
}

static void test_reconcile_sends_a_genuinely_new_blob_untouched(void)
{
    /* Nothing previous at all - the row was completely empty and a fresh
     * blob just landed. Its own tight box, nothing extended. */
    const uint16_t cur_x0[] = { 30 }, cur_x1[] = { 40 };
    uint16_t send_x0[1], send_x1[1];

    const int n = row_runs_reconcile(cur_x0, cur_x1, 1, NULL, NULL, 0,
                                     send_x0, send_x1);

    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(30, send_x0[0]);
    TEST_ASSERT_EQUAL_INT(40, send_x1[0]);
}

static void test_reconcile_handles_a_blob_splitting_into_two(void)
{
    /* Previous: one run spanning [0,30). Current: the middle emptied out,
     * leaving two separate runs, [0,10) and [20,30). Every previously- or
     * currently-occupied index must still be covered by some send range,
     * so the vacated middle actually gets redrawn (as background) rather
     * than left showing stale material. */
    const uint16_t cur_x0[]  = { 0, 20 }, cur_x1[]  = { 10, 30 };
    const uint16_t prev_x0[] = { 0 },     prev_x1[] = { 30 };
    uint16_t send_x0[3], send_x1[3];

    const int n = row_runs_reconcile(cur_x0, cur_x1, 2, prev_x0, prev_x1, 1,
                                     send_x0, send_x1);

    TEST_ASSERT_TRUE_MESSAGE(all_covered(0, 30, send_x0, send_x1, n),
        "every index the row occupied before or occupies now must be "
        "covered by at least one send range, or the vacated middle is "
        "left showing stale material");
}

static void test_reconcile_handles_two_blobs_merging_into_one(void)
{
    /* Previous: two separate runs, [0,10) and [20,30). Current: they have
     * grown together into one run spanning the old gap, [0,30). The single
     * current run must absorb both previous ones. */
    const uint16_t cur_x0[]  = { 0 },      cur_x1[]  = { 30 };
    const uint16_t prev_x0[] = { 0, 20 },  prev_x1[] = { 10, 30 };
    uint16_t send_x0[3], send_x1[3];

    const int n = row_runs_reconcile(cur_x0, cur_x1, 1, prev_x0, prev_x1, 2,
                                     send_x0, send_x1);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, n,
        "a run spanning both previous runs must absorb them both, not "
        "leave either as a separate leftover send");
    TEST_ASSERT_EQUAL_INT(0, send_x0[0]);
    TEST_ASSERT_EQUAL_INT(30, send_x1[0]);
}

static void test_reconcile_handles_a_new_blob_in_a_previously_empty_gap(void)
{
    /* Previous: one run, [0,10). Current: that same run, unchanged, plus a
     * brand new, non-overlapping one at [50,60) - real space away, not
     * touching the old run at all. Both must be covered; the new one must
     * not be silently dropped just because it has no previous counterpart. */
    const uint16_t cur_x0[]  = { 0, 50 }, cur_x1[]  = { 10, 60 };
    const uint16_t prev_x0[] = { 0 },     prev_x1[] = { 10 };
    uint16_t send_x0[3], send_x1[3];

    const int n = row_runs_reconcile(cur_x0, cur_x1, 2, prev_x0, prev_x1, 1,
                                     send_x0, send_x1);

    TEST_ASSERT_TRUE_MESSAGE(all_covered(0, 10, send_x0, send_x1, n),
        "the unchanged old run must still be covered");
    TEST_ASSERT_TRUE_MESSAGE(all_covered(50, 60, send_x0, send_x1, n),
        "the brand new blob must be covered too, not dropped for lack of "
        "a previous counterpart");
}

void run_row_runs_suite(void)
{
    RUN_TEST(test_find_reports_one_run_for_one_contiguous_blob);
    RUN_TEST(test_find_reports_two_runs_for_two_separate_blobs);
    RUN_TEST(test_find_reports_nothing_for_an_empty_row);
    RUN_TEST(test_find_reports_one_run_for_a_fully_occupied_row);
    RUN_TEST(test_find_gives_up_past_the_cap);

    RUN_TEST(test_span_fallback_covers_everything_non_empty);
    RUN_TEST(test_span_fallback_reports_empty_range_for_an_empty_row);

    RUN_TEST(test_reconcile_unions_a_current_run_with_the_overlapping_previous_one);
    RUN_TEST(test_reconcile_keeps_two_non_overlapping_runs_separate);
    RUN_TEST(test_reconcile_still_sends_a_run_that_vanished_entirely);
    RUN_TEST(test_reconcile_sends_a_genuinely_new_blob_untouched);
    RUN_TEST(test_reconcile_handles_a_blob_splitting_into_two);
    RUN_TEST(test_reconcile_handles_two_blobs_merging_into_one);
    RUN_TEST(test_reconcile_handles_a_new_blob_in_a_previously_empty_gap);
}

SUITE_REGISTER(run_row_runs_suite);
