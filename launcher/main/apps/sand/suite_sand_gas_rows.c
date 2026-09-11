/*
 * Portable suite: the gas spread pass may only skip a row that really holds
 * no gas.
 *
 * sand_gas.c's spread pass reads a per-row map the rise sweep fills in, and
 * skips any row whose bit is clear. Every mover in that sweep arms the row it
 * lands in; one that forgets leaves gas sitting in a row the spread pass has
 * already written off, which is a stall no scene assertion is likely to
 * notice - the cell is still there, still the right material, just never
 * spreading. sand_gas_row_audit_enable() (sand_priv.h) re-derives the map by
 * brute force where the skip is about to use it and counts the
 * disagreements; these tests turn it on and drive boards at it.
 *
 * WHY NOT A TRAVEL-DISTANCE TEST. The obvious assertion - gas never moves
 * more than one row a step, so a map widened by one row is safe - is false. A
 * walk drawing straight down lands in a row the rise sweep has not reached
 * yet and takes a second turn there, so one pass can carry a cell several
 * rows. Arming the landing row replaced that argument; auditing the result is
 * what checks it.
 */
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "suites.h"

#include "sand.h"
#include "sand_priv.h"
#include "suite_sand_common.h"

#define GASROW_W 48
#define GASROW_H 40

static uint8_t *gr_cells;
static sand_t   gr;

static void gas_rows_fixture(void)
{
    gr_cells = malloc((size_t)GASROW_W * (size_t)GASROW_H);
    TEST_ASSERT_NOT_NULL(gr_cells);
    sand_init(&gr, gr_cells, GASROW_W, GASROW_H, 4242u);
    sand_clear(&gr);
    sand_gas_row_audit_failures = 0;
    sand_gas_row_audit_enable(true);
}

static void gas_rows_teardown(void)
{
    sand_gas_row_audit_enable(false);
    free(gr_cells);
}

/* Walls, standing water and a scatter of gases - the rise sweep's paths
 * divide by what a grain is BLOCKED BY, so a board of open air exercises one
 * of them. Water gives try_bubble() and the walk's buoyancy fallback
 * something to push through, stone and wood something that refuses, and
 * several gases at once give the gas-displaces-gas swap a chance to run. */
static void build_mixed_gas_board(bool burnable)
{
    for (int x = 0; x < GASROW_W; x++) {
        sand_set(&gr, x, GASROW_H - 1, CELL_MAKE(MAT_STONE, 0));
    }
    for (int y = GASROW_H - 8; y < GASROW_H - 1; y++) {
        for (int x = 4; x < GASROW_W - 4; x++) {
            sand_set(&gr, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    for (int y = 6; y < GASROW_H - 10; y += 5) {
        for (int x = 2; x < GASROW_W - 2; x += 7) {
            sand_set(&gr, x, y, CELL_MAKE(MAT_STONE, 0));
            sand_set(&gr, x + 1, y,
                     CELL_MAKE(burnable ? MAT_WOOD : MAT_SAND, 0));
        }
    }
    for (int y = 3; y < GASROW_H - 3; y += 3) {
        for (int x = 1; x < GASROW_W - 1; x += 4) {
            const material_id_t id = burnable      ? MAT_FIRE
                                     : (y % 6 == 0) ? MAT_SMOKE
                                                    : MAT_GAS;
            if (CELL_IS_EMPTY(sand_at(&gr, x, y))) {
                sand_set(&gr, x, y, CELL_MAKE(id, MATERIAL_VARIANTS - 1));
            }
        }
    }
}

static void assert_no_stranded_gas(const char *why)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, sand_gas_row_audit_failures, why);
}

static void test_the_walk_arms_every_row_it_lands_in(void)
{
    gas_rows_fixture();
    build_mixed_gas_board(false);

    for (int i = 0; i < 80; i++) {
        sand_step(&gr, 0, 1, 0);
    }

    assert_no_stranded_gas(
        "the spread pass was about to skip a row that holds gas - a rise "
        "sweep mover moved a cell without arming the row it landed in");
    gas_rows_teardown();
}

/* A walk that draws straight down lands in a row the sweep has not reached,
 * takes a second turn there and can do it again, which is the case a fixed
 * widening of the map cannot cover. Straight down is 24 of 256 draws, so this
 * runs long enough for the cascade to happen many times over. */
static void test_a_downward_walk_cascade_arms_every_row(void)
{
    gas_rows_fixture();

    for (int x = 0; x < GASROW_W; x++) {
        sand_set(&gr, x, GASROW_H - 1, CELL_MAKE(MAT_STONE, 0));
    }
    for (int y = 2; y < 8; y++) {
        for (int x = 2; x < GASROW_W - 2; x++) {
            sand_set(&gr, x, y, CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1));
        }
    }

    for (int i = 0; i < 200; i++) {
        sand_step(&gr, 0, 1, 0);
    }

    assert_no_stranded_gas(
        "a gas cell walked down into a row the rise sweep had not reached, "
        "took another turn there, and ended up in a row the spread pass was "
        "about to skip");
    gas_rows_teardown();
}

