/*=============================================================================
 * Portable suite: the falling-sand automaton - gas, fire, wood/embers/steam,
 * bubbles, oil/lava, and acid.
 *
 * Split out of suite_sand.c (bd esp32c6 test-suite-refactor), which had grown
 * past 32,000 lines across 500+ tests. Shared fixtures and assertion helpers
 * live in suite_sand_common.{c,h} - see that header.
 *===========================================================================*/
#include <math.h>   /* not every file in the split still needs atan2()/M_PI,
                     * but every file inherited suite_sand.c's own include
                     * block rather than being pruned by hand, to keep the
                     * split itself mechanical and low-risk */
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
/* Not every libc defines this in <math.h> without a feature-test macro this
 * file has no other reason to set (MinGW's, notably, on the host build) -
 * cheaper to supply it directly than to widen this file's own feature-test
 * exposure for one constant. */
#define M_PI 3.14159265358979323846
#endif

#include "unity.h"
#include "suites.h"

#include "material_palette.h"
#include "sand.h"
#include "sand_priv.h"
#include "util/intmath.h"
#include "suite_sand_common.h"

/* --- gas ------------------------------------------------------------------ */

static void test_gas_rises_straight_up_under_ordinary_gravity(void)
{
    fixture();
    sand_set_gas_walk(&s, false);   /* this straight-line guarantee is the
                                     * exhaustive mover's property now, not
                                     * gas's in general - the walk (the
                                     * default) only drifts up on average */
    sand_set(&s, 3, H - 1, GAS);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS,
        CELL_MATERIAL(sand_at(&s, 3, H - 2)),
        "with ordinary gravity pointing down, gas moves up - the opposite "
        "direction from every other material");
}

static void test_gas_falls_when_the_board_is_inverted(void)
{
    fixture();
    sand_set_gas_walk(&s, false);   /* this straight-line guarantee is the
                                     * exhaustive mover's property now, not
                                     * gas's in general - the walk (the
                                     * default) only drifts down on average */
    sand_set(&s, 3, 0, GAS);

    sand_step(&s, 0, -1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS,
        CELL_MATERIAL(sand_at(&s, 3, 1)),
        "gas always moves AGAINST gravity, whatever direction that "
        "currently is - inverted gravity means gas falls, not a hardcoded "
        "upward move");
}

static void test_gas_rises_diagonally_under_tilted_gravity(void)
{
    fixture();
    sand_set_gas_walk(&s, false);   /* this exact-three-cells guarantee is
                                     * the exhaustive mover's property now,
                                     * not gas's in general - the walk (the
                                     * default) only drifts up-and-left on
                                     * average */
    sand_set(&s, 5, H - 1, GAS);

    for (int i = 0; i < 3; i++) {
        sand_step(&s, 1000, 1000, 0);
    }

    TEST_ASSERT_NOT_EQUAL_MESSAGE(MAT_GAS,
        CELL_MATERIAL(sand_at(&s, 5, H - 1)),
        "the grain must have left its starting cell");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS,
        CELL_MATERIAL(sand_at(&s, 2, H - 4)),
        "with gravity down-and-right, anti-gravity is up-and-left - three "
        "steps of (-1,-1) should land it exactly three columns left and "
        "three rows up from where it started");
}

/* The walk (the new default) trades away the exhaustive mover's exact
 * per-step position for a cheaper, stochastic one - 216/256 of its weight
 * is up-ish, 72/256 straight up (see sand_gas.c). A single cell's path
 * proves nothing about that; only the MEAN of many cells over many steps
 * does, which is what these three tests check instead of an exact cell. */

static void test_gas_drifts_upward_under_ordinary_gravity(void)
{
    fixture();
    const int start_row = H / 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, start_row, GAS);
    }

    for (int i = 0; i < 12; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int count   = 0;
    int row_sum = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_GAS) {
                count++;
                row_sum += y;
            }
        }
    }

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(2, count,
        "setup: several gas cells must survive 12 steps, or a mean over "
        "too few (or none) proves nothing about the walk's drift");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(start_row * count, row_sum,
        "with ordinary gravity, the walk's up-ish bias must pull the MEAN "
        "row of surviving gas higher (smaller y) than the row it started "
        "on, even though no single cell's path is deterministic any more");
}

static void test_gas_drifts_downward_when_the_board_is_inverted(void)
{
    fixture();
    const int start_row = H / 2;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, start_row, GAS);
    }

    for (int i = 0; i < 12; i++) {
        sand_step(&s, 0, -1000, 0);
    }

    int count   = 0;
    int row_sum = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_GAS) {
                count++;
                row_sum += y;
            }
        }
    }

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(2, count,
        "setup: several gas cells must survive 12 steps, or a mean over "
        "too few (or none) proves nothing about the walk's drift");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(start_row * count, row_sum,
        "gas always drifts AGAINST gravity - inverted gravity must pull "
        "the MEAN row of surviving gas lower (larger y) than the row it "
        "started on");
}

static void test_gas_drifts_against_tilted_gravity(void)
{
    fixture();
    const int start_row = H / 2;
    int start_col_sum   = 0;
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, start_row, GAS);
        start_col_sum += x;
    }

    for (int i = 0; i < 12; i++) {
        sand_step(&s, 1000, 1000, 0);
    }

    int count   = 0;
    int row_sum = 0;
    int col_sum = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_GAS) {
                count++;
                row_sum += y;
                col_sum += x;
            }
        }
    }

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(2, count,
        "setup: several gas cells must survive 12 steps, or a mean over "
        "too few (or none) proves nothing about the walk's drift");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(start_row * count, row_sum,
        "gravity is down-and-right, so anti-gravity is up-and-left - the "
        "MEAN row of surviving gas must have risen (smaller y)");
    /* Cross-multiplied rather than dividing: mean_col_survived <
     * mean_col_start  <=>  col_sum/count < start_col_sum/W  <=>
     * col_sum*W < start_col_sum*count (W and count both positive). */
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(start_col_sum * count, col_sum * W,
        "up-and-left also means the MEAN column of surviving gas must "
        "have moved left (smaller x) from where the row started");
}

static void test_gas_is_blocked_by_a_stone_ceiling(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, 0, STONE);
    }
    sand_set(&s, 3, H - 1, GAS);

    for (int i = 0; i < 50; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    /* Once blocked from rising further, a lone grain with open space on
     * both sides is free to drift sideways along the row (equalise_gas()
     * has no reason to keep it in its starting column - the same is true
     * of a single isolated liquid grain). So the real invariant to check
     * is not "still at column 3", it is "the ceiling was never displaced,
     * and the grain is still one row below it, not through it". */
    for (int x = 0; x < W; x++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STONE,
            CELL_MATERIAL(sand_at(&s, x, 0)),
            "a solid ceiling with no gap must never be displaced");
    }

    bool found_gas_below_ceiling = false;
    for (int x = 0; x < W; x++) {
        if (CELL_MATERIAL(sand_at(&s, x, 1)) == MAT_GAS) {
            found_gas_below_ceiling = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_gas_below_ceiling,
        "gas must stop right below a sealed ceiling, not pass through it");
}

static void test_gas_disperses_across_a_ceiling(void)
{
    fixture();
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, 0, STONE);
    }
    for (int y = 0; y < H; y++) {
        sand_set(&s, 0, y, STONE);
        sand_set(&s, W - 1, y, STONE);
    }
    /* Four grains stacked in one column - gas cannot rise through gas
     * (same density, can_enter() requires strictly denser), so once the
     * first reaches the ceiling the rest are blocked from stacking through
     * it too. Whether they disperse sideways instead, or just pile up
     * behind the leader, is exactly what this test checks. */
    for (int y = H - 4; y < H; y++) {
        sand_set(&s, W / 2, y, GAS);
    }

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    int occupied_columns = 0;
    for (int x = 1; x < W - 1; x++) {
        for (int y = 1; y < H; y++) {
            if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_GAS) {
                occupied_columns++;
                break;
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_MESSAGE(1, occupied_columns,
        "four gas grains trapped under a ceiling must end up spread across "
        "more than the one column they rose in, or the perpendicular "
        "spread pass (equalise_gas()) is missing or broken");
}

static void test_sand_sinks_through_gas(void)
{
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, GAS);
        }
    }
    sand_set(&s, 3, 3, SAND);

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_SAND,
        CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "sand is denser than gas, so it must sink all the way through "
        "rather than float on it");
}

static void test_water_sinks_through_gas(void)
{
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, GAS);
        }
    }
    sand_set(&s, 3, 3, WATER);

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER,
        CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "water is denser than gas too, so it must sink through it the same "
        "way sand does");
}

static void test_gas_grain_count_is_conserved(void)
{
    fixture();
    for (int y = 1; y < 4; y++) {
        for (int x = 1; x < 6; x++) {
            sand_set(&s, x, y, GAS);
        }
    }
    const int expected = sand_count(&s);
    TEST_ASSERT_EQUAL_INT(15, expected);

    static const int dirs[8][2] = {
        {0,1}, {1,1}, {1,0}, {1,-1}, {0,-1}, {-1,-1}, {-1,0}, {-1,1},
    };
    for (int d = 0; d < 8; d++) {
        for (int i = 0; i < 20; i++) {
            sand_step(&s, dirs[d][0], dirs[d][1], 0);
            TEST_ASSERT_EQUAL_INT_MESSAGE(expected, sand_count(&s),
                "a step must conserve gas grains in every gravity "
                "direction, the same as it does for sand");
        }
    }
}

static void test_rising_gas_wakes_the_blocks_it_passes_through(void)
{
    fixture();
    sand_enable_sleeping(&s, sleep_blocks);
    sand_set(&s, 3, H - 1, GAS);

    /* Straight, unchanging gravity - every step but the first is steady
     * state, so a block that stops earning BLOCK_ACTIVE from the gas pass
     * would start reporting settled almost immediately, well before the
     * grain actually runs out of places to rise to. */
    for (int i = 0; i < H - 1; i++) {
        sand_step(&s, 0, 1000, 0);
        TEST_ASSERT_FALSE_MESSAGE(sand_block_settled(&s, 0, 0),
            "the block must not be marked settled while a gas grain inside "
            "it is still actively rising step after step - that can only "
            "happen if the gas pass forgot to wake the block it just moved "
            "in");
    }
}

static void test_gas_scatter_can_be_disabled(void)
{
    fixture();
    sand_set_gas_walk(&s, false);   /* this straight-line guarantee is the
                                     * exhaustive mover's property now, not
                                     * gas's in general */
    sand_set_scatter(&s, 0);
    sand_set(&s, 3, H - 1, GAS);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS, CELL_MATERIAL(sand_at(&s, 3, H - 2)),
        "with scatter off, gas rises exactly one cell per step in a "
        "straight line - the same guarantee sand's own fall makes");
}

static void test_gas_decays_and_disappears_over_time(void)
{
    fixture();
    /* Off by default (see test_gas_grain_count_is_conserved, which relies
     * on exactly that) - forced to 100% here for a fast, exact test rather
     * than waiting out the real material figure's low per-step odds. */
    sand_set_decay(&s, 255);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_spawn(&s, 3, H - 1, 0, MAT_GAS),
        "setup: exactly one gas grain placed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MATERIAL_VARIANTS - 1,
        CELL_VARIANT(sand_at(&s, 3, H - 1)),
        "a freshly spawned gas grain must start at full life, not a random "
        "shade - random_cell() special-cases a decaying material the same "
        "way it already does a liquid's fill level");

    /* At a forced 100% chance, life ticks down by exactly one per step -
     * gone on the step that takes it from 1 to 0, so full life takes
     * exactly that many steps to clear. */
    for (int i = 0; i < MATERIAL_VARIANTS - 1; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_count(&s),
        "gas must decay away to nothing given enough time - unlike every "
        "other material, whose grain count is conserved forever (see "
        "test_gas_grain_count_is_conserved)");
}

static void test_gas_decaying_away_marks_its_row_dirty(void)
{
    dirty_fixture();
    sand_set_decay(&s, 255);
    sand_set_mobility(&s, 0);   /* stay put, so the vanish lands at a
                                 * known row instead of wherever it drifted */

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_spawn(&s, 3, 4, 0, MAT_GAS),
        "setup: exactly one gas grain placed");
    memset(dirty, 0, sizeof(dirty));

    for (int i = 0; i < MATERIAL_VARIANTS - 1; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_count(&s),
        "setup: the grain must have decayed away by now");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dirty[4],
        "the row a gas grain decayed away in must be marked dirty, or its "
        "last colour stays on the panel forever after the cell itself is "
        "already empty - tick_decay()'s vanish branch must call "
        "mark_rows() the same way its tick-down branch already does");
}

/* bd esp32c6-uc9: the packed-row equalise skip (row_is_packed(), see
 * sand_gas.c) generalised from py == 0 to tilted gravity via a running count
 * of packed rows already behind the sweep. Row 2 here is packed and has only
 * ONE packed row behind it when the sweep reaches it - nowhere near
 * MAT_FIRE's sight of 5 - so the skip must not fire, and the row's fire must
 * still cross into row 3 through the open row_is_packed() found there. */
static void test_tilted_equalise_still_spreads_a_packed_row_under_the_sight_bound(void)
{
    fixture();
    sand_set_gas_walk(&s, false);
    sand_set_scatter(&s, 0);

    /* Row 1 seals off the rise direction (anti-gravity is up-left for
     * down-right gravity) so every fire cell in row 2 is forced into
     * equalise rather than rising through row 1 first. */
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, 1, STONE);
        sand_set(&s, x, 2, FIRE);
    }

    sand_step(&s, 1000, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 0, 2)),
        "column 0's ray runs off the left edge on its first step and has "
        "nowhere to go - it must stay put");
    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 3, 2)),
        "column 3 must have left row 2 - if the tilted skip fired here, "
        "the whole row's equalise body never ran and nothing would move");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 2, 3)),
        "column 3's grain must land one diagonal step down-left, in the "
        "row that was open");
    TEST_ASSERT_EQUAL_INT_MESSAGE(W, count_of(MAT_FIRE),
        "whole-grain gas conserves its count - this is a move, not a loss");
}

/* --- fire ------------------------------------------------------------- */

/* A stone box sealing columns x0..x1 of row 3 on all four sides with NO
 * spare cells inside, so nothing placed inside it can drift away via its
 * OWN movement pass (main sweep, sand_step_liquids(), or sand_step_gas()
 * - all of which run before reactions, in the same step) before the
 * reactions pass gets a chance to check adjacency: gas/fire rise against
 * gravity (blocked by the row 2 ceiling) and disperse sideways (blocked
 * by walls immediately at x0-1 and x1+1 - not just somewhere further
 * out, since ANY empty cell inside the box is still room for
 * equalise_gas() to hop into), water falls (blocked by the row 4
 * floor). Fire needing this too, not just gas, is new since fire became
 * kind = KIND_GAS - it is now just as capable of drifting off during the
 * SAME step it was placed as gas always was, and a room with even one
 * spare empty cell left room for exactly that (confirmed: the first
 * version of this room, walled only two columns further out than
 * strictly necessary, let both the fire and gas cells each hop one cell
 * sideways into the slack before reactions ever ran). The caller passes
 * the exact span it is about to fill (x0..x1 inclusive) - no slack, no
 * spare cells, by construction.
 *
 * fire_room() now lives in suite_sand_common.{c,h} - reused past this
 * section too. */

static void test_fire_ignites_an_adjacent_flammable_neighbour(void)
{
    fire_room(3, 4);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, GAS);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 4, 3)),
        "a flammable neighbour touching fire must ignite");
}

/* bd esp32c6-zs8: an igniting gas cell that touches a KIND_STATIC
 * neighbour bursts instead of just catching - see gas_ignite_confined()'s
 * own comment in sand_reactions.c for the design (gas in the open burns,
 * the same gas walled in bursts) and try_ignite()'s own comment for why
 * this is gated on s->impulse_buf != NULL.
 *
 * HEAP, not static file scope - shared by the two tests below, each of
 * which mallocs its own W * H impulse_t buffer and frees it before its
 * own assertions can fail; see drop_impulse_buf's own comment above for
 * why this file's static test fixtures cannot share the framebuffer's
 * memory budget. */

static void test_a_confined_gas_pocket_bursts_instead_of_just_catching(void)
{
    /* fire_room() boxes row 3 in stone above and below (see its own
     * comment) - the same room test_fire_ignites_an_adjacent_flammable_
     * neighbour uses, just with impulses now enabled, so the gas cell
     * cannot rise away before reactions gets a turn at it and its
     * ceiling/floor neighbours are real KIND_STATIC cells to burst into,
     * not empty air standing in for one. */
    fire_room(3, 4);
    impulse_t *confined_gas_impulse_buf = malloc((size_t)(W * H) * sizeof *confined_gas_impulse_buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(confined_gas_impulse_buf,
        "confined-gas-pocket impulse queue must fit in what the "
        "framebuffer leaves");
    sand_enable_impulses(&s, confined_gas_impulse_buf, W * H);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, GAS);

    sand_step(&s, 0, 1000, 0);

    /* (4,2) is fire_room()'s own ceiling stone, directly above the gas
     * cell - within sand_explode()'s core radius but never itself
     * touching fire. sand_explode()'s core fill (SAND_EXPLODE_CORE_
     * DIVISOR, sand.h) writes fire into every cell within that radius
     * UNCONDITIONALLY, occupied or not, before a single flight entry is
     * even queued - so only a REAL explosion ever touches it at all; a
     * plain place_reacted() ignition only ever touches the one cell it
     * targets, so the stone above it would still be stone. NOT_EQUAL
     * rather than fire specifically: the fresh fire the core just wrote
     * is itself a non-static occupied cell inside the blast radius, so
     * sand_displace()'s own annulus loop queues it for outward flight too
     * (sand_explode()'s own comment in sand.c) - it may already have
     * moved on by the time this runs, same as any grain caught in a
     * blast, and where it lands afterward is no longer pinned; see
     * test_a_strong_close_blast_can_breach_a_wall's own comment for the
     * same reasoning applied to an ordinary DETONATE blast. */
    const uint8_t ceiling_material = CELL_MATERIAL(sand_at(&s, 4, 2));
    const uint8_t gas_cell_material = CELL_MATERIAL(sand_at(&s, 4, 3));
    const int impulse_count = s.impulse_count;

    /* Freed BEFORE the assertions: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. All reads of confined_gas_impulse_buf (via `s`) are done by
     * this point. */
    free(confined_gas_impulse_buf);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, gas_cell_material,
        "setup: the gas cell touching the wall must still ignite");
    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(MAT_STONE, ceiling_material,
        "a confined gas cell's own ignition must reach past itself, into "
        "the wall it was confined by - proof this was sand_explode()'s "
        "own core fill, not a plain place_reacted()");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, impulse_count,
        "a real explosion must also queue its own outward annulus, not "
        "just fill a core of fire");
}

static void test_an_open_gas_pocket_still_just_catches_fire(void)
{
    fixture();
    impulse_t *confined_gas_impulse_buf = malloc((size_t)(W * H) * sizeof *confined_gas_impulse_buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(confined_gas_impulse_buf,
        "open-gas-pocket impulse queue must fit in what the framebuffer "
        "leaves");
    sand_enable_impulses(&s, confined_gas_impulse_buf, W * H);
    sand_set_mobility(&s, 0);   /* keep the gas from rising away before
                                 * reactions gets a turn at it this same
                                 * step - see test_fire_is_not_smothered_
                                 * by_gas's own use of this for the same
                                 * reason */

    /* Same fire-beside-gas shape as the confined test above, minus the
     * room - nothing here touches a KIND_STATIC cell at all, so this must
     * behave exactly like test_fire_ignites_an_adjacent_flammable_neighbour,
     * even with impulses enabled: "gas in the open burns" must not become
     * "gas always explodes now that the mechanism exists". */
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, GAS);

    sand_step(&s, 0, 1000, 0);

    const uint8_t gas_cell_material = CELL_MATERIAL(sand_at(&s, 4, 3));
    const bool neighbour_empty = CELL_IS_EMPTY(sand_at(&s, 4, 2));
    const int impulse_count = s.impulse_count;

    /* Freed BEFORE the assertions: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(confined_gas_impulse_buf);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, gas_cell_material,
        "an unconfined gas cell must still ignite");
    TEST_ASSERT_TRUE_MESSAGE(neighbour_empty,
        "an unconfined ignition must not reach past the cell it targets - "
        "a neighbour that never touched fire must stay untouched");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, impulse_count,
        "an unconfined ignition must never queue an explosion");
}

static void test_extinguishing_wins_over_igniting(void)
{
    fire_room(2, 4);
    sand_set(&s, 2, 3, WATER);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, GAS);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "fire touching water must be extinguished, and now becomes steam "
        "rather than simply vanishing - see reaction_t.quench_to");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_GAS, CELL_MATERIAL(sand_at(&s, 4, 3)),
        "extinguishing must win outright over igniting - a gas neighbour "
        "must not catch fire in the same step the fire that would have "
        "lit it was put out");
}

static void test_fire_burns_out_and_disappears_over_time(void)
{
    fixture();
    sand_set_decay(&s, 255);   /* forced 100% chance, for a fast, exact
                                * test rather than waiting out the real
                                * material figure's low per-step odds */

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_spawn(&s, 3, 3, 0, MAT_FIRE),
        "setup: exactly one fire cell placed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MATERIAL_VARIANTS - 1,
        CELL_VARIANT(sand_at(&s, 3, 3)),
        "a freshly spawned fire cell must start at full life, not a "
        "random shade - random_cell() already generalises this for any "
        "decay != 0 material, gas included");

    for (int i = 0; i < MATERIAL_VARIANTS - 1; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_count(&s),
        "fire must burn out to nothing given enough time, the same "
        "decay mechanism gas already uses");
}