/* The exhaustive mover (sand_set_gas_walk(false)) is a different set of
 * movers - try_fall_or_scatter, try_slide, try_bubble - under the same
 * obligation. */
static void test_the_exhaustive_mover_arms_every_row_it_lands_in(void)
{
    gas_rows_fixture();
    sand_set_gas_walk(&gr, false);
    build_mixed_gas_board(false);

    for (int i = 0; i < 80; i++) {
        sand_step(&gr, 0, 1, 0);
    }

    assert_no_stranded_gas(
        "the exhaustive mover moved gas without arming the row it landed in");
    gas_rows_teardown();
}

/* Tilted gravity turns the spread pass's own moves from same-row into
 * many-row ones, and shaking bypasses the mobility roll and wakes every
 * block. Both are swept here rather than left to the scene suites, which
 * would only reach them by accident. */
static void test_tilted_and_shaken_boards_leave_no_gas_stranded(void)
{
    static const int dirs[8][2] = {
        {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1},
    };

    for (int d = 0; d < 8; d++) {
        gas_rows_fixture();
        build_mixed_gas_board(false);

        for (int i = 0; i < 30; i++) {
            sand_step(&gr, dirs[d][0], dirs[d][1], (i % 4 == 0) ? 200 : 0);
        }

        assert_no_stranded_gas(
            "a tilted or shaken board left gas in a row the spread pass was "
            "about to skip");
        gas_rows_teardown();
    }
}

/* Combustion and boiling mint gas from sand_step_reactions(), which runs
 * AFTER the gas pass - so the map they invalidate is next step's, rebuilt by
 * the rise sweep's own scan. This checks the rebuild really is a full one and
 * that nothing carries over. */
static void test_reaction_made_gas_is_never_stranded(void)
{
    gas_rows_fixture();
    build_mixed_gas_board(true);

    for (int i = 0; i < 120; i++) {
        sand_step(&gr, 0, 1, 0);
    }

    assert_no_stranded_gas(
        "gas minted by a reaction after the gas pass was still unaccounted "
        "for when the next step's spread pass ran");
    gas_rows_teardown();
}

/* Everything above passes for free if the audit never runs or never finds a
 * skippable row to check, so this asserts the mechanism is live: rows with a
 * clear bit exist, and the audit looked at them. It is also the performance
 * claim, stated as a test - a spread pass with nothing to skip is one this
 * change did not speed up. */
static void test_the_skip_actually_fires_and_the_audit_sees_it(void)
{
    gas_rows_fixture();
    build_mixed_gas_board(false);
    sand_gas_row_audit_skippable = 0;

    for (int i = 0; i < 20; i++) {
        sand_step(&gr, 0, 1, 0);
    }

    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0u, sand_gas_row_audit_skippable,
        "no row was ever skippable, so the audit checked nothing and the "
        "tests above assert nothing - the fixture no longer has gas-free "
        "rows, or the map is armed everywhere");
    gas_rows_teardown();
}

void run_sand_gas_rows_suite(void)
{
    RUN_TEST(test_the_walk_arms_every_row_it_lands_in);
    RUN_TEST(test_a_downward_walk_cascade_arms_every_row);
    RUN_TEST(test_the_exhaustive_mover_arms_every_row_it_lands_in);
    RUN_TEST(test_tilted_and_shaken_boards_leave_no_gas_stranded);
    RUN_TEST(test_reaction_made_gas_is_never_stranded);
    RUN_TEST(test_the_skip_actually_fires_and_the_audit_sees_it);
}

SUITE_REGISTER(run_sand_gas_rows_suite);