/* Supersedes the old test_fire_does_not_move_under_gravity: fire was
 * KIND_STATIC and this test asserted the opposite of what it asserts
 * now. Kept under the same topic heading rather than silently deleted,
 * mirroring how sand_reactions.c's own top comment narrates the
 * KIND_STATIC-era design as history worth keeping visible. Mirrors
 * test_gas_rises_straight_up_under_ordinary_gravity exactly - fire is
 * kind = KIND_GAS now, swept by the identical pass. */
static void test_fire_rises_and_disperses_like_gas(void)
{
    fixture();
    sand_set_gas_walk(&s, false);   /* this straight-line guarantee is the
                                     * exhaustive mover's property now, not
                                     * KIND_GAS's in general */
    sand_set(&s, 3, H - 1, FIRE);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 3, H - 2)),
        "with ordinary gravity pointing down, fire moves up - the same "
        "kind = KIND_GAS movement gas already has, replacing the "
        "immobile-ember behaviour this test used to assert");
}

/* Supersedes the old test_fire_is_not_displaced_by_falling_sand, for
 * the same reason as the test above - mirrors test_sand_sinks_through_gas
 * exactly, since fire's displacement rules are now identical to gas's
 * (density-based, not KIND_STATIC's blanket refusal). Burying it
 * completely is a different question - see
 * test_fire_is_smothered_when_fully_buried below, which is the actual
 * replacement for "sand can put fire out", just via smothering rather
 * than simple contact. */
static void test_sand_sinks_through_fire(void)
{
    fixture();
    for (int y = 4; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, FIRE);
        }
    }
    sand_set(&s, 3, 3, SAND);

    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_SAND, CELL_MATERIAL(sand_at(&s, 3, H - 1)),
        "sand is denser than fire (60 > 15), so it must sink all the "
        "way through rather than be blocked by it - a single touch does "
        "not smother fire, it just passes through uneventfully");
}

static void test_fire_is_smothered_when_fully_buried(void)
{
    fixture();
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 3, 2, STONE);   /* above */
    sand_set(&s, 3, 4, STONE);   /* below */
    sand_set(&s, 2, 3, STONE);   /* left */
    sand_set(&s, 4, 3, STONE);   /* right */

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_TRUE_MESSAGE(CELL_IS_EMPTY(sand_at(&s, 3, 3)),
        "fire buried on all four sides by something denser must smother "
        "out - the only way sand puts fire out, since a single touch "
        "just lets sand sink through uneventfully (see "
        "test_sand_sinks_through_fire above)");
}

static void test_fire_is_not_smothered_with_a_gap(void)
{
    fixture();
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 3, 2, STONE);   /* above */
    sand_set(&s, 2, 3, STONE);   /* left */
    sand_set(&s, 4, 3, STONE);   /* right */
    /* Diagonal-up neighbours also need blocking, not just the straight
     * cardinal ones - try_slide()'s own fallback would otherwise carry
     * fire diagonally out of (3,3) via the two open corners before
     * reactions ever ran, leaving the cell empty for a reason that has
     * nothing to do with smothering (confirmed: this is exactly what
     * happened the first version of this test wrote). */
    sand_set(&s, 2, 2, STONE);
    sand_set(&s, 4, 2, STONE);
    /* (3,4), below, deliberately left open - a KIND_GAS material only
     * rises and spreads sideways, never falls, so this gap is safe
     * from being closed by fire itself drifting into it before
     * reactions checks smothering; the test stays a clean check of the
     * smother predicate alone. */

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "one open side is enough for air to reach it - smothered() "
        "requires ALL four neighbours to be denser, not just three");
}

static void test_fire_is_not_smothered_by_gas(void)
{
    fixture();
    sand_set_mobility(&s, 0);   /* keep fire (and the surrounding gas)
                                 * from rising away before reactions
                                 * checks smothering this same step -
                                 * equalise_gas()'s own spread sub-pass
                                 * never touches these neighbours anyway
                                 * (different material, not empty), so
                                 * this alone is enough to pin fire */
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 3, 2, GAS);
    sand_set(&s, 3, 4, GAS);
    sand_set(&s, 2, 3, GAS);
    sand_set(&s, 4, 3, GAS);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "gas is not denser than fire (10 < 15), so a gas-only surround "
        "must not smother it - otherwise any sufficiently large, dense "
        "pocket of fire/gas would extinguish itself from the inside "
        "out");
}

static void test_liquid_wins_over_smothering(void)
{
    fixture();
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 3, 2, STONE);
    sand_set(&s, 2, 3, STONE);
    sand_set(&s, 4, 3, STONE);
    /* The two upward diagonals also need blocking, not just the three
     * cardinal sides above - the straight-up cell being blocked leaves
     * try_slide()'s own diagonal fallback free to carry fire out to
     * (2,2) or (4,2) before reactions ever runs, now that the assertion
     * below checks WHAT fire became rather than merely that (3,3) ended
     * up empty (which a fire that drifted away would also satisfy,
     * masking exactly this - confirmed: this is what the first version
     * of this test, without these two lines, actually did). Mirrors
     * test_fire_is_not_smothered_with_a_gap's identical reasoning. */
    sand_set(&s, 2, 2, STONE);
    sand_set(&s, 4, 2, STONE);
    sand_set(&s, 3, 4, WATER);
    /* Floor plus both down-diagonal neighbours, not the floor alone -
     * WATER here is CELL_MAKE(MAT_WATER, 8), not a full MASS_MAX cell,
     * and move_liquid_grain() can hand its entire mass to a single open
     * down-diagonal in one main-sweep call, draining (3,4) before
     * reactions ever gets a turn to see it - see
     * test_creating_steam_arms_the_gas_pass's identical fix for the
     * full reasoning. */
    sand_set(&s, 3, 5, STONE);
    sand_set(&s, 2, 5, STONE);
    sand_set(&s, 4, 5, STONE);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "fire touching water on even one side must extinguish via the "
        "liquid rule (becoming steam, not simply vanishing - see "
        "reaction_t.quench_to) - smothered() itself would have said no "
        "here too (it explicitly excludes liquid neighbours from "
        "counting towards a smother, even though water is denser than "
        "fire), so this confirms that exclusion does not accidentally "
        "block the liquid path from still working");
}

static void test_igniting_a_neighbour_marks_its_row_dirty(void)
{
    dirty_fixture();
    /* Box gas in on every side except where it touches fire below, so it
     * cannot drift away via its OWN rise or spread pass (both run before
     * reactions, in the same step) before reactions gets to check it. A
     * straight-up ceiling alone blocks the plain rise but leaves the
     * diagonal slide fallback free to move it up-left/up-right instead
     * (which is exactly what happened the first time this test was
     * written with only the straight-up cell blocked) - and a ceiling
     * with no side walls at all leaves the perpendicular spread pass
     * free to walk it sideways out of column 3 entirely (the failure
     * before that). All three of straight-up, both diagonals-up, and
     * both sideways neighbours need blocking - everywhere except where
     * fire sits, directly below. */
    sand_set(&s, 2, 2, STONE);
    sand_set(&s, 3, 2, STONE);
    sand_set(&s, 4, 2, STONE);
    sand_set(&s, 2, 3, STONE);
    sand_set(&s, 4, 3, STONE);
    sand_set(&s, 3, 4, FIRE);
    sand_set(&s, 3, 3, GAS);        /* directly above fire - a DIFFERENT
                                      * row from fire's own, so this test
                                      * can tell whether mark_rows()
                                      * targeted the ignited neighbour's
                                      * row specifically, not just the
                                      * fire cell's */
    memset(dirty, 0, sizeof(dirty));

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "setup: the gas neighbour must have ignited");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dirty[3],
        "the row the newly-ignited neighbour is in must be marked dirty, "
        "or its cell changes colour on the panel without ever being "
        "redrawn - ignition's mark_rows() call must target the "
        "neighbour's row, not just the fire cell's own");
}

static void test_fire_burning_out_marks_its_row_dirty(void)
{
    dirty_fixture();
    sand_set_decay(&s, 255);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_spawn(&s, 3, 4, 0, MAT_FIRE),
        "setup: exactly one fire cell placed");
    memset(dirty, 0, sizeof(dirty));

    for (int i = 0; i < MATERIAL_VARIANTS - 1; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_count(&s),
        "setup: the fire cell must have burned out by now");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, dirty[4],
        "the row a fire cell burned out in must be marked dirty, or its "
        "last colour stays on the panel forever after the cell itself is "
        "already empty - tick_decay()'s vanish branch already does this "
        "correctly (shared with gas), this pins it down for fire too");
}

static void test_fire_spreads_through_a_connected_pocket_in_one_step(void)
{
    fixture();
    sand_set_gas_walk(&s, false);   /* gas runs before reactions in
                                     * sand_step() - the walk would scatter
                                     * this line before reactions ever saw
                                     * it, so the pass is pinned to isolate
                                     * reaction scan order, not gas motion */
    sand_set(&s, 0, 0, FIRE);
    for (int x = 1; x < W; x++) {
        sand_set(&s, x, 0, GAS);
    }

    sand_step(&s, 0, 1000, 0);

    for (int x = 1; x < W; x++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE,
            CELL_MATERIAL(sand_at(&s, x, 0)),
            "a straight line of gas laid out AHEAD of the reactions "
            "pass's own fixed scan direction (row-major, left to right) "
            "must ignite all the way through in a single step - the "
            "confirmed explosion-like cascade, not creeping spread. A "
            "line laid out BEHIND the scan direction would need several "
            "steps instead - a documented, accepted scan-order artifact, "
            "not a bug (see sand_reactions.c's own top comment)");
    }
}

static void test_pouring_stone_never_arms_the_reactions_pass(void)
{
    fixture();

    /* No public getter for may_have_burning - reading the field directly
     * is intentional here. The bug this guards (gating on kind ==
     * KIND_STATIC instead of the material ID, in sand_set()/
     * try_spawn_one()) is only externally visible as a silent
     * performance regression on device - a host test can only catch it
     * by checking the bookkeeping directly, there is no behavioural
     * difference in simulation output to assert on instead. */
    for (int i = 0; i < 20; i++) {
        sand_spawn(&s, 3, 3, 2, MAT_STONE);
        sand_step(&s, 0, 1000, 0);
        TEST_ASSERT_FALSE_MESSAGE(s.may_have_burning,
            "placing plain stone must never set may_have_burning - stone "
            "shares KIND_STATIC with fire and ember, so gating on kind "
            "instead of the material ID would silently re-arm a "
            "full-grid reactions scan on every stone touch");
    }
}


/* Snow over a dirt bed at one moisture level, run out, snow left standing.
 * Its own grid each time: a wet half and a dry half on one board contaminate
 * each other, because powder scatters sideways and moisture percolates - the
 * first version of this test lost its control that way. */
static int snow_left_over_soil_at(uint8_t moisture)
{
    fixture();

    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
        sand_set(&s, x, H - 2, soil_set_moisture(CELL_MAKE(MAT_DIRT, 0), moisture, 0));
        sand_set(&s, x, H - 3, SNOW);
    }

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    return count_cells_of(MAT_SNOW);
}



/* COLD REACHES THROUGH THE MEDIUM, not just into the cell it touches.
 *
 * A snowbank on glass used to chill three rows and stop, identical at 250
 * steps and at 1000. The slab is deliberately taller than CONDUCT_REACH, so a
 * pass means the cold travelled rather than simply hitting the bottom.
 *
 * This is the REACH-AND-STRENGTH test, not a rate test: how long the slab
 * takes to get there is tuning, and lives in the two period constants. */
static void test_cold_conducts_deep_into_a_slab(void)
{
    const int W2 = 40, H2 = 60;
    const int slab_top = 10;
    uint8_t *cells = calloc(W2 * H2, 1);
    TEST_ASSERT_NOT_NULL(cells);

    sand_t g;
    sand_init(&g, cells, W2, H2, 7u);
    for (int y = slab_top; y < H2; y++) {
        for (int x = 0; x < W2; x++) {
            sand_set(&g, x, y, CELL_MAKE(MAT_GLASS, SAND_AMBIENT_HEAT));
        }
    }
    for (int y = 6; y < slab_top; y++) {
        for (int x = 0; x < W2; x++) {
            sand_set(&g, x, y, SNOW);
        }
    }

    /* 2000 steps, a minute of play, because conduction is deliberately slow -
     * see COLD_CARRY_PERIOD. At 250 steps the slab is only 5% shocked, which
     * is the mechanic working, not failing. */
    for (int i = 0; i < 2000; i++) {
        sand_step(&g, 0, 1000, 0);
    }

    int deepest = slab_top - 1, shocked = 0;
    for (int y = slab_top; y < H2; y++) {
        for (int x = 0; x < W2; x++) {
            const cell_t c = sand_at(&g, x, y);
            if (CELL_MATERIAL(c) != MAT_GLASS) {
                continue;
            }
            if (CELL_VARIANT(c) < SAND_AMBIENT_HEAT && y > deepest) {
                deepest = y;
            }
            if (CELL_VARIANT(c) <= SAND_SHOCK_COLD) {
                shocked++;
            }
        }
    }
    const int depth = deepest - slab_top + 1;
    free(cells);

    /* Measured 31 rows with the walk and 3 without, so this sits far from
     * both and fails loudly rather than drifting. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(20, depth,
        "cold must conduct well down a glass slab, not stop at the cells it "
        "touches - three rows is what it managed before it could travel");

    /* AND ARRIVE COLD, not merely tinted. Reach alone was not what the
     * report asked for: a slab where every chilled cell sits one level under
     * ambient looks weak and, more to the point, never reaches
     * SAND_SHOCK_COLD, so heat from below cannot shatter it. Measured 43% of
     * the slab at or below that threshold, against 12% when the walk
     * attenuated at every cell. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE((W2 * (H2 - slab_top)) / 4, shocked,
        "a quarter of the slab at least must reach SAND_SHOCK_COLD, or the "
        "cold is too shallow for heat below to break the glass");
}

/* WET SOIL MELTS SNOW, DRY SOIL DOES NOT.
 *
 * Snow melted only against open water before this, because the thaw test
 * reads the neighbour's KIND and dirt carries its water as a moisture nibble
 * instead - so a soaked bank and a dry one looked identical to it
 * (bd esp32c6-bl4).
 *
 * The dry bed is the control, doing two jobs: it proves the melt is about the
 * WATER in the soil and not about dirt, and it pins the floor, since the rate
 * scales with moisture and is meant to reach zero well before bone dry. */
static void test_snow_melts_on_wet_soil_but_not_on_dry(void)
{
    const reaction_t *dr = reaction_of(CELL_MAKE(MAT_DIRT, 0));

    const int on_wet = snow_left_over_soil_at(dr->moist_max);
    const int on_dry = snow_left_over_soil_at(0);

    TEST_ASSERT_LESS_THAN_INT_MESSAGE(on_dry, on_wet,
        "snow resting on saturated soil must melt faster than snow on dry "
        "soil - the water is bound in the grains rather than standing free, "
        "but it is still water");
    TEST_ASSERT_EQUAL_INT_MESSAGE(W, on_dry,
        "control: snow on BONE DRY dirt must not melt at all, or the rule is "
        "about dirt rather than about the water in it");
}

/* THE BALANCE CEILING: 32 cells of snow reach 90% ice in about three minutes.
 *
 * The one crust test that does NOT force the rate - the others call
 * sand_set_crust(), so sweeping crusts through them is byte-identical and the
 * shipped value goes untested. Measured 5855 steps.
 *
 * 90% and not all: the rim never crusts, so a cover this shape tops out just
 * over 90%. No side walls - they seed the crust up the full height, and the
 * front would travel sideways rather than into the cover's depth. */
static void test_a_32_cell_snow_cover_turns_to_ice_in_about_three_minutes(void)
{
    enum { GW = 56, GH = 40, X0 = 4, X1 = 52, DEPTH = 32 };
    uint8_t *cells  = calloc(GW * GH, 1);
    uint8_t *blocks = calloc((size_t)((GW + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
                           * (size_t)((GH + SAND_BLOCK_H - 1) / SAND_BLOCK_H), 1);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t g;
    memset(&g, 0, sizeof g);
    sand_init(&g, cells, GW, GH, 71u);
    sand_enable_sleeping(&g, blocks);

    for (int x = 0; x < GW; x++) {
        sand_set(&g, x, GH - 1, STONE);
    }
    for (int y = GH - 1 - DEPTH; y < GH - 1; y++) {
        for (int x = X0; x < X1; x++) {
            sand_set(&g, x, y, SNOW);
        }
    }
    for (int i = 0; i < 200; i++) {
        sand_step(&g, 0, 1000, 0);
    }

    int almost_at = -1;
    for (int i = 1; i <= 20000 && almost_at < 0; i++) {
        sand_step(&g, 0, 1000, 0);
        int ice = 0, total = 0;
        for (int y = 0; y < GH; y++) {
            for (int x = X0; x < X1; x++) {
                const int m = CELL_MATERIAL(sand_at(&g, x, y));
                if (m == MAT_EXTENDED) {
                    ice++;
                    total++;
                } else if (m == MAT_SNOW) {
                    total++;
                }
            }
        }
        if (total != 0 && ice * 10 >= total * 9) {
            almost_at = i;
        }
    }
    /* AND THE SKIN IS STILL SNOW. Ice grows inside the drift; the surface it
     * is growing under does not join it, which is why the ceiling above is
     * 90% and not everything. The cover's own top row - open sky above every
     * cell of it. Read BEFORE the frees, not after. */
    int skin_snow = 0, skin_total = 0;
    for (int x = X0; x < X1; x++) {
        for (int y = 0; y < GH; y++) {
            const int m = CELL_MATERIAL(sand_at(&g, x, y));
            if (m != MAT_SNOW && m != MAT_EXTENDED) {
                continue;
            }
            skin_total++;
            skin_snow += (m == MAT_SNOW) ? 1 : 0;
            break;   /* topmost cell of this column only */
        }
    }

    free(cells);
    free(blocks);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, almost_at,
        "a 32 cell cover of snow must end up 90% ice - measured 5855 steps; "
        "never getting there means the shipped crusts rate cannot reach the "
        "balance ceiling at all, which is what a byte-wide field against a "
        "65536 roll used to guarantee");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(2000, almost_at,
        "and must not get there in a few seconds - snow landing on anything "
        "would stop reading as snow");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(12000, almost_at,
        "nor take ten minutes - the ceiling this pins is about three");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(skin_total * 3 / 4, skin_snow,
        "the drift's own surface must still be snow after the inside has "
        "iced - a cell with open space beside it is the rim, and the rim does "
        "not thicken a crust forming under it");
}

/* AIR IS NOT A MATERIAL, so an exposed surface does not crust - otherwise a
 * drift rims its whole outline in ice.
 *
 * A free-standing block tells the two apart: stone under it, air on the other
 * three sides. Measured at 2000 steps, 12 of 12 along the stone and 1 of 12
 * down the open sides, that one widened up from the iced floor. */
static void test_snow_does_not_crust_against_open_air(void)
{
    enum { GW = 32, GH = 32, X0 = 10, X1 = 22, YTOP = 18, YBOT = GH - 2 };
    uint8_t *cells  = calloc(GW * GH, 1);
    uint8_t *blocks = calloc((size_t)((GW + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
                           * (size_t)((GH + SAND_BLOCK_H - 1) / SAND_BLOCK_H), 1);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t g;
    memset(&g, 0, sizeof g);
    sand_init(&g, cells, GW, GH, 71u);
    sand_enable_sleeping(&g, blocks);
    sand_set_crust(&g, 4);   /* 4 in CRUST_ROLL_MAX, as 256 in 65536 was */

    for (int x = 0; x < GW; x++) {
        sand_set(&g, x, GH - 1, STONE);
    }
    for (int y = YTOP; y <= YBOT; y++) {
        for (int x = X0; x < X1; x++) {
            sand_set(&g, x, y, SNOW);
        }
    }

    for (int i = 0; i < 2000; i++) {
        sand_step(&g, 0, 1000, 0);
    }

    int floor_ice = 0, floor_n = 0, side_ice = 0, side_n = 0;
    for (int y = YTOP; y <= YBOT; y++) {
        for (int x = X0; x < X1; x++) {
            const cell_t c = sand_at(&g, x, y);
            if (CELL_MATERIAL(c) != MAT_EXTENDED && CELL_MATERIAL(c) != MAT_SNOW) {
                continue;
            }
            const int ice = (CELL_MATERIAL(c) == MAT_EXTENDED) ? 1 : 0;
            if (y == YBOT) {
                floor_n++;
                floor_ice += ice;
            } else if (y > YTOP && (x == X0 || x == X1 - 1)) {
                side_n++;
                side_ice += ice;
            }
        }
    }
    free(cells);
    free(blocks);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(floor_n / 2, floor_ice,
        "setup: snow resting on stone must crust along that contact, or the "
        "assertion below passes on a bank that never crusted anywhere");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(side_n / 3, side_ice,
        "snow with nothing but open air beside it must stay powder - a crust "
        "forms where snow meets another material, and air is not one");
}

/* A CRUST STARTS AT THE FACES AND THICKENS INWARD, never reaching the core.
 *
 * Three claims, one per assertion below. Measured at 4000 steps, ice per
 * depth: ring 90/90, then 48/82, 21/74, 4/66, 1/50. Without the border test
 * the bank ices flat - 71 of 74 two deep - and without the slower widening
 * rate the front eats inward and does the same. */
static void test_a_snowbank_crusts_on_its_faces_and_thickens_slowly_inward(void)
{
    enum { GW = 40, GH = 40, X0 = 8, X1 = 32, YTOP = 16, YBOT = GH - 2 };
    uint8_t *cells  = calloc(GW * GH, 1);
    uint8_t *blocks = calloc((size_t)((GW + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
                           * (size_t)((GH + SAND_BLOCK_H - 1) / SAND_BLOCK_H), 1);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t g;
    memset(&g, 0, sizeof g);
    sand_init(&g, cells, GW, GH, 71u);
    sand_enable_sleeping(&g, blocks);
    sand_set_crust(&g, 4);   /* the shipped rate is minutes, not frames */

    /* WALLED, and resting on the floor. Snow is a powder: a block of it left
     * in mid-air collapses into rubble, and the first version of this measured
     * the rubble. */
    for (int x = 0; x < GW; x++) {
        sand_set(&g, x, GH - 1, STONE);
    }
    for (int y = 10; y < GH - 1; y++) {
        sand_set(&g, X0 - 1, y, STONE);
        sand_set(&g, X1, y, STONE);
    }
    for (int y = YTOP; y <= YBOT; y++) {
        for (int x = X0; x < X1; x++) {
            sand_set(&g, x, y, SNOW);
        }
    }

    /* Long enough for the second layer to be well under way. Both paths are
     * deliberately slow now - seeding as well as widening - so this is a
     * multiple of what the shape needs, not a rate being measured. */
    for (int i = 0; i < 12000; i++) {
        sand_step(&g, 0, 1000, 0);
    }

    int ring_ice = 0, ring_total = 0;
    int next_ice = 0, next_total = 0, core_ice = 0, core_total = 0;
    for (int y = YTOP; y <= YBOT; y++) {
        for (int x = X0; x < X1; x++) {
            int depth = x - X0;
            if (X1 - 1 - x < depth) { depth = X1 - 1 - x; }
            if (y - YTOP < depth)   { depth = y - YTOP; }
            if (YBOT - y < depth)   { depth = YBOT - y; }

            const cell_t c = sand_at(&g, x, y);
            const int is_ice = (CELL_MATERIAL(c) == MAT_EXTENDED) ? 1 : 0;
            if (depth == 0) {
                ring_total++;
                ring_ice += is_ice;
            } else if (depth == 1) {
                next_total++;
                next_ice += is_ice;
            } else if (depth >= 3) {
                core_total++;
                core_ice += is_ice;
            }
        }
    }
    free(cells);
    free(blocks);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(ring_total * 3 / 4, ring_ice,
        "setup: the exposed faces of a settled bank must actually crust, or "
        "everything below passes on a bank that never iced at all");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(next_total / 8, next_ice,
        "the layer behind the shell must ice too - a crust thickens inward "
        "from the face it started on, it is not frozen at one cell forever");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(core_total / 4, core_ice,
        "but the core of a drift must still be mostly powder - a bank iced "
        "all the way through is not a crust, it is a block of ice");
}

/* A SETTLED SNOWBANK CRUSTS OVER; A FALLING ONE DOES NOT.
 *
 * The rest test is the whole rule - a snowfall in flight must cost nothing,
 * and only banks that have stopped moving pay anything. Sleeping has to be
 * ENABLED for any of it: cell_settled() reads block_state, and with sleeping
 * off nothing is ever known to be at rest, so the rule correctly never fires.
 *
 * sand_set_crust() forces the roll because the shipped rate is a handful in
 * CRUST_ROLL_MAX a step - minutes of crusting, not frames. */
static void test_a_settled_snowbank_crusts_to_ice(void)
{
    uint8_t *cells  = calloc(W * H, 1);
    uint8_t *blocks = calloc((size_t)((W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
                             * (size_t)((H + SAND_BLOCK_H - 1) / SAND_BLOCK_H), 1);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t snow_sim;
    sand_init(&snow_sim, cells, W, H, 41u);
    sand_enable_sleeping(&snow_sim, blocks);
    sand_set_crust(&snow_sim, CRUST_ROLL_MAX);   /* every settled snow cell, every step */

    for (int x = 0; x < W; x++) {
        sand_set(&snow_sim, x, H - 1, STONE);
        sand_set(&snow_sim, x, H - 2, SNOW);
    }

    /* Long enough for the bank to land and its block to be marked settled -
     * the rule cannot fire before that however hard the roll is forced. */
    int ice = 0;
    for (int i = 0; i < 200 && ice == 0; i++) {
        sand_step(&snow_sim, 0, 1000, 0);
        for (int k = 0; k < W * H; k++) {
            if (cells[k] == MATX(MATX_ICE)) {
                ice++;
            }
        }
    }

    /* THE CONTROL, and it is what proves the rest test is load-bearing rather
     * than decorative: the same bank, the same forced roll, but jostled every
     * step so nothing is ever marked settled. A jostle clears the settled bits
     * board-wide (see sand_step()), which is exactly the state a snowfall in
     * flight is in. Without this the test passes just as well on a rule that
     * ignores rest entirely and crusts everything it sees. */
    memset(cells, 0, W * H);
    sand_init(&snow_sim, cells, W, H, 41u);
    sand_enable_sleeping(&snow_sim, blocks);
    sand_set_crust(&snow_sim, CRUST_ROLL_MAX);

    for (int x = 0; x < W; x++) {
        sand_set(&snow_sim, x, H - 1, STONE);
        sand_set(&snow_sim, x, H - 2, SNOW);
    }

    for (int i = 0; i < 200; i++) {
        sand_step(&snow_sim, 0, 1000, 1);   /* jostled: never settles */
    }

    int shaken_ice = 0;
    for (int k = 0; k < W * H; k++) {
        if (cells[k] == MATX(MATX_ICE)) {
            shaken_ice++;
        }
    }

    /* BOTH frees before EITHER assert: a failed TEST_ASSERT longjmps straight
     * past anything after it, and this test owns two allocations. */
    free(cells);
    free(blocks);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, ice,
        "a snowbank that has come to rest must crust into ice - with the "
        "roll forced, the only thing left gating it is the settled test");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, shaken_ice,
        "snow that is never allowed to settle must never crust, however hard "
        "the roll is forced - the rule is about rest, and a snowfall in "
        "flight has to cost nothing");
}

/* BURYING A FIRE PUTS IT OUT - one of only two ways fire ends, and asserted
 * nowhere until now: the whole suite passed with smothering disabled outright
 * (bd esp32c6-dxj).
 *
 * Stone, because sand falls and the arrangement must still be one when the
 * reactions pass arrives. THE OILED CELL IS THE POINT: without a control this
 * passes just as well on a board where fire merely decays, which is the other
 * way fire ends and has nothing to do with burial. One step, because
 * smothering carries no roll. */
static void test_a_fire_buried_on_all_four_sides_goes_out(void)
{
    fixture();

    /* Fire is KIND_GAS, so both cells are boxed on all EIGHT neighbours -
     * leaving a corner open just lets it escape diagonally, and the cell
     * reads empty for a reason that has nothing to do with burial. */
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            sand_set(&s, 2 + dx, 3 + dy, STONE);
            sand_set(&s, 5 + dx, 3 + dy, STONE);
        }
    }
    sand_set(&s, 2, 3, FIRE);
    sand_set(&s, 5, 3, FIRE);

    /* THE CONTROL, and the only difference between the two cells: one
     * cardinal is oil instead of stone. neighbor_smothers() rejects any
     * KIND_LIQUID whatever its density, so this cell is boxed in just as
     * tightly and is NOT smothered. Oil rather than water because water
     * quenches, which would end the fire by the other route and prove
     * nothing. */
    sand_set(&s, 5, 4, OIL);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 2, 3)),
        "a fire covered on all four cardinals by a denser non-liquid must go "
        "out - burying a fire is how you put it out, and smothered() carries "
        "no roll, so one step is enough");

    TEST_ASSERT_EQUAL_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&s, 5, 3)),
        "control: the same cell with ONE side open must still be burning - "
        "otherwise this test is measuring fire decaying on its own and would "
        "pass with smothering removed entirely");
}

/* A MATERIAL THIS PASS CREATES MUST SURVIVE THE PASS'S OWN WRITE-BACK.
 *
 * The reactions walk logs a cell's material, then a stage may convert that
 * same cell: saturated gunpowder becomes oil at the walk's OWN (x, y). It
 * logged MAT_EXTENDED and never returns, so its census cannot hold MAT_OIL -
 * only latch_content_flags() knows (bd esp32c6-cxx).
 *
 * NOT VISIBLE TO THE FINGERPRINT: the dropped bit only changes an outcome
 * where a skip fires, so every scene hashes identically either way. */
static void test_a_material_created_during_the_pass_stays_in_the_mask(void)
{
    fixture();

    const reaction_t *r = reaction_of(GUNPOWDER_BASE);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, STONE);
    }
    /* Saturated on arrival, so the soaked_to roll is live immediately and no
     * water is needed - water would put a second material on the board and
     * blur what the mask is being asked about. */
    sand_set(&s, 4, H - 2, with_moisture(GUNPOWDER_CELL(0), r->moist_max, r));

    bool turned = false;
    for (int i = 0; i < 4000 && !turned; i++) {
        sand_step(&s, 0, 1000, 0);
        if (count_cells_of(MAT_OIL) > 0) {
            turned = true;
            TEST_ASSERT_TRUE_MESSAGE((s.may_have_materials & (1u << MAT_OIL)) != 0,
                "oil is on the board, so may_have_materials must say so - the "
                "cell that became oil was converted at the reactions walk's "
                "own coordinates, which it had already counted as gunpowder "
                "and does not revisit");
        }
    }

    /* Without this the test passes on a board where nothing ever converts,
     * asserting nothing at all. */
    TEST_ASSERT_TRUE_MESSAGE(turned,
        "setup: saturated gunpowder must actually reach oil inside the "
        "window, or this test proves nothing");
}

/* A DRY BOARD WITH SAND ON IT HAS TO LET THE MOISTURE PASS GO.
 *
 * Sand soaks but does not dry, so its variant is a SHADE, not a wetness -
 * and step_one_soaking_cell() reads CELL_MOISTURE() off every cell it
 * runs on, sand included. Soil's dry tones put real values in the low
 * half of the nibble, so that read only returns zero for a shade below
 * SOIL_DRY_TONES: a grain shaded any higher reported moisture it never
 * had, the pass kept setting FOUND_MOISTURE, and may_have_moisture -
 * which only ever CLEARS from that flag, never sets from it - could
 * never go back down once any water had armed it.
 *
 * Same justification for touching the field directly as
 * test_pouring_stone_never_arms_the_reactions_pass above: the only
 * symptom is a full-board pass that never switches off again, which
 * costs frame time on device and changes no simulation output at all,
 * so there is nothing behavioural to assert on instead.
 *
 * The shade is set explicitly rather than poured, because the pour band
 * decides which shades a brushful gets and this needs the specific half
 * of the range that used to lie. */
static void test_sand_alone_lets_the_moisture_pass_switch_off_again(void)
{
    fixture();
    sand_clear(&s);

    for (uint8_t shade = 0; shade < SAND_DUNE_SHADES; shade++) {
        sand_clear(&s);
        sand_set(&s, W / 2, H - 1, CELL_MAKE(MAT_SAND, shade));

        /* Whatever armed it is gone - a puddle that has since dried, or
         * the soil it was watering taken off the board. */
        s.may_have_moisture = true;
        sand_step(&s, 0, 1000, 0);

        char why[128];
        snprintf(why, sizeof why,
                 "a lone sand grain at shade %u kept the moisture pass "
                 "armed - its shade is being read as wetness", shade);
        TEST_ASSERT_FALSE_MESSAGE(s.may_have_moisture, why);
    }
}

static void test_placing_fire_arms_both_gas_and_fire_passes(void)
{
    fixture();

    /* Guards the else-if ordering bug found before this shipped: fire is
     * BOTH kind == KIND_GAS (needs sand_step_gas() to rise/disperse) AND
     * reactions[].burns (needs sand_step_reactions() to ignite/
     * extinguish/burn out) at once. sand_set()/try_spawn_one() used to
     * test these as an else-if chain with the kind check first, which
     * would shadow the burns check for every fire cell the moment fire's
     * kind became KIND_GAS - may_have_burning would silently never get
     * set, and a freshly painted fire spark would rise correctly
     * (kind-generic, unaffected) but never ignite, extinguish, or burn
     * out. No public getter for either flag - reading them directly is
     * intentional, mirroring test_pouring_stone_never_arms_the_reactions_pass's
     * own justified exception. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_spawn(&s, 3, 3, 0, MAT_FIRE),
        "setup: exactly one fire cell placed");
    TEST_ASSERT_TRUE_MESSAGE(s.may_have_gas,
        "a directly-placed fire cell must arm may_have_gas - it needs "
        "sand_step_gas() to rise and disperse");
    TEST_ASSERT_TRUE_MESSAGE(s.may_have_burning,
        "a directly-placed fire cell must ALSO arm may_have_burning - it "
        "needs sand_step_reactions() to ignite/extinguish/burn out, and "
        "an else-if chain that checks kind == KIND_GAS first would "
        "shadow this branch entirely");
}

/* --- wood, embers and steam ---------------------------------------------- */

/* Sets up a fire cell at (3,3) beside a wood cell at (4,3), boxed just
 * enough to keep the fire from smothering itself or drifting away
 * before reactions gets a turn.
 *
 * fire_room() (used by the plain-fire tests above) will not do here: it
 * seals a fire cell in on all four sides with stone, which works for a
 * GAS neighbour (density 10, lighter than fire's 15, so it never counts
 * towards smothered()'s ALL-of-4 test) but not for a WOOD one - wood's
 * density (150) is, unlike gas's, denser than fire, so a wood neighbour
 * on the one remaining open side completes the smother all by itself,
 * clearing the fire before it ever reaches the ignite loop (confirmed:
 * this is exactly what the first version of these two tests did). The
 * cell below fire is deliberately left open instead, mirroring
 * test_fire_is_not_smothered_with_a_gap's identical reasoning: gas only
 * ever rises or spreads sideways, never falls, so leaving it empty
 * cannot let fire drift away before this step's reactions pass runs,
 * and it is one fewer denser neighbour, so smothered() reads false. */
static void wood_ignition_room(void)
{
    fixture();
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, WOOD);
    sand_set(&s, 3, 2, STONE);
    sand_set(&s, 2, 2, STONE);
    sand_set(&s, 4, 2, STONE);
    sand_set(&s, 2, 3, STONE);
}

static void test_wood_does_not_catch_instantly(void)
{
    wood_ignition_room();

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WOOD, CELL_MATERIAL(sand_at(&s, 4, 3)),
        "at the real per-material flammability (6 in 256), wood touching "
        "fire for a single step must almost always still be wood - a fire "
        "that catches instantly defeats the whole point of a slow-burning "
        "fuel");
}

static void test_wood_eventually_catches_and_becomes_an_ember(void)
{
    wood_ignition_room();
    sand_set_flammability(&s, 255);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_TRUE_MESSAGE(cell_is_burning(sand_at(&s, 4, 3)),
        "wood forced to catch (flammability=255) must char into an "
        "ember, not flash straight to MAT_FIRE - fire is KIND_GAS and "
        "would float away on the very next gas pass, dissolving the log "
        "instead of letting it burn in place (see sand_reactions.c's top "
        "comment for the full reasoning)");
}

static void test_an_ember_does_not_rise(void)
{
    fixture();
    sand_set(&s, 3, H - 1, EMBER);

    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_TRUE_MESSAGE(cell_is_burning(sand_at(&s, 3, H - 1)),
        "a burning log is KIND_STATIC, unlike fire - it must stay exactly "
        "where it was placed rather than rising the way fire (KIND_GAS) "
        "does");
}

static void test_an_ember_burns_out_over_time(void)
{
    fixture();
    sand_set_decay(&s, 255);

    sand_set(&s, 3, 3, EMBER);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_count(&s),
        "setup: exactly one burning log placed");

    /* Twice an ember's own full life, not once: reaction_t.flare (no test
     * override exists for it - see the note on why smoke/flare get none)
     * can spawn a fresh MAT_FIRE cell on any step the ember is still
     * alive, and that fire cell gets its own full MATERIAL_VARIANTS-1
     * decay budget starting from whenever it was born - worst case, on
     * the ember's very last living step. This gives both budgets room to
     * run out even then, so the test is not at the mercy of exactly when
     * (if ever) the flare roll happens to hit. */
    for (int i = 0; i < 2 * (MATERIAL_VARIANTS - 1); i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_count(&s),
        "an ember, and anything it flared into fire along the way, must "
        "burn out to nothing given enough time - the same decay "
        "mechanism fire and gas already use");
}

static void test_an_ember_flares_fire_into_an_empty_neighbour(void)
{
    fixture();
    /* Decay left off (the default) - see material.c's own comment on
     * wood's burn_decay figure and test_an_ember_burns_out_over_time
     * above; this test wants an ember that lives long enough to get many
     * tries at the flare roll, not one racing its own burn-out. */
    sand_set(&s, 3, 3, EMBER);

    bool found_fire = false;
    for (int i = 0; i < 200 && !found_fire; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int y = 0; y < H && !found_fire; y++) {
            for (int x = 0; x < W; x++) {
                if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_FIRE) {
                    found_fire = true;
                }
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(found_fire,
        "an ember must eventually flare a MAT_FIRE cell into an empty "
        "neighbour - at 48 in 256 per step, 200 steps is comfortably "
        "enough that never seeing one means try_flare() is broken, not "
        "unlucky");
}

static void test_quenching_costs_the_water_a_unit_of_mass(void)
{
    fire_room(3, 4);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, CELL_MAKE(MAT_WATER, MASS_MAX));

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MASS_MAX - 1,
        CELL_VARIANT(sand_at(&s, 4, 3)),
        "the liquid neighbour that quenches a burning cell must lose "
        "exactly one unit of its own mass, not the whole cell - a fire "
        "should cost a pot a sip of water per step boiled, not a gulp");
}

/* MAT_FIRE.quench_to is MAT_STEAM (material.c) - water's own figure -
 * but step_one_burning_cell() substitutes the QUENCHING liquid's own
 * boils_to when that liquid is acid, and then, unlike water's clean
 * deterministic flash to steam, rolls twice more - see
 * SAND_ACID_QUENCH_RESIDUE_CHANCE / SAND_ACID_QUENCH_SMOKE_CHANCE's own
 * comment (sand.h) for why: whether anything is left behind at all, and
 * if so, gas or smoke (biased toward smoke). 2000 independent fire/acid
 * pairs, one step, counted - the same loose statistical-bias shape
 * test_the_dilution_split_favours_neither_side already uses for
 * SAND_ACID_DILUTE_TO_WATER_CHANCE, for the same reason: asserting on the
 * bias itself, not a single sample, and without hard-coding exact counts
 * a future retune of either constant would break. */
#define QUENCH_W 2000

/* cells is HEAP, not static file scope - each of the three callers below
 * mallocs its own QUENCH_W * 2 (4000 byte) grid and frees it before its
 * own assertions can fail; see drop_impulse_buf's own comment above for
 * why this file's static test fixtures cannot share the framebuffer's
 * memory budget. */
static void acid_quench_fixture(uint8_t *cells)
{
    sand_init(&fx.quench_sim, cells, QUENCH_W, 2, 11u);
    sand_set_mobility(&fx.quench_sim, 0);   /* keep fire from rising away
                                          * before reactions quenches it
                                          * this same step - same
                                          * technique
                                          * test_creating_steam_arms_the_gas_pass
                                          * already uses */
    for (int x = 0; x < QUENCH_W; x++) {
        sand_set(&fx.quench_sim, x, 0, FIRE);
        sand_set(&fx.quench_sim, x, 1, CELL_MAKE(MAT_ACID, MASS_MAX));
    }
}

static void test_acid_quenching_fire_never_leaves_steam(void)
{
    uint8_t *quench_cells = malloc((size_t)QUENCH_W * 2);
    TEST_ASSERT_NOT_NULL_MESSAGE(quench_cells,
        "acid-quench grid must fit in what the framebuffer leaves");
    acid_quench_fixture(quench_cells);
    sand_step(&fx.quench_sim, 0, 1000, 0);

    bool any_steam = false;
    for (int x = 0; x < QUENCH_W; x++) {
        if (CELL_MATERIAL(sand_at(&fx.quench_sim, x, 0)) == MAT_STEAM) {
            any_steam = true;
            break;
        }
    }

    /* Freed BEFORE the assertion: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(quench_cells);

    TEST_ASSERT_FALSE_MESSAGE(any_steam,
        "acid quenching fire must never leave MAT_STEAM behind - "
        "steam is water's own byproduct");
}

static void test_acid_quenching_fire_sometimes_leaves_nothing(void)
{
    uint8_t *quench_cells = malloc((size_t)QUENCH_W * 2);
    TEST_ASSERT_NOT_NULL_MESSAGE(quench_cells,
        "acid-quench grid must fit in what the framebuffer leaves");
    acid_quench_fixture(quench_cells);
    sand_step(&fx.quench_sim, 0, 1000, 0);

    int empty = 0;
    for (int x = 0; x < QUENCH_W; x++) {
        if (CELL_IS_EMPTY(sand_at(&fx.quench_sim, x, 0))) {
            empty++;
        }
    }

    /* Freed BEFORE the assertion: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(quench_cells);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, empty,
        "SAND_ACID_QUENCH_RESIDUE_CHANCE must be able to miss - some acid "
        "quenches must leave the flame simply out, with nothing behind, "
        "not a residue cell every single time");
}

static void test_acid_quenching_fire_favours_smoke_over_gas(void)
{
    uint8_t *quench_cells = malloc((size_t)QUENCH_W * 2);
    TEST_ASSERT_NOT_NULL_MESSAGE(quench_cells,
        "acid-quench grid must fit in what the framebuffer leaves");
    acid_quench_fixture(quench_cells);
    sand_step(&fx.quench_sim, 0, 1000, 0);

    int smoke = 0;
    int gas   = 0;
    for (int x = 0; x < QUENCH_W; x++) {
        const uint8_t m = CELL_MATERIAL(sand_at(&fx.quench_sim, x, 0));
        if (m == MAT_SMOKE) {
            smoke++;
        } else if (m == MAT_GAS) {
            gas++;
        }
    }

    /* Freed BEFORE the assertions: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(quench_cells);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, gas,
        "setup: acid quenching fire must sometimes leave gas too, not "
        "only smoke, or the bias below proves nothing");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(gas, smoke,
        "SAND_ACID_QUENCH_SMOKE_CHANCE must favour smoke over gas when "
        "acid quenches a flame");
}

static void test_steam_rises_and_disperses(void)
{
    fixture();
    sand_set_gas_walk(&s, false);   /* this straight-line guarantee is the
                                     * exhaustive mover's property now, not
                                     * KIND_GAS's in general */
    sand_set(&s, 3, H - 1, STEAM);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM,
        CELL_MATERIAL(sand_at(&s, 3, H - 2)),
        "with ordinary gravity pointing down, steam rises - the same "
        "KIND_GAS movement gas and fire already have");
}

static void test_creating_steam_arms_the_gas_pass(void)
{
    fixture();
    sand_set_gas_walk(&s, false);   /* the second half of this test checks
                                     * steam rises to an EXACT cell - the
                                     * exhaustive mover's guarantee, not
                                     * the walk's */
    sand_set_mobility(&s, 0);   /* keep fire from rising away before
                                 * reactions quenches it this same step -
                                 * mirrors test_fire_is_not_smothered_by_gas's
                                 * own use of this technique */
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 3, 4, WATER);
    /* Floor plus both down-diagonal neighbours, not the floor alone:
     * move_liquid_grain() tries down, then down-the-slope both ways, and
     * a mostly-empty cell (WATER here is CELL_MAKE(MAT_WATER, 8), not a
     * full MASS_MAX) can hand its ENTIRE mass to a single open diagonal
     * in one main-sweep call, draining (3,4) completely before
     * reactions ever gets a turn to check it for a liquid neighbour
     * (confirmed: this is exactly what the first version of this test,
     * with only the floor blocked, actually did). */
    sand_set(&s, 3, 5, STONE);
    sand_set(&s, 2, 5, STONE);
    sand_set(&s, 4, 5, STONE);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "setup: quenching must have produced steam, with no other gas "
        "anywhere on the grid that could accidentally arm may_have_gas "
        "some OTHER way and mask the bug this test exists to catch");

    sand_set_mobility(&s, 255);   /* steam's own turn to rise, forced
                                   * deterministic the same way every
                                   * other single-step gas-movement test
                                   * in this suite is */
    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 3, 2)),
        "steam created by place_reacted() must actually be able to rise "
        "on its very next chance - if may_have_gas was not latched for "
        "it, sand_step_gas() early-returns and the cell sits frozen on "
        "the grid forever, a bug this test's empty-grid setup is built "
        "specifically to catch (see place_reacted()'s own comment in "
        "sand_reactions.c)");
}

static void test_burnt_out_fire_can_leave_smoke(void)
{
    fixture();
    sand_set_decay(&s, 255);
    sand_set_mobility(&s, 0);   /* keep every fire cell pinned in place
                                 * rather than rising or spreading into
                                 * whatever gaps open up as neighbours
                                 * burn out around it - not required for
                                 * correctness (a drifting fire cell
                                 * still burns out and can still leave
                                 * smoke wherever it ends up), but it
                                 * keeps this deterministic rather than
                                 * merely probable */

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            sand_set(&s, x, y, FIRE);
        }
    }

    /* Checked after EVERY step, not just once at the end, and for a
     * reason worth spelling out: s->decay is a single override that
     * applies to every material at once (see sand_set_decay()), not
     * just fire, so a freshly created steam cell is JUST as forced to
     * decay away as the fire that made it - it does not get to sit
     * still and wait to be inspected. sand_set(FIRE) here (unlike
     * sand_spawn()) also skips random_cell()'s "fresh transient starts
     * at full life" rule, so these cells start at CELL_MAKE(..., 8), not
     * 15 - they burn out around step 8, not step 15. A loop that ran a
     * comfortable margin PAST that (as
     * test_an_ember_burns_out_over_time's does, deliberately, to give a
     * late flare room to also finish decaying) would just as
     * deliberately give any steam created around step 8 enough of that
     * same margin to fully decay away AGAIN before the check ever runs -
     * the opposite of comfortable here. Checking every step catches
     * steam the moment it exists, however long it goes on to live. */
    bool found_steam = false;
    for (int i = 0; i < 2 * (MATERIAL_VARIANTS - 1) && !found_steam; i++) {
        sand_step(&s, 0, 1000, 0);
        for (int y = 0; y < H && !found_steam; y++) {
            for (int x = 0; x < W; x++) {
                if (CELL_MATERIAL(sand_at(&s, x, y)) == MAT_SMOKE) {
                    found_steam = true;
                    break;
                }
            }
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(found_steam,
        "a whole grid of fire burning out at once (40 in 256 smoke "
        "chance per cell) must leave at least one MAT_SMOKE cell behind "
        "- not seeing a single one across 64 cells means smoke is "
        "broken, not unlucky");
}

/* The two byproducts must not be the same material, asserted directly
 * rather than left implied by the two tests either side of it.
 *
 * They WERE one material - MAT_STEAM did both jobs - and sharing the row
 * was defensible right up until you watched it: a fire dying in mid-air
 * with no water within reach puffing bright white kettle-steam reads as
 * a bug, because the player can see there was nothing there to boil.
 * This pins the split so nobody re-merges them on the entirely correct
 * observation that the two materials[] rows are nearly identical - the
 * difference that matters is the palette, not the physics. */
/* Relative luminance of a rendered cell, 0-255. The palette stores
 * panel-ready (byte-swapped) RGB565 - see gfx_color.h - so this undoes
 * both before weighting the channels the way an eye does. */
static int cell_luminance(cell_t c)
{
    const gfx_color_t p = material_palette()[c];
    const unsigned v = (unsigned)(((p & 0xFF) << 8) | ((p >> 8) & 0xFF));
    const int r = (int)((v >> 11) & 0x1F) * 255 / 31;
    const int g = (int)((v >> 5)  & 0x3F) * 255 / 63;
    const int b = (int)(v         & 0x1F) * 255 / 31;
    return (r * 30 + g * 59 + b * 11) / 100;
}

/* The palette test the two-material split exists for.
 *
 * MAT_STEAM and MAT_SMOKE are near-identical rows in materials[] - the
 * ONLY thing that makes them worth being two materials is that a player
 * can tell them apart on sight. That makes their palettes load-bearing
 * rather than decorative, which is unusual enough here to be worth
 * asserting: everything else in this suite tests behaviour, and a future
 * palette tweak that quietly collapsed these two back into the same
 * range would break the feature while passing every other test in the
 * file.
 *
 * Two separate properties, and the second is the one that is easy to
 * lose. Equal-life brightness ordering is the obvious one. Non-overlap
 * of the whole RANGES is the subtle one: a puff of smoke is caught at
 * whatever point in its life you happen to look at it, so "fresh smoke
 * is dimmer than dying steam" is what actually guarantees no cell is
 * ever ambiguous. The first draft of this palette had the first
 * property and not the second. */
static void test_steam_and_smoke_are_told_apart_by_brightness(void)
{
    int smallest_gap = 255;
    for (int life = 0; life < MATERIAL_VARIANTS; life++) {
        const int steam = cell_luminance(CELL_MAKE(MAT_STEAM, life));
        const int smoke = cell_luminance(CELL_MAKE(MAT_SMOKE, life));
        if (steam - smoke < smallest_gap) {
            smallest_gap = steam - smoke;
        }
    }

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(60, smallest_gap,
        "at equal life, steam must be clearly brighter than smoke at "
        "every one of the sixteen variants - measured at 89 when this "
        "was written, held to a looser 60 so ordinary palette tuning "
        "does not trip it but a collapse of the two ranges does");

    const int freshest_smoke = cell_luminance(CELL_MAKE(MAT_SMOKE,
                                                        MATERIAL_VARIANTS - 1));
    const int dying_steam    = cell_luminance(CELL_MAKE(MAT_STEAM, 1));

    TEST_ASSERT_LESS_THAN_INT_MESSAGE(dying_steam, freshest_smoke,
        "and the two ranges must not overlap AT ALL: the brightest smoke "
        "there is must still be dimmer than the faintest steam, or a puff "
        "caught at the wrong moment of its life is ambiguous - which "
        "defeats the entire reason these are two materials rather than "
        "one");
}

/* --- bubbles: gas rising through standing liquid ---------------------- */

/* water_column()/first_row_holding()/mass_held_by() live in
 * suite_sand_common.{c,h} - reused far past this section.
 *
 * The behaviour try_bubble() exists for, and the one this simulation could
 * not do at all before it.
 *
 * can_enter() only lets a DENSER mover displace a lighter target, and a
 * liquid never consults it anyway (room_in() refuses a cell holding any
 * other material). Between them, a steam cell under standing water had no
 * legal move in EITHER direction and sat frozen there forever - which is
 * exactly what it looked like on the device: a boiler that made steam and
 * then held onto it. */
/* --- oil and lava ----------------------------------------------------- */

/* A stone basin holding a pool of oil `depth` cells deep in columns
 * 2..5, with open air above it, and returns the row the surface sits on.
 * Open above on purpose, unlike fire_room(): these tests need a flame to
 * be able to sit ON the pool. */
static int oil_pool(int depth)
{
    fixture();
    sand_set_decay(&s, 0);
    const int floor = 6;
    for (int x = 1; x <= 6; x++) {
        sand_set(&s, x, floor, STONE);
    }
    for (int y = floor - depth; y < floor; y++) {
        sand_set(&s, 1, y, STONE);
        sand_set(&s, 6, y, STONE);
        for (int x = 2; x <= 5; x++) {
            sand_set(&s, x, y, OIL);
        }
    }
    return floor - depth;
}

/* The rule that makes a slick burn instead of detonate.
 *
 * Without reaction_t.needs_air, one spark lights every cell of a
 * connected pool inside a single pass - this file's reactions scan
 * propagates ignition through a whole pocket in one step (see
 * sand_reactions.c's top comment), which is a fuel-air bomb, not a
 * slick. With it, only cells touching air can catch, so the interior of
 * the pool is untouchable until the layer above it has burned off. */
static void test_only_the_exposed_surface_of_an_oil_pool_can_ignite(void)
{
    const int surface = oil_pool(4);
    sand_set(&s, 3, surface - 1, FIRE);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_OIL,
        CELL_MATERIAL(sand_at(&s, 3, surface + 2)),
        "an oil cell buried under more oil must NOT ignite, however "
        "much of the surface is alight - a pool burns off its top, it "
        "does not go up all at once");
}

/* The other half of needs_air, and the bug that shipped in the first
 * draft of it.
 *
 * "Exposed" originally meant "has an EMPTY cardinal neighbour", which is
 * wrong in the one situation that matters: a flame sitting on the pool is
 * not empty space, so the surface stopped counting as exposed at exactly
 * the moment it caught fire, and a slick with a fire blob parked on it
 * burned for thirty steps without ever lighting. Air has to include
 * gases. */
static void test_oil_ignites_with_a_flame_sitting_directly_on_it(void)
{
    const int surface = oil_pool(3);
    /* Cover the whole surface with fire, so no oil cell has any EMPTY
     * neighbour left at all - the exact case the first version failed. */
    for (int x = 2; x <= 5; x++) {
        sand_set(&s, x, surface - 1, FIRE);
    }

    /* Re-laid every step, because a single flame does not stay put long
     * enough to react: fire is KIND_GAS, so sand_step_gas() lifts it away
     * during the SAME step it was placed, before sand_step_reactions()
     * ever runs. Holding the fire brush down is exactly this, and it is
     * how the app produces the situation in the first place. */
    bool caught = false;
    for (int i = 0; i < 40 && !caught; i++) {
        for (int x = 2; x <= 5; x++) {
            if (CELL_IS_EMPTY(sand_at(&s, x, surface - 1))) {
                sand_set(&s, x, surface - 1, FIRE);
            }
        }
        sand_step(&s, 0, 1000, 0);
        caught = CELL_MATERIAL(sand_at(&s, 3, surface)) != MAT_OIL;
    }

    TEST_ASSERT_TRUE_MESSAGE(caught,
        "oil with a flame resting on it must catch - a fire neighbour "
        "is a KIND_GAS cell, not an empty one, and touches_air() has to "
        "count gases as air or the surface is declared unexposed "
        "precisely when it is on fire");
}

/* Oil is fuel, so it must not also be an extinguisher. neighbor_quenches()
 * runs before ignition and returns outright, so getting this wrong does
 * not merely weaken the effect - it inverts it, and oil becomes the best
 * fire suppressant in the simulation. */
static void test_oil_does_not_put_fire_out()
{
    fire_room(3, 4);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, OIL);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(MAT_EMPTY, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "a fire touching OIL must not be extinguished - only liquids "
        "that are neither fuel nor a heat source quench");
}

/* And water must still work exactly as it did, which is the thing the
 * new rule could most easily have broken. */
static void test_water_still_puts_fire_out(void)
{
    fire_room(3, 4);
    sand_set(&s, 3, 3, FIRE);
    sand_set(&s, 4, 3, WATER);

    sand_step(&s, 0, 1000, 0);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_STEAM, CELL_MATERIAL(sand_at(&s, 3, 3)),
        "water is neither fuel nor a heat source, so it must still "
        "quench on one touch - and still turn the fire to steam");
}

/* Two liquids of different densities have to sort themselves out, which
 * nothing before oil required. room_in() refuses a cell holding another
 * material and a liquid never consults can_enter(), so without
 * sink_through_lighter_liquid() the two simply block each other and oil
 * trapped under water stays there forever. */
/* Viscosity: liquids used to read no rate field at all, so every liquid
 * flowed at exactly the same speed and oil behaved like coloured water.
 * material.h's `mobility` is that rate - read by a gas as buoyancy and by
 * a liquid as viscosity inverted.
 *
 * Measured as steps for a tall column to reach the far wall: water 8,
 * oil 28 when this was written. Held to "oil takes at least twice as
 * long" so ordinary tuning does not trip it. */
/* Every liquid must DECLARE a mobility, checked in the table rather than
 * in motion.
 *
 * This exists because one did not. `mobility` arrived as a gas-only field
 * and only grew a liquid reader later, so lava's row - written before
 * that - left it unset, and an unset byte is zero.
 *
 * The obvious test is behavioural: place a column and assert it spreads.
 * That was written first and it does not work, which is worth recording.
 * A mobility of zero does not actually freeze a liquid, because (at the
 * time this was measured) the wall-rebound splash moved liquid without
 * consulting the gate, since removed (2026-08-30, see git history) - lava
 * at zero still crossed the same distance, in 249 steps against 20. Any
 * budget loose enough not to be flaky is loose enough to let that pass,
 * and any budget tight enough to catch it is pinning a performance figure
 * rather than an invariant.
 *
 * So this checks the table instead. Nonzero is not a claim about the
 * right value - only that somebody chose one, which is precisely the step
 * that got skipped. */
/* --- acid ---------------------------------------------------------------- */

/* count_cells_of()/acid_tank() live in suite_sand_common.{c,h} - reused far
 * past this section too. */

static void test_acid_dissolves_sand(void)
{
    acid_tank(2, 2);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, count_cells_of(MAT_SAND),
        "setup: there must be sand to eat");

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_cells_of(MAT_SAND),
        "acid must eat the sand it settles onto");
}

/* Glass is what acid can be kept in, and the only thing left that is.
 *
 * Stone held that role until acid learned to eat it. Moving the job to a
 * material you have to MAKE - sand plus sustained heat - is the point of
 * the change: acid is now dangerous to everything the level is built out
 * of, and a container is something you earn rather than something you
 * already had. */
static void test_acid_does_not_dissolve_its_container(void)
{
    acid_tank(2, 2);
    const int walls = count_cells_of(MAT_GLASS);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, walls,
        "setup: the tank must actually be made of glass");

    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(walls, count_cells_of(MAT_GLASS),
        "acid must not touch glass - it is the one material that resists, "
        "and therefore the only thing acid can be held in");
}

/* The other half, and the reason glass has a job at all. */
static void test_acid_eats_through_stone(void)
{
    fixture();
    sand_set_mobility(&s, SAND_MOBILITY_PER_MATERIAL);
    for (int x = 0; x < W; x++) {
        sand_set(&s, x, H - 1, GLASS);
    }
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 2, STONE);
    }
    const int before = count_cells_of(MAT_STONE);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, before, "setup: a stone floor");
    for (int x = 1; x < W - 1; x++) {
        sand_set(&s, x, H - 4, CELL_MAKE(MAT_ACID, MASS_MAX));
    }

    for (int i = 0; i < 600; i++) {
        sand_step(&s, 0, 1000, 0);
    }

    TEST_ASSERT_LESS_THAN_INT_MESSAGE(before, count_cells_of(MAT_STONE),
        "acid must eat into stone - stone stopped being the acid-proof "
        "material when glass took that role, and a stone wall that still "
        "held would leave glass with nothing to do");
}

void run_sand_combustion_suite(void)
{
    RUN_TEST(test_gas_rises_straight_up_under_ordinary_gravity);
    RUN_TEST(test_gas_falls_when_the_board_is_inverted);
    RUN_TEST(test_gas_rises_diagonally_under_tilted_gravity);
    RUN_TEST(test_gas_drifts_upward_under_ordinary_gravity);
    RUN_TEST(test_gas_drifts_downward_when_the_board_is_inverted);
    RUN_TEST(test_gas_drifts_against_tilted_gravity);
    RUN_TEST(test_gas_is_blocked_by_a_stone_ceiling);
    RUN_TEST(test_gas_disperses_across_a_ceiling);
    RUN_TEST(test_sand_sinks_through_gas);
    RUN_TEST(test_water_sinks_through_gas);
    RUN_TEST(test_gas_grain_count_is_conserved);
    RUN_TEST(test_rising_gas_wakes_the_blocks_it_passes_through);
    RUN_TEST(test_gas_scatter_can_be_disabled);
    RUN_TEST(test_gas_decays_and_disappears_over_time);
    RUN_TEST(test_gas_decaying_away_marks_its_row_dirty);
    RUN_TEST(test_tilted_equalise_still_spreads_a_packed_row_under_the_sight_bound);
    RUN_TEST(test_fire_ignites_an_adjacent_flammable_neighbour);
    RUN_TEST(test_a_confined_gas_pocket_bursts_instead_of_just_catching);
    RUN_TEST(test_an_open_gas_pocket_still_just_catches_fire);
    RUN_TEST(test_extinguishing_wins_over_igniting);
    RUN_TEST(test_fire_burns_out_and_disappears_over_time);
    RUN_TEST(test_fire_rises_and_disperses_like_gas);
    RUN_TEST(test_sand_sinks_through_fire);
    RUN_TEST(test_fire_is_smothered_when_fully_buried);
    RUN_TEST(test_fire_is_not_smothered_with_a_gap);
    RUN_TEST(test_fire_is_not_smothered_by_gas);
    RUN_TEST(test_liquid_wins_over_smothering);
    RUN_TEST(test_igniting_a_neighbour_marks_its_row_dirty);
    RUN_TEST(test_fire_burning_out_marks_its_row_dirty);
    RUN_TEST(test_fire_spreads_through_a_connected_pocket_in_one_step);
    RUN_TEST(test_pouring_stone_never_arms_the_reactions_pass);
    RUN_TEST(test_cold_conducts_deep_into_a_slab);
    RUN_TEST(test_snow_melts_on_wet_soil_but_not_on_dry);
    RUN_TEST(test_a_settled_snowbank_crusts_to_ice);
    RUN_TEST(test_a_snowbank_crusts_on_its_faces_and_thickens_slowly_inward);
    RUN_TEST(test_snow_does_not_crust_against_open_air);
    RUN_TEST(test_a_32_cell_snow_cover_turns_to_ice_in_about_three_minutes);
    RUN_TEST(test_a_fire_buried_on_all_four_sides_goes_out);
    RUN_TEST(test_a_material_created_during_the_pass_stays_in_the_mask);
    RUN_TEST(test_sand_alone_lets_the_moisture_pass_switch_off_again);
    RUN_TEST(test_placing_fire_arms_both_gas_and_fire_passes);
    RUN_TEST(test_wood_does_not_catch_instantly);
    RUN_TEST(test_wood_eventually_catches_and_becomes_an_ember);
    RUN_TEST(test_an_ember_does_not_rise);
    RUN_TEST(test_an_ember_burns_out_over_time);
    RUN_TEST(test_an_ember_flares_fire_into_an_empty_neighbour);
    RUN_TEST(test_quenching_costs_the_water_a_unit_of_mass);
    RUN_TEST(test_acid_quenching_fire_never_leaves_steam);
    RUN_TEST(test_acid_quenching_fire_sometimes_leaves_nothing);
    RUN_TEST(test_acid_quenching_fire_favours_smoke_over_gas);
    RUN_TEST(test_steam_rises_and_disperses);
    RUN_TEST(test_creating_steam_arms_the_gas_pass);
    RUN_TEST(test_burnt_out_fire_can_leave_smoke);
    RUN_TEST(test_steam_and_smoke_are_told_apart_by_brightness);
    RUN_TEST(test_only_the_exposed_surface_of_an_oil_pool_can_ignite);
    RUN_TEST(test_oil_ignites_with_a_flame_sitting_directly_on_it);
    RUN_TEST(test_water_still_puts_fire_out);
    RUN_TEST(test_oil_does_not_put_fire_out);
    RUN_TEST(test_acid_dissolves_sand);
    RUN_TEST(test_acid_does_not_dissolve_its_container);
    RUN_TEST(test_acid_eats_through_stone);
}

SUITE_REGISTER(run_sand_combustion_suite);
